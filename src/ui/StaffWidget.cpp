#include "ui/StaffWidget.h"

#include "score/BarlineModel.h"
#include "score/MarkerModel.h"

#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QString>

#include <algorithm>

namespace fiddler::ui {

namespace {

// Visual layout constants. Pulled out as named constants so the
// numbers in the paint code mean something at a glance.
constexpr int kStaffLineCount      = 5;
constexpr int kStaffSpacingPx      = 8;     // gap between adjacent staff lines
constexpr int kStaffTopMarginPx    = 18;    // room above for tune-type label + marker flags
constexpr int kStaffBottomMarginPx = 12;    // unused for now; reserved for step 6 ledger lines

// Click-to-select tolerance in pixels — same value as WaveformWidget
// so the two widgets feel equally forgiving. Single-pixel ticks are
// too narrow to hit precisely with a mouse.
constexpr int kHitTolerancePx = 5;

constexpr int kTimeSigLeftMarginPx = 6;
constexpr int kTimeSigPointSize    = 14;

// Marker label flag — same shape as WaveformWidget's, sits in the
// top margin band above the staff lines.
constexpr int kMarkerFlagHeightPx     = 12;
constexpr int kMarkerFlagPaddingPx    = 4;
constexpr int kMarkerFlagFontPointSz  = 8;
constexpr int kMarkerFlagMaxWidthPx   = 120;

} // namespace

// ---- construction --------------------------------------------------------

StaffWidget::StaffWidget(QWidget* parent) : QWidget(parent) {
    // The widget paints its own background opaquely; tell Qt so it
    // skips the default-clear pass before paintEvent.
    setAttribute(Qt::WA_OpaquePaintEvent);

    // StrongFocus: a click on the widget gives it keyboard focus,
    // and Tab can also reach it. Without this the arrow / Esc / Del
    // keys handled by keyPressEvent would never arrive.
    setFocusPolicy(Qt::StrongFocus);
}

StaffWidget::~StaffWidget() = default;

// ---- public setters ------------------------------------------------------

void StaffWidget::setDurationMs(std::int64_t ms) {
    if (durationMs_ == ms) return;
    durationMs_ = ms;
    update();   // schedules a repaint; Qt batches and runs it on the event loop
}

void StaffWidget::setBarlineModel(
    std::shared_ptr<const score::BarlineModel> model)
{
    // Drop every connection between the previous model and this
    // widget before swapping. The 4-argument
    // `disconnect(sender, nullptr, this, nullptr)` form means
    // "disconnect any signal of `sender` from any slot of `this`",
    // which is exactly what we want here.
    if (barlineModel_) {
        disconnect(barlineModel_.get(), nullptr, this, nullptr);
    }
    barlineModel_ = std::move(model);
    if (barlineModel_) {
        // Subscribe to the model's `changed()` signal so the staff
        // repaints whenever a barline is added, removed, or the
        // time signature changes.
        connect(barlineModel_.get(), &score::BarlineModel::changed,
                this, &StaffWidget::onBarlineModelChanged);
    }
    // Any previously selected index belongs to the old model; drop it.
    if (selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    update();
}

void StaffWidget::setMarkerModel(
    std::shared_ptr<const score::MarkerModel> model)
{
    if (markerModel_) {
        disconnect(markerModel_.get(), nullptr, this, nullptr);
    }
    markerModel_ = std::move(model);
    if (markerModel_) {
        connect(markerModel_.get(), &score::MarkerModel::changed,
                this, &StaffWidget::onMarkerModelChanged);
    }
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    update();
}

void StaffWidget::setPositionMs(std::int64_t ms) {
    if (positionMs_ == ms) return;
    positionMs_ = ms;
    update();
}

void StaffWidget::setSelectedBarline(std::optional<std::size_t> index) {
    // Defensive: if a caller passes an index that's out of range
    // (e.g. they observed an older snapshot of the model), coerce
    // it to "no selection" rather than holding a dangling index.
    if (index.has_value() && barlineModel_
        && *index >= barlineModel_->size()) {
        index = std::nullopt;
    }
    // MEMO: mutual exclusion — see WaveformWidget's setSelectedBarline
    // for the rationale; both widgets enforce the same rule.
    if (index.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (selectedBarline_ == index) {
        update();
        return;
    }
    selectedBarline_ = index;
    update();
    emit barlineSelectionChanged(selectedBarline_);
}

void StaffWidget::setSelectedMarkerId(std::optional<std::int64_t> id) {
    if (id.has_value() && markerModel_
        && !markerModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    if (id.has_value() && selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (selectedMarkerId_ == id) {
        update();
        return;
    }
    selectedMarkerId_ = id;
    update();
    emit markerSelectionChanged(selectedMarkerId_);
}

void StaffWidget::onBarlineModelChanged() {
    // The selection's index might no longer be valid (the entry was
    // removed or the model was cleared). Drop it if so, then repaint
    // either way.
    if (selectedBarline_.has_value() && barlineModel_
        && *selectedBarline_ >= barlineModel_->size()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    update();
}

void StaffWidget::onMarkerModelChanged() {
    // Drop the marker selection only if the ID has truly gone away;
    // a mere re-position keeps the ID valid (just at a new index).
    if (selectedMarkerId_.has_value() && markerModel_
        && !markerModel_->indexOf(*selectedMarkerId_).has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    update();
}

// ---- coordinate transforms ----------------------------------------------

std::int64_t StaffWidget::xToMs(int x) const noexcept {
    if (durationMs_ <= 0 || width() <= 0) return 0;
    // 64-bit math throughout: long files and high-resolution widgets
    // would otherwise overflow a 32-bit multiply.
    const std::int64_t ms =
        static_cast<std::int64_t>(x) * durationMs_ / width();
    return std::clamp<std::int64_t>(ms, 0, durationMs_);
}

int StaffWidget::msToX(std::int64_t ms) const noexcept {
    if (durationMs_ <= 0 || width() <= 0) return 0;
    const std::int64_t x = ms * static_cast<std::int64_t>(width()) / durationMs_;
    return static_cast<int>(std::clamp<std::int64_t>(x, 0, width() - 1));
}

QSize StaffWidget::sizeHint()        const { return QSize(800, 100); }
QSize StaffWidget::minimumSizeHint() const { return QSize(120,  60); }

int StaffWidget::staffTopY()    const noexcept { return kStaffTopMarginPx; }
int StaffWidget::staffBottomY() const noexcept {
    return staffTopY() + (kStaffLineCount - 1) * kStaffSpacingPx;
}

// ---- painting ------------------------------------------------------------

void StaffWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(20, 20, 24));   // matches WaveformWidget

    // One paint helper per visual concern keeps each method short
    // and easy to follow. Order matters: things drawn later sit on
    // top, so the cursor is last.
    paintStaffLines(painter);
    paintTimeSignature(painter);
    paintBarlines(painter);
    paintMarkers(painter);
    paintCursor(painter);
}

void StaffWidget::paintStaffLines(QPainter& painter) const {
    painter.setPen(QPen(QColor(180, 180, 180), 1.0));
    const int top = staffTopY();
    for (int i = 0; i < kStaffLineCount; ++i) {
        const int y = top + i * kStaffSpacingPx;
        painter.drawLine(0, y, width(), y);
    }
}

void StaffWidget::paintTimeSignature(QPainter& painter) const {
    if (!barlineModel_) return;
    const auto ts = barlineModel_->timeSignature();

    // Numerator + denominator stacked, left of the first barline.
    // A future engraving pass would use proper SMuFL glyphs from a
    // music font; this is good enough for an empty-staff prototype.
    QFont digitFont = painter.font();
    digitFont.setPointSize(kTimeSigPointSize);
    digitFont.setBold(true);
    painter.setFont(digitFont);
    painter.setPen(QColor(230, 230, 230));

    const QFontMetrics fm(digitFont);
    const int top    = staffTopY();
    const int bottom = staffBottomY();
    // Vertical centres of the upper / lower halves of the staff.
    const int upperCentreY = top    + (bottom - top) / 4;
    const int lowerCentreY = bottom - (bottom - top) / 4;
    const int leftX        = kTimeSigLeftMarginPx;

    // QPainter::drawText takes the text *baseline* y, so we offset
    // by ~half the ascent to centre the glyph on the line we want.
    painter.drawText(leftX, upperCentreY + fm.ascent() / 2,
                     QString::number(ts.numerator));
    painter.drawText(leftX, lowerCentreY + fm.ascent() / 2,
                     QString::number(ts.denominator));

    // Optional tune-type label above the staff (handbook hint —
    // "Reel", "Jig", etc. — see project_handbook_for_self_taught.md).
    if (!ts.tuneType.isEmpty()) {
        QFont labelFont = painter.font();
        labelFont.setPointSize(10);
        labelFont.setBold(false);
        painter.setFont(labelFont);
        painter.drawText(leftX, top - 4, ts.tuneType);
    }
}

void StaffWidget::paintBarlines(QPainter& painter) const {
    if (!barlineModel_) return;

    const auto bars       = barlineModel_->barlines();
    const int  topY       = staffTopY();
    const int  bottomY    = staffBottomY();
    const QPen normalPen{ QColor(210, 170, 60), 1.0 };
    const QPen selectedPen{ QColor(255, 200, 90), 2.0 };

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const int x = msToX(bars[i]);
        if (x < 0 || x >= width()) continue;   // off-screen, skip

        const bool selected = (selectedBarline_ == i);
        painter.setPen(selected ? selectedPen : normalPen);
        painter.drawLine(x, topY, x, bottomY);
    }
}

void StaffWidget::paintMarkers(QPainter& painter) const {
    if (!markerModel_) return;
    const auto markers = markerModel_->markers();

    QFont flagFont = painter.font();
    flagFont.setPointSize(kMarkerFlagFontPointSz);
    flagFont.setBold(true);
    painter.setFont(flagFont);
    const QFontMetrics fm(flagFont);

    for (const auto& m : markers) {
        const int x = msToX(m.sourceMs);
        if (x < 0 || x >= width()) continue;
        const bool selected = (selectedMarkerId_ == m.id);
        const QColor lineCol = selected
            ? QColor(140, 230, 250)
            : QColor(100, 200, 220);

        // Marker tick: full height, like the staff barline.
        painter.setPen(QPen(lineCol, selected ? 2.0 : 1.0));
        painter.drawLine(x, 0, x, height());

        // Label flag in the top margin (sits above the staff lines).
        const int rawTextWidth = fm.horizontalAdvance(m.name);
        const int textWidth =
            std::min(rawTextWidth, kMarkerFlagMaxWidthPx
                     - 2 * kMarkerFlagPaddingPx);
        const int flagWidth = textWidth + 2 * kMarkerFlagPaddingPx;
        const QRect flagRect(x, 0, flagWidth, kMarkerFlagHeightPx);
        painter.fillRect(flagRect, lineCol.darker(140));
        painter.setPen(QColor(20, 20, 24));
        painter.drawText(flagRect.adjusted(kMarkerFlagPaddingPx, 0,
                                          -kMarkerFlagPaddingPx, 0),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         fm.elidedText(m.name, Qt::ElideRight, textWidth));
    }
}

void StaffWidget::paintCursor(QPainter& painter) const {
    const int cursorX = msToX(positionMs_);
    if (cursorX < 0 || cursorX >= width()) return;
    painter.setPen(QPen(QColor(255, 80, 80), 2.0));
    painter.drawLine(cursorX, 0, cursorX, height());
}

// ---- input ---------------------------------------------------------------

void StaffWidget::mousePressEvent(QMouseEvent* event) {
    // We only handle left-clicks; everything else (right-click for a
    // future context menu, middle-click) falls through to Qt's default.
    if (event->button() != Qt::LeftButton || durationMs_ <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();   // make the widget the keyboard target

    const int          clickX = event->pos().x();
    const std::int64_t ms     = xToMs(clickX);

    // MEMO: same hit-test priority as WaveformWidget — markers
    // first (labelled, visually above), then barlines, then plain
    // seek with both selections cleared.
    std::int64_t tolMs = 0;
    if (width() > 0) {
        tolMs = static_cast<std::int64_t>(kHitTolerancePx)
                * durationMs_ / width();
    }

    // 1. Marker hit?
    if (markerModel_ && markerModel_->size() > 0) {
        if (const auto markerHit = markerModel_->nearest(ms, tolMs)) {
            const auto idx = markerModel_->indexOf(*markerHit);
            if (idx) {
                setSelectedMarkerId(*markerHit);
                emit seekRequested(
                    markerModel_->markers()[*idx].sourceMs);
                event->accept();
                return;
            }
        }
    }

    // 2. Barline hit?
    if (barlineModel_ && barlineModel_->size() > 0) {
        if (const auto barHit = barlineModel_->nearest(ms, tolMs)) {
            setSelectedBarline(*barHit);
            emit seekRequested(barlineModel_->barlines()[*barHit]);
            event->accept();
            return;
        }
    }

    // 3. Plain seek — clear both selections.
    if (selectedBarline_.has_value()) {
        selectedBarline_.reset();
        update();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        update();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    emit seekRequested(ms);
    event->accept();
}

void StaffWidget::keyPressEvent(QKeyEvent* event) {
    const bool haveBarline = static_cast<bool>(barlineModel_);
    const bool haveMarker  = static_cast<bool>(markerModel_);
    if (!haveBarline && !haveMarker) {
        QWidget::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
    case Qt::Key_Left:
        if (selectedBarline_.has_value() && *selectedBarline_ > 0) {
            setSelectedBarline(*selectedBarline_ - 1);
            emit seekRequested(
                barlineModel_->barlines()[*selectedBarline_]);
        } else if (selectedMarkerId_ && markerModel_) {
            const auto idx = markerModel_->indexOf(*selectedMarkerId_);
            if (idx && *idx > 0) {
                if (const auto prevId = markerModel_->idAt(*idx - 1)) {
                    setSelectedMarkerId(*prevId);
                    const auto newIdx = markerModel_->indexOf(*prevId);
                    if (newIdx) {
                        emit seekRequested(
                            markerModel_->markers()[*newIdx].sourceMs);
                    }
                }
            }
        }
        event->accept();
        return;

    case Qt::Key_Right:
        if (selectedBarline_.has_value() && barlineModel_
            && *selectedBarline_ + 1 < barlineModel_->size())
        {
            setSelectedBarline(*selectedBarline_ + 1);
            emit seekRequested(
                barlineModel_->barlines()[*selectedBarline_]);
        } else if (selectedMarkerId_ && markerModel_) {
            const auto idx = markerModel_->indexOf(*selectedMarkerId_);
            if (idx && *idx + 1 < markerModel_->size()) {
                if (const auto nextId = markerModel_->idAt(*idx + 1)) {
                    setSelectedMarkerId(*nextId);
                    const auto newIdx = markerModel_->indexOf(*nextId);
                    if (newIdx) {
                        emit seekRequested(
                            markerModel_->markers()[*newIdx].sourceMs);
                    }
                }
            }
        }
        event->accept();
        return;

    case Qt::Key_Escape:
        if (selectedBarline_.has_value()) {
            setSelectedBarline(std::nullopt);
        } else if (selectedMarkerId_.has_value()) {
            setSelectedMarkerId(std::nullopt);
        }
        event->accept();
        return;

    case Qt::Key_Delete:
        if (selectedBarline_.has_value()) {
            emit barlineDeleteRequested(*selectedBarline_);
        } else if (selectedMarkerId_.has_value()) {
            emit markerDeleteRequested(*selectedMarkerId_);
        }
        event->accept();
        return;

    default:
        QWidget::keyPressEvent(event);
    }
}

} // namespace fiddler::ui
