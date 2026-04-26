#include "audio/WaveformOverview.h"

#include "audio/Decoder.h"
#include "util/Log.h"

#include <algorithm>
#include <limits>

namespace fiddler::audio {

WaveformOverview WaveformOverview::fromSamples(
    std::span<const float> interleaved,
    int sampleRate,
    int channels,
    std::size_t buckets)
{
    WaveformOverview ov;
    ov.sampleRate_ = sampleRate;
    ov.channels_   = channels;

    if (channels <= 0 || sampleRate <= 0 || buckets == 0
        || interleaved.empty()) {
        return ov;
    }

    ov.totalFrames_ = static_cast<std::int64_t>(
        interleaved.size() / static_cast<std::size_t>(channels));
    if (ov.totalFrames_ == 0) {
        ov.channels_ = 0;
        return ov;
    }

    ov.bucketCount_ = std::min<std::size_t>(
        buckets, static_cast<std::size_t>(ov.totalFrames_));

    // Initialise peaks to "extreme so anything beats them"; we'll
    // overwrite as we scan. Empty buckets (impossible after the clamp
    // above) would otherwise show as ±inf, so we still seed with 0
    // for safety.
    PeakBucket seed{ std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity() };
    ov.peaks_.assign(static_cast<std::size_t>(channels) * ov.bucketCount_,
                     seed);

    const std::int64_t totalFrames = ov.totalFrames_;
    const auto         buckets64   = static_cast<std::int64_t>(ov.bucketCount_);
    const std::size_t  bucketSize  = ov.bucketCount_;

    for (std::int64_t f = 0; f < totalFrames; ++f) {
        // Bucket b spans frame range [b*total/buckets, (b+1)*total/buckets).
        const auto bucket = static_cast<std::size_t>(
            (f * buckets64) / totalFrames);
        for (int c = 0; c < channels; ++c) {
            const float v = interleaved[
                static_cast<std::size_t>(f) * static_cast<std::size_t>(channels)
                + static_cast<std::size_t>(c)];
            auto& pb = ov.peaks_[
                static_cast<std::size_t>(c) * bucketSize + bucket];
            if (v < pb.min) pb.min = v;
            if (v > pb.max) pb.max = v;
        }
    }

    // Replace any seed-pristine buckets (shouldn't happen after the
    // clamp, but cheap insurance) with a flat zero so downstream code
    // never sees infinities.
    for (auto& pb : ov.peaks_) {
        if (pb.min > pb.max) { pb.min = 0.0f; pb.max = 0.0f; }
    }

    return ov;
}

std::chrono::milliseconds WaveformOverview::duration() const noexcept {
    if (sampleRate_ <= 0) return std::chrono::milliseconds::zero();
    return std::chrono::milliseconds{ totalFrames_ * 1000 / sampleRate_ };
}

std::span<const PeakBucket> WaveformOverview::peaks(int channel) const {
    if (channel < 0 || channel >= channels_ || bucketCount_ == 0) {
        return {};
    }
    return std::span<const PeakBucket>{
        peaks_.data() + static_cast<std::size_t>(channel) * bucketCount_,
        bucketCount_
    };
}

std::chrono::milliseconds
WaveformOverview::bucketStartMs(std::size_t i) const noexcept {
    if (bucketCount_ == 0 || sampleRate_ <= 0) {
        return std::chrono::milliseconds::zero();
    }
    const std::int64_t startFrame =
        static_cast<std::int64_t>(i) * totalFrames_
        / static_cast<std::int64_t>(bucketCount_);
    return std::chrono::milliseconds{ startFrame * 1000 / sampleRate_ };
}

std::chrono::milliseconds
WaveformOverview::bucketEndMs(std::size_t i) const noexcept {
    return bucketStartMs(i + 1);
}

std::size_t
WaveformOverview::bucketAtMs(std::chrono::milliseconds ms) const noexcept {
    if (bucketCount_ == 0 || totalFrames_ == 0 || sampleRate_ <= 0) {
        return 0;
    }
    const std::int64_t frame = std::clamp<std::int64_t>(
        ms.count() * sampleRate_ / 1000, 0, totalFrames_ - 1);
    const auto bucket = static_cast<std::size_t>(
        frame * static_cast<std::int64_t>(bucketCount_) / totalFrames_);
    return std::min(bucket, bucketCount_ - 1);
}

std::optional<WaveformOverview>
buildOverview(Decoder& decoder, std::size_t buckets) {
    if (!decoder.isOpen() || buckets == 0) return std::nullopt;
    const auto fmt = decoder.outputFormat();
    if (fmt.channels <= 0 || fmt.sampleRate <= 0) return std::nullopt;

    // Read everything into memory. The chunk size is chosen to keep
    // FFmpeg's per-call overhead amortised without being so large
    // that we spend a lot of time copying into the growing vector.
    constexpr std::size_t kChunk = 65536;
    std::vector<float> all;
    std::vector<float> buf(kChunk);
    while (true) {
        const auto got = decoder.read({buf.data(), kChunk});
        if (got <= 0) break;
        all.insert(all.end(), buf.begin(),
                   buf.begin() + static_cast<std::ptrdiff_t>(got));
    }
    if (all.empty()) {
        FLOG_WARN("waveform", "decoder yielded zero samples; no overview built");
        return std::nullopt;
    }

    auto overview = WaveformOverview::fromSamples(
        all, fmt.sampleRate, fmt.channels, buckets);
    FLOG_DEBUG("waveform",
               "built overview: {} buckets, {} ch, {} ms, {} samples",
               overview.bucketCount(), overview.channels(),
               overview.duration().count(), all.size());
    return overview;
}

} // namespace fiddler::audio
