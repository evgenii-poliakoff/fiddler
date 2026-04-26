#include "audio/Decoder.h"

// FFmpeg headers are intentionally included only here, in the .cpp,
// so the public interface stays clean.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace fiddler::audio {

Decoder::Decoder()  = default;
Decoder::~Decoder() { close(); }

bool Decoder::open(const std::filesystem::path& path) {
    // TODO step 1:
    //   1. avformat_open_input + avformat_find_stream_info
    //   2. find best audio stream, open codec
    //   3. set up SwrContext to convert to interleaved float32, stereo,
    //      outFormat_.sampleRate
    //   4. populate streamIndex_, codecCtx_, swr_
    (void)path;
    lastError_ = "Decoder::open not yet implemented (step 1)";
    return false;
}

void Decoder::close() {
    if (swr_)       { swr_free(&swr_); }
    if (codecCtx_)  { avcodec_free_context(&codecCtx_); }
    if (formatCtx_) { avformat_close_input(&formatCtx_); }
    streamIndex_ = -1;
}

std::chrono::milliseconds Decoder::duration() const noexcept {
    if (!formatCtx_ || formatCtx_->duration <= 0) {
        return std::chrono::milliseconds::zero();
    }
    // AV_TIME_BASE is microseconds; convert to ms.
    return std::chrono::milliseconds{formatCtx_->duration / 1000};
}

std::ptrdiff_t Decoder::read(std::span<float> out) {
    // TODO step 1: pump packets through codecCtx_ and swr_ into `out`.
    (void)out;
    return 0;
}

bool Decoder::seek(std::chrono::milliseconds position) {
    // TODO step 1: av_seek_frame on streamIndex_, then avcodec_flush_buffers.
    (void)position;
    return false;
}

} // namespace fiddler::audio
