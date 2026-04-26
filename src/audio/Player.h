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

    // Tempo as a fraction of the source rate. 1.0 = original speed,
    // 0.5 = half speed, etc. Clamped to [0.25, 1.0]. Pitch is
    // preserved across changes. Safe to call from the GUI thread; the
    // change is applied by the decoder thread on its next iteration.
    void setTempoRatio(double ratio);
    [[nodiscard]] double tempoRatio() const noexcept {
        return currentTempoRatio_.load();
    }

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

    // Tempo control. The GUI thread writes targetTempoRatio_; the
    // decoder thread observes it once per iteration, applies it to the
    // stretcher under mutex_, then updates currentTempoRatio_ together
    // with the position anchor below.
    std::atomic<double>        targetTempoRatio_{1.0};
    std::atomic<double>        currentTempoRatio_{1.0};

    // Position-tracking anchor (Rule 8: simple-first). position() is
    //   anchorSourceMs_ + (framesPlayed_ - anchorOutFrames_)
    //                     / sr * 1000 * currentTempoRatio_
    // The anchor is moved on every seek and tempo change so the
    // formula stays correct piecewise. While the ring drains output
    // produced under an older ratio, position() drifts by at most
    // ~2 s of source time, then re-converges.
    std::atomic<std::int64_t>  anchorSourceMs_{0};
    std::atomic<std::int64_t>  anchorOutFrames_{0};
};

} // namespace fiddler::audio
