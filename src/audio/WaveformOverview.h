// WaveformOverview — downsampled peak summary of an audio file, used
// by the waveform widget (and any future view that needs to show peaks
// across the whole file: mini-strip, spectrogram, exporters, etc.).
//
// The data is keyed on *source-time* milliseconds: bucket index N
// covers a fixed slice of the source file, regardless of any
// time-stretching applied at playback. This keeps the overview
// alignable with the staff and any other source-time-keyed overlay
// added in later steps.
//
// Construction is two-stage so it's testable end-to-end without the
// FFmpeg machinery:
//   - WaveformOverview::fromSamples(...)  — pure function, fed
//     synthetic float buffers in tests.
//   - buildOverview(Decoder&, ...)        — convenience for production
//     callers; reads the whole file into memory and forwards to
//     fromSamples.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fiddler::audio {

class Decoder;

struct PeakBucket {
    float min = 0.0f;
    float max = 0.0f;
};

class WaveformOverview {
public:
    // Build directly from an already-decoded interleaved float buffer.
    // `interleaved.size()` must be a multiple of `channels`; trailing
    // partial frames are silently ignored. If the input has fewer
    // frames than `buckets`, the output bucketCount is clamped to the
    // number of input frames so every bucket contains at least one
    // sample.
    [[nodiscard]] static WaveformOverview fromSamples(
        std::span<const float> interleaved,
        int sampleRate,
        int channels,
        std::size_t buckets);

    [[nodiscard]] int sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int channels()    const noexcept { return channels_; }
    [[nodiscard]] std::size_t bucketCount() const noexcept { return bucketCount_; }
    [[nodiscard]] std::int64_t totalFrames() const noexcept { return totalFrames_; }
    [[nodiscard]] std::chrono::milliseconds duration() const noexcept;

    // Peaks for one channel. Returned span has size == bucketCount().
    // Channel index must be in [0, channels()).
    [[nodiscard]] std::span<const PeakBucket> peaks(int channel) const;

    // Source-time mapping. Inverse of bucketAtMs() up to bucket-width
    // quantisation.
    [[nodiscard]] std::chrono::milliseconds bucketStartMs(std::size_t i) const noexcept;
    [[nodiscard]] std::chrono::milliseconds bucketEndMs(std::size_t i)   const noexcept;
    [[nodiscard]] std::size_t bucketAtMs(std::chrono::milliseconds ms)   const noexcept;

private:
    WaveformOverview() = default;

    int                     sampleRate_  = 0;
    int                     channels_    = 0;
    std::size_t             bucketCount_ = 0;
    std::int64_t            totalFrames_ = 0;
    // Layout: peaks_[c * bucketCount_ + b] — channel-major so each
    // channel's peaks are contiguous and can be returned as a span.
    std::vector<PeakBucket> peaks_;
};

// Decode the entire file through `decoder` and build an overview at
// the requested bucket resolution. Returns std::nullopt if the
// decoder isn't open or yields zero samples.
//
// Memory budget: holds the whole decoded stream in RAM during the
// build (typical Irish trad-length files ≤ ~50 MB at 48 kHz stereo).
// If we ever target longer recordings, switch to a streaming
// bucket-fill — the public API stays the same.
[[nodiscard]] std::optional<WaveformOverview> buildOverview(
    Decoder& decoder, std::size_t buckets);

} // namespace fiddler::audio
