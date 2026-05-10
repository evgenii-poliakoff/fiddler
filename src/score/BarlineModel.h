// score::BarlineModel — user-placed barlines + time signature.
//
// Data ownership: a single BarlineModel instance is owned by
// MainWindow and shared with the widgets that need to display or
// mutate it (WaveformWidget, StaffWidget, the future markers dock and
// loops controller). Both the waveform and the staff connect to the
// `changed()` signal and call update() to repaint.
//
// Architectural commitments worth preserving:
//
//   1. Barlines are *user-placed source-time anchors*, not derivations
//      from time-signature × tempo. Each entry is just an int64_t ms
//      position. There is no "expected next bar" calculation, no
//      uniformity enforcement, no snap-to-grid. This is what lets the
//      model handle non-uniform recording speed (rubato) and crooked
//      tunes (irregular bar lengths) honestly. See
//      `memory/project_crooked_tunes.md`.
//
//   2. `TimeSignature` is descriptive metadata, not prescriptive. It
//      labels the tune ("Reel (4/4)", "Jig (6/8)") for staff display
//      and for step 7's MusicXML / ABC export. It never constrains or
//      generates barline positions.
//
//   3. Undo lives in MainWindow — a tagged-variant LIFO covering
//      every kind of mutation across all three score models. The
//      models themselves expose only their direct mutators (add,
//      removeAt, clear); reversing those is MainWindow's job, with
//      the previous state captured inline at each push site. See
//      `src/ui/UndoEntry.h`.

#pragma once

#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fiddler::score {

struct TimeSignature {
    int     numerator   = 4;
    int     denominator = 4;
    // Traditional tune-type label ("Reel", "Jig", "Slip Jig", "Air",
    // …) — empty when the user picked "Custom" or "Other". Carried
    // through the model so step 7's exporter can emit it as a genre
    // marking without inferring from the numbers.
    QString tuneType;

    bool operator==(const TimeSignature& other) const noexcept = default;
};

class BarlineModel : public QObject {
    Q_OBJECT
public:
    explicit BarlineModel(QObject* parent = nullptr);

    [[nodiscard]] TimeSignature timeSignature() const noexcept { return timeSig_; }
    void setTimeSignature(TimeSignature ts);

    // Sorted ascending by sourceMs.
    [[nodiscard]] std::span<const std::int64_t> barlines() const noexcept {
        return { barlines_.data(), barlines_.size() };
    }
    [[nodiscard]] std::size_t size()  const noexcept { return barlines_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return barlines_.empty(); }

    // Insert a barline at sourceMs. Returns the (sorted) index it
    // was inserted at. If a barline already exists at exactly this
    // ms the insert is rejected and the existing index is returned;
    // either way, no duplicate is created.
    std::optional<std::size_t> add(std::int64_t sourceMs);

    // Remove the barline at `index`. Bounds-checked: out-of-range
    // calls are silently ignored.
    void removeAt(std::size_t index);

    // Remove all barlines. Time signature is preserved.
    void clear();

    // Index of the barline closest to sourceMs within toleranceMs,
    // or std::nullopt if none. Used by widgets for click-to-select
    // hit-testing.
    [[nodiscard]] std::optional<std::size_t>
        nearest(std::int64_t sourceMs,
                std::int64_t toleranceMs) const noexcept;

signals:
    void changed();

private:
    TimeSignature             timeSig_{};
    // Stored sorted ascending so widgets can scan in display order.
    std::vector<std::int64_t> barlines_;
};

} // namespace fiddler::score
