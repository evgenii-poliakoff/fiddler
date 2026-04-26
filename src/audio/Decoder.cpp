#include "audio/Decoder.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace fiddler::audio {

namespace {

// Pretty-print an FFmpeg error code into the lastError_ string.
std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

} // namespace

Decoder::Decoder() = default;

Decoder::~Decoder() { close(); }

bool Decoder::open(const std::filesystem::path& path) {
    close(); // idempotent — supports re-opening on the same instance

    int ret = avformat_open_input(&formatCtx_, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        lastError_ = "avformat_open_input failed: " + avErr(ret);
        return false;
    }

    ret = avformat_find_stream_info(formatCtx_, nullptr);
    if (ret < 0) {
        lastError_ = "avformat_find_stream_info failed: " + avErr(ret);
        close();
        return false;
    }

    const AVCodec* codec = nullptr;
    streamIndex_ = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_AUDIO,
                                       -1, -1, &codec, 0);
    if (streamIndex_ < 0 || !codec) {
        lastError_ = "no audio stream in file";
        close();
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        lastError_ = "avcodec_alloc_context3 failed";
        close();
        return false;
    }

    AVStream* stream = formatCtx_->streams[streamIndex_];
    ret = avcodec_parameters_to_context(codecCtx_, stream->codecpar);
    if (ret < 0) {
        lastError_ = "avcodec_parameters_to_context failed: " + avErr(ret);
        close();
        return false;
    }

    ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        lastError_ = "avcodec_open2 failed: " + avErr(ret);
        close();
        return false;
    }

    // Resampler: anything → interleaved float32 stereo at outFormat_.sampleRate.
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, outFormat_.channels);

    ret = swr_alloc_set_opts2(&swr_,
        &outLayout, AV_SAMPLE_FMT_FLT, outFormat_.sampleRate,
        &codecCtx_->ch_layout, codecCtx_->sample_fmt, codecCtx_->sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&outLayout);

    if (ret < 0 || !swr_) {
        lastError_ = "swr_alloc_set_opts2 failed: " + avErr(ret);
        close();
        return false;
    }

    ret = swr_init(swr_);
    if (ret < 0) {
        lastError_ = "swr_init failed: " + avErr(ret);
        close();
        return false;
    }

    pkt_   = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!pkt_ || !frame_) {
        lastError_ = "av_packet/frame_alloc failed";
        close();
        return false;
    }

    eofReached_     = false;
    resampleBuf_.clear();
    resampleBufPos_ = 0;
    lastError_.clear();
    return true;
}

void Decoder::close() {
    if (frame_)     { av_frame_free(&frame_); }
    if (pkt_)       { av_packet_free(&pkt_); }
    if (swr_)       { swr_free(&swr_); }
    if (codecCtx_)  { avcodec_free_context(&codecCtx_); }
    if (formatCtx_) { avformat_close_input(&formatCtx_); }
    streamIndex_   = -1;
    resampleBuf_.clear();
    resampleBufPos_ = 0;
    eofReached_    = false;
}

std::chrono::milliseconds Decoder::duration() const noexcept {
    if (!formatCtx_ || formatCtx_->duration <= 0) {
        return std::chrono::milliseconds::zero();
    }
    // formatCtx_->duration is in AV_TIME_BASE units (microseconds).
    return std::chrono::milliseconds{formatCtx_->duration / 1000};
}

// Convert one decoded frame through the resampler; append the result to
// resampleBuf_. Returns true on success, false on swr error.
static bool appendResampled(SwrContext* swr,
                            AVFrame* frame,
                            int outSampleRate,
                            int outChannels,
                            std::vector<float>& dst) {
    const int inRate = frame->sample_rate ? frame->sample_rate
                                          : outSampleRate;

    // swr_convert may need extra slack for filter delay.
    const int64_t maxOutSamples = av_rescale_rnd(
        swr_get_delay(swr, inRate) + frame->nb_samples,
        outSampleRate, inRate, AV_ROUND_UP);

    const std::size_t oldSize = dst.size();
    dst.resize(oldSize + maxOutSamples * outChannels);

    uint8_t* outPtr = reinterpret_cast<uint8_t*>(dst.data() + oldSize);
    const int actual = swr_convert(swr,
        &outPtr, static_cast<int>(maxOutSamples),
        const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);

    if (actual < 0) return false;
    dst.resize(oldSize + actual * outChannels);
    return true;
}

std::ptrdiff_t Decoder::read(std::span<float> out) {
    if (!isOpen())   return -1;
    if (out.empty()) return 0;

    std::size_t written = 0;

    auto drainBuffer = [&]() {
        const std::size_t avail = resampleBuf_.size() - resampleBufPos_;
        const std::size_t want  = out.size() - written;
        const std::size_t n     = std::min(avail, want);
        std::copy_n(resampleBuf_.begin() + resampleBufPos_, n,
                    out.begin() + written);
        resampleBufPos_ += n;
        written         += n;
        if (resampleBufPos_ >= resampleBuf_.size()) {
            resampleBuf_.clear();
            resampleBufPos_ = 0;
        }
    };

    drainBuffer();

    while (written < out.size()) {
        // Try to pull a decoded frame.
        int ret = avcodec_receive_frame(codecCtx_, frame_);

        if (ret == AVERROR(EAGAIN)) {
            // Codec wants more input. Read a packet from the container.
            if (eofReached_) {
                // Nothing left to send; flush already done.
                break;
            }
            ret = av_read_frame(formatCtx_, pkt_);
            if (ret == AVERROR_EOF) {
                avcodec_send_packet(codecCtx_, nullptr);  // drain mode
                eofReached_ = true;
            } else if (ret < 0) {
                lastError_ = "av_read_frame failed: " + avErr(ret);
                return -1;
            } else {
                if (pkt_->stream_index == streamIndex_) {
                    avcodec_send_packet(codecCtx_, pkt_);
                }
                av_packet_unref(pkt_);
            }
            continue;
        }

        if (ret == AVERROR_EOF) {
            // Codec fully drained. Flush the resampler too.
            uint8_t* outPtr = nullptr;
            const int extra = swr_get_out_samples(swr_, 0);
            if (extra > 0) {
                const std::size_t oldSize = resampleBuf_.size();
                resampleBuf_.resize(oldSize + extra * outFormat_.channels);
                outPtr = reinterpret_cast<uint8_t*>(resampleBuf_.data() + oldSize);
                const int actual = swr_convert(swr_, &outPtr, extra, nullptr, 0);
                if (actual >= 0) {
                    resampleBuf_.resize(oldSize + actual * outFormat_.channels);
                } else {
                    resampleBuf_.resize(oldSize);
                }
                drainBuffer();
            }
            break;
        }

        if (ret < 0) {
            lastError_ = "avcodec_receive_frame failed: " + avErr(ret);
            return -1;
        }

        // Got a frame — resample and drain.
        if (!appendResampled(swr_, frame_,
                             outFormat_.sampleRate, outFormat_.channels,
                             resampleBuf_)) {
            lastError_ = "swr_convert failed";
            av_frame_unref(frame_);
            return -1;
        }
        av_frame_unref(frame_);
        drainBuffer();
    }

    return static_cast<std::ptrdiff_t>(written);
}

bool Decoder::seek(std::chrono::milliseconds position) {
    if (!isOpen()) return false;

    AVStream* stream = formatCtx_->streams[streamIndex_];
    const int64_t targetUs = position.count() * 1000;
    const int64_t ts = av_rescale_q(targetUs, AV_TIME_BASE_Q, stream->time_base);

    int ret = av_seek_frame(formatCtx_, streamIndex_, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        lastError_ = "av_seek_frame failed: " + avErr(ret);
        return false;
    }

    avcodec_flush_buffers(codecCtx_);
    // The resampler holds filter-delay state that's now stale; drop it.
    if (swr_) {
        const int pending = swr_get_out_samples(swr_, 0);
        if (pending > 0) {
            swr_drop_output(swr_, pending);
        }
    }
    resampleBuf_.clear();
    resampleBufPos_ = 0;
    eofReached_     = false;
    return true;
}

} // namespace fiddler::audio
