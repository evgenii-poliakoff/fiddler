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

        // Zero the deinterleaved scratch so we can use it as a source
        // of silence for the start-pad below without re-zeroing.
        for (int c = 0; c < channels; ++c) {
            std::fill(deinterleaved[c].begin(),
                      deinterleaved[c].end(), 0.0f);
        }
    }

    // MEMO[#40]: prime the engine after construct/reset so the next
    // process(real) produces output aligned with the real input
    // instead of a leading silence gap. Rubber Band's realtime
    // protocol: feed getPreferredStartPad() zeros at the start of
    // input, and trim getStartDelay() frames from the start of
    // output. We do the input pad here and remember the output trim
    // in pendingDiscardFrames; retrieve() drains it internally so
    // the caller (decoder thread) only ever sees aligned audio.
    void primeRealtime() {
        const std::size_t pad = rb.getPreferredStartPad();
        pendingDiscardFrames  = rb.getStartDelay();

        std::size_t pushed = 0;
        while (pushed < pad) {
            const std::size_t chunk =
                std::min(pad - pushed, kMaxProcessFrames);
            for (int c = 0; c < channels; ++c) {
                std::fill_n(deinterleaved[c].begin(), chunk, 0.0f);
            }
            rb.process(deinterleavedPtrs.data(), chunk, /*final=*/false);
            pushed += chunk;
        }
    }

    // Drain up to `frames` output frames from the engine into the
    // scratch buffer and throw them away. Returns the number
    // actually drained (may be less than requested if the engine
    // has no more output available right now).
    std::size_t drainAndDiscard(std::size_t frames) {
        std::size_t drained = 0;
        while (drained < frames) {
            const int avail = rb.available();
            if (avail <= 0) break;
            const std::size_t chunk = std::min({
                static_cast<std::size_t>(avail),
                frames - drained,
                kMaxProcessFrames });
            const std::size_t got =
                rb.retrieve(deinterleavedPtrs.data(), chunk);
            if (got == 0) break;
            drained += got;
        }
        return drained;
    }

    int                              channels;
    RubberBandStretcher              rb;
    std::vector<std::vector<float>>  deinterleaved;     // [ch][frame]
    std::vector<float*>              deinterleavedPtrs; // for rb's API

    // Output frames the next retrieve() must drop before returning
    // real audio to the caller. Set by primeRealtime() to the
    // engine's getStartDelay(); drained opportunistically by
    // available()/retrieve() as the engine produces output.
    std::size_t pendingDiscardFrames = 0;
};

Stretcher::Stretcher(int sampleRate, int channels)
    : impl_(std::make_unique<Impl>(sampleRate, channels)) {
    impl_->primeRealtime();
    FLOG_DEBUG("stretcher",
               "created at {} Hz x{} (engine=Finer, pitch-high-consistency); "
               "primed pad={} delay={}",
               sampleRate, channels,
               impl_->rb.getPreferredStartPad(),
               impl_->rb.getStartDelay());
}

Stretcher::~Stretcher() = default;

void Stretcher::setTimeRatio(double ratio) {
    impl_->rb.setTimeRatio(ratio);
    FLOG_DEBUG("stretcher", "time ratio = {}", ratio);
}

void Stretcher::reset() {
    impl_->rb.reset();
    impl_->primeRealtime();
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
    if (a <= 0) return 0;
    const std::size_t avail = static_cast<std::size_t>(a);
    // Pending start-delay frames don't count as "available real audio".
    return avail > impl_->pendingDiscardFrames
           ? avail - impl_->pendingDiscardFrames
           : 0u;
}

std::size_t Stretcher::retrieve(std::span<float> interleaved) {
    // MEMO[#40]: first, finish draining any pending start-delay
    // output left over from the prime. The engine produces these
    // proportionally to input, so we may consume some now and the
    // rest on later retrieve() calls. Real audio for the caller
    // begins only after pendingDiscardFrames hits zero.
    if (impl_->pendingDiscardFrames > 0) {
        impl_->pendingDiscardFrames -=
            impl_->drainAndDiscard(impl_->pendingDiscardFrames);
        if (impl_->pendingDiscardFrames > 0) return 0;
    }

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
