// Player — owns the audio output stream and the decoder feeding it.
//
// Architecture (target for step 1):
//
//      [ Decoder ] --(decoder thread)--> [ RingBuffer ] --(callback)--> [ PortAudio ]
//
// The PortAudio callback runs on a real-time thread and only touches
// the ring buffer. The decoder thread tops up the ring buffer when low.
// In step 2, a Rubber Band stretcher will be inserted between Decoder
// and RingBuffer.

#pragma once

#include "audio/Decoder.h"
#include "audio/RingBuffer.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

// Forward-declare PortAudio stream so the header doesn't pull <portaudio.h>.
typedef void PaStream;

namespace fiddler::audio {

enum class TransportState { Stopped, Playing, Paused };

class Player {
public:
    Player();
    ~Player();

    Player(const Player&)            = delete;
    Player& operator=(const Player&) = delete;

    [[nodiscard]] bool load(const std::filesystem::path& path);
    void unload();

    void play();
    void pause();
    void stop();
    void seek(std::chrono::milliseconds position);

    [[nodiscard]] TransportState state() const noexcept { return state_.load(); }
    [[nodiscard]] std::chrono::milliseconds position() const noexcept;
    [[nodiscard]] std::chrono::milliseconds duration() const noexcept;

private:
    static int paCallback(const void* input, void* output,
                          unsigned long frameCount,
                          const struct PaStreamCallbackTimeInfo* timeInfo,
                          unsigned long statusFlags,
                          void* userData);

    void decoderThread();

    Decoder                    decoder_;
    std::unique_ptr<RingBuffer> ring_;
    PaStream*                  stream_ = nullptr;

    std::thread                decoderThread_;
    std::atomic<bool>          decoderRunning_{false};
    std::atomic<TransportState> state_{TransportState::Stopped};
    std::atomic<std::int64_t>  framesPlayed_{0};
};

} // namespace fiddler::audio
