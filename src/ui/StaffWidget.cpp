#include "ui/StaffWidget.h"

#include "score/BarlineModel.h"

#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>

#include <algorithm>

namespace fiddler::ui {

namespace {

// Visual layout constants. Pulled out as named constants so the
// numbers in the paint code mean something at a glance.
constexpr int kStaffLineCount      = 5;
constexpr int kStaffSpacingPx      = 8;     // gap between adjacent staff lines
constexpr int kStaffTopMarginPx    = 18;    // room above for a tune-type label
constexpr int kStaffBottomMarginPx = 12;    // unused for now; reserved for step 6 ledger lines

// Click-to-select tolerance in pixels — same value as WaveformWidget
// so the two widgets feel equally forgiving. Single-pixel barlines
// are too narrow to hit precisely with a mouse.
constexpr int kBarlineHitTolerancePx = 5;

constexpr int kTimeSigLeftMarginPx = 6;
constexpr int kTimeSigPointSize    = 14;

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
    if (selectedBarline_ == index) return;
    selectedBarline_ = index;
    update();
    emit barlineSelectionChanged(selectedBarline_);
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

    // First, try to hit-test a nearby barline. Tolerance is in
    // source-ms; we convert the pixel tolerance using the current
    // ms-per-pixel rate. The static_cast keeps the multiplication
    // 64-bit so very long files don't overflow.
    std::optional<std::size_t> hit;
    if (barlineModel_ && barlineModel_->size() > 0 && width() > 0) {
        const std::int64_t tolMs =
            static_cast<std::int64_t>(kBarlineHitTolerancePx)
                * durationMs_ / width();
        hit = barlineModel_->nearest(ms, tolMs);
    }

    if (hit.has_value()) {
        // Click landed on (or very near) a barline.
        if (selectedBarline_ != hit) {
            selectedBarline_ = hit;
            update();
            emit barlineSelectionChanged(selectedBarline_);
        }
        // Seek to the barline's *exact* ms — not the click's ms —
        // so the cursor lands on the tick the user just clicked.
        emit seekRequested(barlineModel_->barlines()[*hit]);
    } else {
        // Click landed in empty staff — clear any existing selection
        // and just seek to the clicked time.
        if (selectedBarline_.has_value()) {
            selectedBarline_.reset();
            update();
            emit barlineSelectionChanged(selectedBarline_);
        }
        emit seekRequested(ms);
    }
    event->accept();
}

void StaffWidget::keyPressEvent(QKeyEvent* event) {
    if (!barlineModel_) {
        QWidget::keyPressEvent(event);
        return;
    }
    const std::size_t count = barlineModel_->size();

    switch (event->key()) {
    case Qt::Key_Left:
        if (selectedBarline_.has_value() && *selectedBarline_ > 0) {
            setSelectedBarline(*selectedBarline_ - 1);
            emit seekRequested(
                barlineModel_->barlines()[*selectedBarline_]);
        }
        event->accept();
        return;

    case Qt::Key_Right:
        if (selectedBarline_.has_value()
            && *selectedBarline_ + 1 < count) {
            setSelectedBarline(*selectedBarline_ + 1);
            emit seekRequested(
                barlineModel_->barlines()[*selectedBarline_]);
        }
        event->accept();
        return;

    case Qt::Key_Escape:
        if (selectedBarline_.has_value()) {
            setSelectedBarline(std::nullopt);
        }
        event->accept();
        return;

    case Qt::Key_Delete:
        if (selectedBarline_.has_value()) {
            // Don't reset selection here. Once MainWindow turns this
            // signal into a model->removeAt() call, the model will
            // emit its own `changed()` signal and
            // onBarlineModelChanged() will drop the now-stale index.
            emit barlineDeleteRequested(*selectedBarline_);
        }
        event->accept();
        return;

    default:
        QWidget::keyPressEvent(event);
    }
}

} // namespace fiddler::ui
