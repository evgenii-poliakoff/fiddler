#include "audio/Player.h"

#include "util/Log.h"

#include <portaudio.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace fiddler::audio {

using namespace std::chrono_literals;

Player::Player() {
    Pa_Initialize();
}

Player::~Player() {
    unload();
    Pa_Terminate();
}

bool Player::load(const std::filesystem::path& path) {
    unload();

    if (!decoder_.open(path)) {
        return false;
    }

    const auto fmt = decoder_.outputFormat();

    // Cache audio format as atomics so the GUI thread (position(),
    // duration()) and the audio callback (channels for the
    // samples-per-frame math) can read them without touching
    // decoder_. Set BEFORE the decoder thread starts; cleared in
    // unload() AFTER it joins. While the thread is running these
    // are read-only.
    sampleRate_.store(fmt.sampleRate);
    channels_.store(fmt.channels);
    durationMs_.store(decoder_.duration().count());

    // Two seconds of headroom: enough to absorb scheduler hiccups,
    // small enough to keep seek latency under ~100 ms.
    const std::size_t ringSize =
        static_cast<std::size_t>(2 * fmt.sampleRate * fmt.channels);
    ring_ = std::make_unique<RingBuffer>(ringSize);

    stretcher_ = std::make_unique<Stretcher>(fmt.sampleRate, fmt.channels);
    eofFlushed_ = false;

    // Try to acquire a PortAudio output stream. If the host has no
    // usable audio device (CI runners, broken audio config, headless
    // environments) we fall through with stream_ == nullptr — the
    // decoder, stretcher, ring buffer, and seek path all stay valid
    // for visualisation; play() will no-op.
    PaStreamParameters outParams{};
    outParams.device = Pa_GetDefaultOutputDevice();
    if (outParams.device == paNoDevice) {
        FLOG_WARN("player",
                  "Pa_GetDefaultOutputDevice returned paNoDevice — "
                  "loaded for visualisation only");
    } else {
        outParams.channelCount     = fmt.channels;
        outParams.sampleFormat     = paFloat32;
        outParams.suggestedLatency =
            Pa_GetDeviceInfo(outParams.device)->defaultLowOutputLatency;
        outParams.hostApiSpecificStreamInfo = nullptr;

        PaError err = Pa_OpenStream(&stream_,
            nullptr, &outParams,
            static_cast<double>(fmt.sampleRate),
            paFramesPerBufferUnspecified,
            paNoFlag, &Player::paCallback, this);
        if (err != paNoError) {
            FLOG_WARN("player",
                      "Pa_OpenStream failed: {} — loaded for visualisation only",
                      Pa_GetErrorText(err));
            stream_ = nullptr;
        }
    }

    framesPlayed_.store(0);
    targetTempoRatio_.store(1.0);
    currentTempoRatio_.store(1.0);
    anchorSourceMs_.store(0);
    anchorOutFrames_.store(0);
    pendingSeekMs_.store(kNoPendingSeek);
    state_.store(TransportState::Stopped);

    decoderRunning_.store(true, std::memory_order_release);
    decoderThread_ = std::thread([this] { decoderThread(); });

    FLOG_INFO("player", "loaded; ring={} samples ({} s at {} Hz x{}){}",
              ringSize,
              static_cast<double>(ringSize) / fmt.channels / fmt.sampleRate,
              fmt.sampleRate, fmt.channels,
              stream_ ? "" : " [no audio output]");
    return true;
}

void Player::unload() {
    // Stop the decoder thread BEFORE touching decoder_/stretcher_/
    // ring_, since it owns them while running. After join the GUI
    // thread can mutate them freely.
    decoderRunning_.store(false, std::memory_order_release);
    if (decoderThread_.joinable()) decoderThread_.join();

    if (stream_) {
        if (Pa_IsStreamActive(stream_) == 1) Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    decoder_.close();
    stretcher_.reset();
    ring_.reset();
    eofFlushed_ = false;
    state_.store(TransportState::Stopped);
    framesPlayed_.store(0);
    sampleRate_.store(0);
    channels_.store(0);
    durationMs_.store(0);
}

void Player::play() {
    if (!stream_)                                     return;
    if (sampleRate_.load() <= 0)                      return;
    if (state_.load() == TransportState::Playing)     return;

    // If we're sitting at EOF, restart from the beginning. Use the
    // cached duration atomic — decoder_ might be mid-read on the
    // decoder thread; we mustn't touch it from here.
    const auto durMs = durationMs_.load();
    if (durMs > 0 && position().count() >= durMs) {
        seek(0ms);
    }

    if (Pa_StartStream(stream_) == paNoError) {
        state_.store(TransportState::Playing);
        FLOG_DEBUG("player", "play (resume from {} ms)", position().count());
    } else {
        FLOG_ERROR("player", "Pa_StartStream failed");
    }
}

void Player::pause() {
    if (state_.load() != TransportState::Playing) return;
    if (stream_ && Pa_IsStreamActive(stream_) == 1) Pa_StopStream(stream_);
    state_.store(TransportState::Paused);
    FLOG_DEBUG("player", "pause at {} ms", position().count());
}

void Player::stop() {
    if (stream_ && Pa_IsStreamActive(stream_) == 1) Pa_StopStream(stream_);

    // Reset position to zero. The actual decoder.seek(0) + stretcher
    // reset + ring discard happen on the decoder thread via the
    // pendingSeekMs_ channel — same path as a normal seek, lock-free.
    // We also reset framesPlayed so position() reads 0 immediately.
    //
    // MEMO[#52 considered-and-rejected]: briefly tried "Stop
    // preserves position" (Audacity / Logic / GarageBand
    // convention). The change made Stop functionally identical
    // to Pause — Stop became redundant. Reverted; Stop's job is
    // "halt + rewind to 0", Pause's is "halt at current
    // position". The two gestures keep their distinct meaning.
    framesPlayed_.store(0);
    anchorSourceMs_.store(0);
    anchorOutFrames_.store(0);
    pendingSeekMs_.store(0);

    state_.store(TransportState::Stopped);
    FLOG_DEBUG("player", "stop");
}

void Player::seek(std::chrono::milliseconds position) {
    if (sampleRate_.load() <= 0) return;   // nothing loaded

    FLOG_DEBUG("player", "seek to {} ms", position.count());

    // MEMO[#40]: GUI side is purely atomic publish — no
    // Pa_StopStream / Pa_StartStream, no mutex. The audio stream
    // keeps running; the decoder thread picks up pendingSeekMs_,
    // calls decoder.seek + stretcher.reset, then
    // RingBuffer::publishDiscard(). The audio callback rides the
    // transient by silencing the stale samples in the ring (one
    // buffer worth max) before continuing with new-position audio.

    // Anchor the source position at the seek target so position()
    // returns the new value to GUI callers (cursor follow during
    // drag) before the decoder thread has caught up.
    anchorSourceMs_.store(position.count());
    anchorOutFrames_.store(framesPlayed_.load());

    pendingSeekMs_.store(position.count());
}

std::chrono::milliseconds Player::position() const noexcept {
    const auto sr = sampleRate_.load();
    if (sr <= 0) return std::chrono::milliseconds::zero();

    // Anchor model: the decoder thread updates the anchor whenever
    // the tempo changes; seek() updates it on the GUI side. position()
    // across a piece of constant-ratio playback is just
    //   anchor + (framesPlayed - anchorFrames) / sr * 1000 * ratio.
    const auto frames       = framesPlayed_.load();
    const auto anchorFrames = anchorOutFrames_.load();
    const auto anchorMs     = anchorSourceMs_.load();
    const auto ratio        = currentTempoRatio_.load();

    const double deltaSec = static_cast<double>(frames - anchorFrames) / sr;
    const auto deltaMs    = static_cast<std::int64_t>(
        deltaSec * 1000.0 * ratio);
    const auto pos = std::chrono::milliseconds{anchorMs + deltaMs};
    const auto dur = duration();
    return (dur.count() > 0 && pos > dur) ? dur : pos;
}

void Player::setTempoRatio(double ratio) {
    targetTempoRatio_.store(std::clamp(ratio, 0.25, 1.0));
}

std::chrono::milliseconds Player::duration() const noexcept {
    return std::chrono::milliseconds{durationMs_.load()};
}

int Player::paCallback(const void* /*input*/, void* output,
                       unsigned long frameCount,
                       const PaStreamCallbackTimeInfo* /*timeInfo*/,
                       unsigned long /*statusFlags*/,
                       void* userData) {
    // REALTIME — DO NOT LOG, ALLOCATE, OR LOCK FROM THIS FUNCTION.
    // The decoder thread or the GUI timer should surface diagnostics;
    // this thread only touches atomics and the ring buffer.
    auto* self = static_cast<Player*>(userData);
    auto* out  = static_cast<float*>(output);

    // Channels read from the atomic, not from decoder_ (which the
    // decoder thread owns during its lifetime).
    const int channels = self->channels_.load(std::memory_order_relaxed);
    if (channels <= 0) {
        // Loaded but not yet ready, or being torn down — silence.
        const std::size_t total =
            static_cast<std::size_t>(frameCount) * 2u;   // worst-case stereo
        for (std::size_t i = 0; i < total; ++i) out[i] = 0.0f;
        return paContinue;
    }

    const std::size_t samplesNeeded =
        static_cast<std::size_t>(frameCount)
        * static_cast<std::size_t>(channels);

    std::size_t written = 0;
    if (self->ring_) {
        // MEMO[#40]: flush any pending discard in a single shot.
        // The decoder publishes a discard threshold after a seek;
        // we drop ALL stale frames at once (not throttled to this
        // callback's size) so the silence after a seek is bounded
        // by decoder refill latency, not by ring occupancy /
        // playback rate. The dropped frames never reach the
        // speakers and are NOT counted toward framesPlayed_.
        (void) self->ring_->flushDiscardPending();

        // Read whatever real post-seek audio the decoder has
        // refilled into the ring. If it hasn't caught up yet,
        // got < samplesNeeded and we pad with silence below.
        const std::size_t got = self->ring_->read(
            {out, samplesNeeded});
        written = got;

        // framesPlayed_ counts only real frames consumed — the
        // padding silence below is intentionally NOT counted.
        self->framesPlayed_.fetch_add(
            static_cast<std::int64_t>(got / channels),
            std::memory_order_relaxed);
    }

    // Underrun / EOF padding.
    for (std::size_t i = written; i < samplesNeeded; ++i) out[i] = 0.0f;
    return paContinue;
}

void Player::decoderThread() {
    constexpr std::size_t chunkSamples = 4096; // 2048 stereo frames
    std::vector<float> inBuf(chunkSamples);
    std::vector<float> outBuf(chunkSamples);

    while (decoderRunning_.load(std::memory_order_acquire)) {
        bool didWork = false;
        // MEMO[#40]: no mutex. Decoder/stretcher/ring writer are
        // owned by this thread for its entire lifetime. The GUI
        // thread reads cached atomics (sampleRate_, channels_,
        // durationMs_) instead of touching decoder_; the callback
        // touches only the ring (lock-free SPSC) and channels_.
        if (decoder_.isOpen() && ring_ && stretcher_) {
            // 0a. Pending seek? Pull the latest value out and execute
            //     the heavy seek work here. The GUI may overwrite
            //     pendingSeekMs_ between our load and exchange — that's
            //     fine; we'll catch the newer value next iteration.
            const auto pending =
                pendingSeekMs_.exchange(kNoPendingSeek);
            if (pending != kNoPendingSeek) {
                decoder_.seek(std::chrono::milliseconds{pending});
                stretcher_->reset();
                // MEMO[#40]: publishDiscard, NOT ring_->reset.
                // reset() races with the audio callback's read.
                // publishDiscard atomically marks the ring's current
                // contents stale; the callback silences them on its
                // next pass via consumeDiscardPending.
                ring_->publishDiscard();
                eofFlushed_ = false;
                didWork = true;
            }

            // 0b. Tempo change since last iteration? Apply.
            const double target = targetTempoRatio_.load();
            const double currentRatio = currentTempoRatio_.load();
            if (target != currentRatio) {
                const auto sr = sampleRate_.load();
                if (sr > 0) {
                    const auto frames     = framesPlayed_.load();
                    const auto oldFrames  = anchorOutFrames_.load();
                    const auto oldAnchor  = anchorSourceMs_.load();
                    const double deltaSec =
                        static_cast<double>(frames - oldFrames) / sr;
                    const auto sourceMs = oldAnchor +
                        static_cast<std::int64_t>(
                            deltaSec * 1000.0 * currentRatio);
                    anchorSourceMs_.store(sourceMs);
                    anchorOutFrames_.store(frames);
                }
                currentTempoRatio_.store(target);
                stretcher_->setTimeRatio(1.0 / target);
                FLOG_DEBUG("player.thread",
                           "tempo {} (time ratio {})",
                           target, 1.0 / target);
            }

            // 1. Drain whatever the stretcher already has into the
            //    ring. Priority: don't let the ring starve the
            //    callback while the stretcher sits on output.
            while (ring_->writeAvailable() >= chunkSamples
                   && stretcher_->available() > 0) {
                const std::size_t got = stretcher_->retrieve(
                    {outBuf.data(), chunkSamples});
                if (got == 0) break;
                ring_->write({outBuf.data(), got});
                didWork = true;
            }

            // 2. If the ring still has space, feed the stretcher
            //    one chunk from the decoder (or flush its tail at
            //    clean EOF, exactly once).
            if (ring_->writeAvailable() >= chunkSamples && !eofFlushed_) {
                const auto got = decoder_.read({inBuf.data(), chunkSamples});
                if (got > 0) {
                    stretcher_->process(
                        {inBuf.data(), static_cast<std::size_t>(got)},
                        /*final=*/false);
                    didWork = true;
                    FLOG_TRACE("player.thread",
                               "in {} samples; stretcher avail={} frames; "
                               "ring r/w={}/{}",
                               got, stretcher_->available(),
                               ring_->readAvailable(),
                               ring_->writeAvailable());
                } else if (got == 0) {
                    // Clean EOF — flush the stretcher tail so the
                    // last preroll-delayed samples come out.
                    stretcher_->process({}, /*final=*/true);
                    eofFlushed_ = true;
                    didWork = true;
                    FLOG_DEBUG("player.thread",
                               "decoder EOF; flushed stretcher tail "
                               "({} frames pending)",
                               stretcher_->available());
                }
                // got < 0 → decoder error. Stop pushing until
                // seek/unload clears state.
            }
        }
        if (!didWork) std::this_thread::sleep_for(5ms);
    }
}

} // namespace fiddler::audio
