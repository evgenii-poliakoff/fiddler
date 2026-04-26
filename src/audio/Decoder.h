// Decoder — opens any FFmpeg-supported audio container, decodes to a
// canonical interleaved float32 stereo stream at a fixed sample rate.
//
// Forward-declares FFmpeg structs so that public callers don't pull
// libavformat/libavcodec headers into their TUs.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

// FFmpeg forward declarations — at global namespace scope so they refer
// to the real ::AVFormatContext etc., not a fresh declaration inside
// fiddler::audio. Including libavformat/libavcodec headers from this
// public header would leak FFmpeg into every translation unit; we keep
// them confined to Decoder.cpp.
struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;

namespace fiddler::audio {

struct AudioFormat {
    int sampleRate = 48000;
    int channels   = 2;          // always 2 for step 1
};

class Decoder {
public:
    Decoder();
    ~Decoder();

    Decoder(const Decoder&)            = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Open `path` and prepare for decoding. Returns false on failure.
    [[nodiscard]] bool open(const std::filesystem::path& path);
    void close();

    [[nodiscard]] bool isOpen() const noexcept { return formatCtx_ != nullptr; }

    [[nodiscard]] AudioFormat outputFormat() const noexcept { return outFormat_; }

    // Total duration; zero if unknown.
    [[nodiscard]] std::chrono::milliseconds duration() const noexcept;

    // Pull at most `out.size()` interleaved float samples
    // (i.e. `out.size() / channels` frames).
    // Returns the number of float samples actually written.
    // Returns 0 on EOF; negative on error.
    std::ptrdiff_t read(std::span<float> out);

    // Seek to absolute position. Returns false on failure.
    bool seek(std::chrono::milliseconds position);

    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }

private:
    // Opaque FFmpeg state. Real types are pulled in by Decoder.cpp;
    // here we only need the global forward declarations above.
    AVFormatContext* formatCtx_  = nullptr;
    AVCodecContext*  codecCtx_   = nullptr;
    SwrContext*      swr_        = nullptr;
    int              streamIndex_ = -1;

    AudioFormat outFormat_{};
    std::string lastError_;
};

} // namespace fiddler::audio
