#include "ui/ScoreOverlayBase.h"

#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"

#include <QKeyEvent>
#include <QMouseEvent>

namespace fiddler::ui {

namespace {

// Click-to-select tolerance in pixels — used by mousePressEvent's
// hit test for barlines and markers. Same value the widgets used
// before the refactor (#12) so user feel is unchanged.
constexpr int kHitTolerancePx = 5;

} // namespace

ScoreOverlayBase::ScoreOverlayBase(QWidget* parent) : QWidget(parent) {
    // StrongFocus so the widget can receive arrow / Esc / Del keys
    // after a click gives it focus.
    setFocusPolicy(Qt::StrongFocus);
}

ScoreOverlayBase::~ScoreOverlayBase() = default;

// ---- model attachment ----------------------------------------------------

void ScoreOverlayBase::setBarlineModel(
    std::shared_ptr<const score::BarlineModel> model)
{
    if (barlineModel_) {
        disconnect(barlineModel_.get(), nullptr, this, nullptr);
    }
    barlineModel_ = std::move(model);
    if (barlineModel_) {
        connect(barlineModel_.get(), &score::BarlineModel::changed,
                this, &ScoreOverlayBase::onBarlineModelChanged);
    }
    if (selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    update();
}

void ScoreOverlayBase::setMarkerModel(
    std::shared_ptr<const score::MarkerModel> model)
{
    if (markerModel_) {
        disconnect(markerModel_.get(), nullptr, this, nullptr);
    }
    markerModel_ = std::move(model);
    if (markerModel_) {
        connect(markerModel_.get(), &score::MarkerModel::changed,
                this, &ScoreOverlayBase::onMarkerModelChanged);
    }
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    update();
}

void ScoreOverlayBase::setLoopModel(
    std::shared_ptr<const score::LoopModel> model)
{
    if (loopModel_) {
        disconnect(loopModel_.get(), nullptr, this, nullptr);
    }
    loopModel_ = std::move(model);
    if (loopModel_) {
        connect(loopModel_.get(), &score::LoopModel::changed,
                this, &ScoreOverlayBase::onLoopModelChanged);
    }
    if (selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    update();
}

// ---- read-only state -----------------------------------------------------

std::optional<std::int64_t>
ScoreOverlayBase::primaryAnchorMs() const noexcept {
    if (selectedBarline_.has_value() && barlineModel_
        && *selectedBarline_ < barlineModel_->size())
    {
        return barlineModel_->barlines()[*selectedBarline_];
    }
    if (selectedMarkerId_.has_value() && markerModel_) {
        if (const auto idx = markerModel_->indexOf(*selectedMarkerId_)) {
            return markerModel_->markers()[*idx].sourceMs;
        }
    }
    return std::nullopt;
}

std::int64_t ScoreOverlayBase::pixelsToMs(int px) const noexcept {
    // MEMO: derive the pixel→ms scale by sampling xToMs at the
    // origin and at `px`. Using the virtual lets a future
    // zoom-aware subclass return a tighter ms-per-pixel value
    // automatically.
    return xToMs(px) - xToMs(0);
}

// ---- selection setters ---------------------------------------------------

void ScoreOverlayBase::setPositionMs(std::int64_t ms) {
    if (positionMs_ == ms) return;
    positionMs_ = ms;
    update();
}

void ScoreOverlayBase::setSelectedBarline(std::optional<std::size_t> index) {
    if (index.has_value() && barlineModel_
        && *index >= barlineModel_->size()) {
        index = std::nullopt;
    }
    // MEMO: cross-kind mutual exclusion — setting a barline selection
    // clears any active marker AND loop selection. The user wanted
    // "the selected artifact" to be a single concept (the project
    // viewer shows its properties), so even though all three
    // selection slots exist they're never simultaneously populated.
    if (index.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (index.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (selectedBarline_ == index) {
        // Repaint just in case the marker/loop-clear above changed
        // visible state. update() is a no-op when nothing moved.
        update();
        return;
    }
    selectedBarline_ = index;
    update();
    emit barlineSelectionChanged(selectedBarline_);
}

void ScoreOverlayBase::setSelectedMarkerId(std::optional<std::int64_t> id) {
    if (id.has_value() && markerModel_
        && !markerModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    if (id.has_value() && selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (id.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (selectedMarkerId_ == id) {
        update();
        return;
    }
    selectedMarkerId_ = id;
    update();
    emit markerSelectionChanged(selectedMarkerId_);
}

void ScoreOverlayBase::setSelectedLoopId(std::optional<std::int64_t> id) {
    if (id.has_value() && loopModel_
        && !loopModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    if (id.has_value() && selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (id.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (selectedLoopId_ == id) {
        update();
        return;
    }
    selectedLoopId_ = id;
    update();
    emit loopSelectionChanged(selectedLoopId_);
}

void ScoreOverlayBase::setSecondaryAnchorMs(std::optional<std::int64_t> ms) {
    if (secondaryAnchorMs_ == ms) return;
    secondaryAnchorMs_ = ms;
    update();
    emit secondaryAnchorChanged(secondaryAnchorMs_);
}

// ---- model-changed slots -------------------------------------------------

void ScoreOverlayBase::onBarlineModelChanged() {
    if (selectedBarline_.has_value() && barlineModel_
        && *selectedBarline_ >= barlineModel_->size()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    update();
}

void ScoreOverlayBase::onMarkerModelChanged() {
    // Position edits keep the same ID, so a selected marker
    // survives a re-sort; only an outright remove drops the
    // selection.
    if (selectedMarkerId_.has_value() && markerModel_
        && !markerModel_->indexOf(*selectedMarkerId_).has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    update();
}

void ScoreOverlayBase::onLoopModelChanged() {
    if (selectedLoopId_.has_value() && loopModel_
        && !loopModel_->indexOf(*selectedLoopId_).has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    update();
}

// ---- mouse + key handling ------------------------------------------------

void ScoreOverlayBase::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !hasContent()) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    const int x = event->pos().x();
    const auto ms = xToMs(x);
    const bool ctrlHeld =
        (event->modifiers() & Qt::ControlModifier) != 0;
    const auto tolMs = pixelsToMs(kHitTolerancePx);

    // MEMO: hit-test priority — markers FIRST (labelled and visually
    // atop barlines, so a click on a flag should select the marker),
    // then barlines, then plain seek.

    // MEMO: Ctrl+click semantics — "add as second anchor for loop
    // creation". The current primary's ms is captured into the
    // secondary slot before the new selection is installed; the
    // dashed tick at that ms persists until cleared. Plain click
    // clears the secondary slot as a side effect (the user is
    // starting a fresh selection).
    auto prepareClickStateChange = [&]() {
        if (ctrlHeld) {
            if (const auto primMs = primaryAnchorMs()) {
                setSecondaryAnchorMs(*primMs);
            }
        } else {
            setSecondaryAnchorMs(std::nullopt);
        }
    };

    // 1. Marker hit?
    if (markerModel_ && markerModel_->size() > 0 && tolMs >= 0) {
        if (const auto markerHit = markerModel_->nearest(ms, tolMs)) {
            const auto idx = markerModel_->indexOf(*markerHit);
            if (idx) {
                prepareClickStateChange();
                setSelectedMarkerId(*markerHit);
                emit seekRequested(
                    markerModel_->markers()[*idx].sourceMs);
                event->accept();
                return;
            }
        }
    }

    // 2. Barline hit?
    if (barlineModel_ && barlineModel_->size() > 0 && tolMs >= 0) {
        if (const auto barHit = barlineModel_->nearest(ms, tolMs)) {
            prepareClickStateChange();
            setSelectedBarline(*barHit);
            emit seekRequested(barlineModel_->barlines()[*barHit]);
            event->accept();
            return;
        }
    }

    // 3. No artifact hit. Ctrl+click on empty space is a no-op so
    // the user can't accidentally lose their second anchor by
    // missing a tick. Plain click clears everything and seeks.
    if (ctrlHeld) {
        event->accept();
        return;
    }
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
    if (secondaryAnchorMs_.has_value()) {
        setSecondaryAnchorMs(std::nullopt);
    }
    emit seekRequested(ms);
    event->accept();
}

void ScoreOverlayBase::keyPressEvent(QKeyEvent* event) {
    // MEMO: arrow nav, Esc, and Del all operate on whichever
    // artifact kind is currently selected. A barline-selection
    // arrow moves between barlines; a marker-selection arrow moves
    // between markers. Esc clears whichever is set; Del fires the
    // matching delete signal.

    const bool haveBarline = static_cast<bool>(barlineModel_);
    const bool haveMarker  = static_cast<bool>(markerModel_);
    if (!haveBarline && !haveMarker) {
        QWidget::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
    case Qt::Key_Left:
        if (selectedBarline_ && *selectedBarline_ > 0) {
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
        if (selectedBarline_ && barlineModel_
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
        // Also drop any secondary anchor — Esc means "I'm done with
        // the in-flight selection state, including loop anchors".
        if (secondaryAnchorMs_.has_value()) {
            setSecondaryAnchorMs(std::nullopt);
        }
        event->accept();
        return;
    case Qt::Key_Delete:
        if (selectedBarline_.has_value()) {
            emit barlineDeleteRequested(*selectedBarline_);
            // Don't reset selection here — onBarlineModelChanged()
            // handles it once the model drops the entry.
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
