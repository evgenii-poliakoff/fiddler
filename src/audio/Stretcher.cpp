#include "audio/Stretcher.h"

#include "util/Log.h"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <vector>

namespace fiddler::audio {

namespace {

using RubberBand::RubberBandStretcher;

// R3 engine + realtime mode + pitch consistency across ratio changes.
// Formant preservation is intentionally off — it's a vocal feature; on
// fiddle audio it can subtly tint timbre, which we'd rather avoid.
constexpr int kEngineOptions =
      RubberBandStretcher::OptionProcessRealTime
    | RubberBandStretcher::OptionEngineFiner
    | RubberBandStretcher::OptionPitchHighConsistency;

// Maximum frames we'll push to rb.process() in a single call. The
// decoder thread feeds 2048-frame stereo chunks, so 16k is generous
// headroom. Setting it explicitly via setMaxProcessSize lets Rubber
// Band size its internal buffers up front and keeps process() free of
// allocation in steady state.
constexpr std::size_t kMaxProcessFrames = 16384;

} // namespace

struct Stretcher::Impl {
    Impl(int sampleRate, int channelCount)
        : channels(channelCount)
        , rb(static_cast<std::size_t>(sampleRate),
             static_cast<std::size_t>(channelCount),
             kEngineOptions,
             1.0,    // initial time ratio  (1:1)
             1.0)    // initial pitch scale (we never change pitch)
    {
        rb.setMaxProcessSize(kMaxProcessFrames);

        deinterleaved.assign(channels, std::vector<float>(kMaxProcessFrames));
        deinterleavedPtrs.resize(channels);
        for (int c = 0; c < channels; ++c) {
            deinterleavedPtrs[c] = deinterleaved[c].data();
        }
    }

    int                              channels;
    RubberBandStretcher              rb;
    std::vector<std::vector<float>>  deinterleaved;     // [ch][frame]
    std::vector<float*>              deinterleavedPtrs; // for rb's API
};

Stretcher::Stretcher(int sampleRate, int channels)
    : impl_(std::make_unique<Impl>(sampleRate, channels)) {
    FLOG_DEBUG("stretcher",
               "created at {} Hz x{} (engine=Finer, pitch-high-consistency)",
               sampleRate, channels);
}

Stretcher::~Stretcher() = default;

void Stretcher::setTimeRatio(double ratio) {
    impl_->rb.setTimeRatio(ratio);
    FLOG_DEBUG("stretcher", "time ratio = {}", ratio);
}

void Stretcher::reset() {
    impl_->rb.reset();
}

void Stretcher::process(std::span<const float> interleaved, bool final) {
    const int channels = impl_->channels;
    const std::size_t totalFrames = interleaved.size() / channels;

    // Empty + final → flush an empty final block so Rubber Band knows
    // the stream is over and can release its tail.
    if (totalFrames == 0) {
        if (final) {
            impl_->rb.process(impl_->deinterleavedPtrs.data(), 0, true);
        }
        return;
    }

    std::size_t offset = 0;
    while (offset < totalFrames) {
        const std::size_t chunk =
            std::min(totalFrames - offset, kMaxProcessFrames);
        for (std::size_t f = 0; f < chunk; ++f) {
            for (int c = 0; c < channels; ++c) {
                impl_->deinterleaved[c][f] =
                    interleaved[(offset + f) * channels + c];
            }
        }
        const bool isFinal = final && (offset + chunk == totalFrames);
        impl_->rb.process(impl_->deinterleavedPtrs.data(), chunk, isFinal);
        offset += chunk;
    }
}

std::size_t Stretcher::available() const noexcept {
    const int a = impl_->rb.available();
    return a < 0 ? 0u : static_cast<std::size_t>(a);
}

std::size_t Stretcher::retrieve(std::span<float> interleaved) {
    const int channels = impl_->channels;
    const std::size_t maxFrames = interleaved.size() / channels;
    if (maxFrames == 0) return 0;

    std::size_t writtenFrames = 0;
    while (writtenFrames < maxFrames) {
        const int avail = impl_->rb.available();
        if (avail <= 0) break;

        const std::size_t chunk =
            std::min({static_cast<std::size_t>(avail),
                      maxFrames - writtenFrames,
                      kMaxProcessFrames});

        const std::size_t got =
            impl_->rb.retrieve(impl_->deinterleavedPtrs.data(), chunk);
        if (got == 0) break;

        for (std::size_t f = 0; f < got; ++f) {
            for (int c = 0; c < channels; ++c) {
                interleaved[(writtenFrames + f) * channels + c] =
                    impl_->deinterleaved[c][f];
            }
        }
        writtenFrames += got;
    }
    return writtenFrames * channels;
}

} // namespace fiddler::audio
