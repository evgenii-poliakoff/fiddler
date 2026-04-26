// Player — owns the audio output stream and the decoder feeding it.
//
// Architecture:
//
//      [ Decoder ] --> [ Stretcher ] --> [ RingBuffer ] --(callback)--> [ PortAudio ]
//                  decoder thread                          (RT thread)
//
// The PortAudio callback runs on a real-time thread and only touches
// the ring buffer. The decoder thread reads from the decoder, feeds
// the Rubber Band stretcher, and pushes the stretched output into the
// ring buffer when there's space.

#pragma once

#include "audio/Decoder.h"
#include "audio/RingBuffer.h"
#include "audio/Stretcher.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

// Forward-declare PortAudio types so the header doesn't pull <portaudio.h>.
// IMPORTANT: declared at *global* scope. If we put `struct PaStreamCallbackTimeInfo;`
// inside namespace fiddler::audio, C++ treats it as a fresh declaration in
// that namespace and Pa_OpenStream rejects our callback as a different type.
typedef void PaStream;
struct PaStreamCallbackTimeInfo;

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
                          const PaStreamCallbackTimeInfo* timeInfo,
                          unsigned long statusFlags,
                          void* userData);

    void decoderThread();

    Decoder                    decoder_;
    std::unique_ptr<Stretcher> stretcher_;
    std::unique_ptr<RingBuffer> ring_;
    PaStream*                  stream_ = nullptr;

    // True once we've sent a final flush to the stretcher at EOF.
    // Protected by mutex_; reset on seek / load / unload.
    bool                       eofFlushed_ = false;

    // Protects decoder_ and ring_ against concurrent access by the
    // decoder thread and the GUI thread (during seek). The PortAudio
    // callback NEVER takes this mutex — it stays lock-free via the
    // ring buffer's atomic read/write positions.
    mutable std::mutex         mutex_;

    std::thread                decoderThread_;
    std::atomic<bool>          decoderRunning_{false};
    std::atomic<TransportState> state_{TransportState::Stopped};
    std::atomic<std::int64_t>  framesPlayed_{0};
};

} // namespace fiddler::audio
