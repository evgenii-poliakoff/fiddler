#include "audio/Player.h"

#include <portaudio.h>

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

    // Two seconds of headroom: enough to absorb scheduler hiccups,
    // small enough to keep seek latency under ~100 ms.
    const std::size_t ringSize =
        static_cast<std::size_t>(2 * fmt.sampleRate * fmt.channels);
    ring_ = std::make_unique<RingBuffer>(ringSize);

    PaStreamParameters outParams{};
    outParams.device = Pa_GetDefaultOutputDevice();
    if (outParams.device == paNoDevice) {
        decoder_.close();
        ring_.reset();
        return false;
    }
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
        decoder_.close();
        ring_.reset();
        return false;
    }

    framesPlayed_.store(0);
    state_.store(TransportState::Stopped);

    decoderRunning_.store(true, std::memory_order_release);
    decoderThread_ = std::thread([this] { decoderThread(); });
    return true;
}

void Player::unload() {
    decoderRunning_.store(false, std::memory_order_release);
    if (decoderThread_.joinable()) decoderThread_.join();

    if (stream_) {
        if (Pa_IsStreamActive(stream_) == 1) Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    decoder_.close();
    ring_.reset();
    state_.store(TransportState::Stopped);
    framesPlayed_.store(0);
}

void Player::play() {
    if (!stream_ || !decoder_.isOpen())               return;
    if (state_.load() == TransportState::Playing)     return;

    // If we're sitting at EOF, restart from the beginning.
    if (position() >= duration() && duration().count() > 0) {
        seek(0ms);
    }

    if (Pa_StartStream(stream_) == paNoError) {
        state_.store(TransportState::Playing);
    }
}

void Player::pause() {
    if (state_.load() != TransportState::Playing) return;
    if (stream_ && Pa_IsStreamActive(stream_) == 1) Pa_StopStream(stream_);
    state_.store(TransportState::Paused);
}

void Player::stop() {
    if (stream_ && Pa_IsStreamActive(stream_) == 1) Pa_StopStream(stream_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decoder_.seek(0ms);
        if (ring_) ring_->reset();
        framesPlayed_.store(0);
    }
    state_.store(TransportState::Stopped);
}

void Player::seek(std::chrono::milliseconds position) {
    if (!decoder_.isOpen()) return;

    const bool wasPlaying = (state_.load() == TransportState::Playing);
    if (wasPlaying && stream_ && Pa_IsStreamActive(stream_) == 1) {
        Pa_StopStream(stream_);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        decoder_.seek(position);
        if (ring_) ring_->reset();
        const auto sr = decoder_.outputFormat().sampleRate;
        if (sr > 0) {
            framesPlayed_.store(position.count() * sr / 1000);
        }
    }

    if (wasPlaying && stream_) {
        Pa_StartStream(stream_);
    }
}

std::chrono::milliseconds Player::position() const noexcept {
    if (!decoder_.isOpen()) return std::chrono::milliseconds::zero();
    const auto sr = decoder_.outputFormat().sampleRate;
    if (sr <= 0)            return std::chrono::milliseconds::zero();

    const auto pos = std::chrono::milliseconds{
        framesPlayed_.load() * 1000 / sr};
    const auto dur = duration();
    return (dur.count() > 0 && pos > dur) ? dur : pos;
}

std::chrono::milliseconds Player::duration() const noexcept {
    return decoder_.duration();
}

int Player::paCallback(const void* /*input*/, void* output,
                       unsigned long frameCount,
                       const PaStreamCallbackTimeInfo* /*timeInfo*/,
                       unsigned long /*statusFlags*/,
                       void* userData) {
    auto* self = static_cast<Player*>(userData);
    auto* out  = static_cast<float*>(output);

    const int channels = self->decoder_.outputFormat().channels;
    const std::size_t samplesNeeded =
        static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(channels);

    std::size_t got = 0;
    if (self->ring_) {
        got = self->ring_->read({out, samplesNeeded});
    }
    // Pad with silence on underrun / EOF.
    for (std::size_t i = got; i < samplesNeeded; ++i) out[i] = 0.0f;

    // Only count what we actually drained — keeps position() honest at EOF.
    self->framesPlayed_.fetch_add(
        static_cast<std::int64_t>(got / channels),
        std::memory_order_relaxed);
    return paContinue;
}

void Player::decoderThread() {
    constexpr std::size_t chunkSamples = 4096; // 2048 stereo frames
    std::vector<float> chunk(chunkSamples);

    while (decoderRunning_.load(std::memory_order_acquire)) {
        bool didWork = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (decoder_.isOpen() && ring_
                && ring_->writeAvailable() >= chunkSamples) {
                const auto got = decoder_.read({chunk.data(), chunkSamples});
                if (got > 0) {
                    ring_->write({chunk.data(),
                                  static_cast<std::size_t>(got)});
                    didWork = true;
                }
                // got == 0 → clean EOF; got < 0 → error. Either way, stop
                // pushing until something changes (seek / unload).
            }
        }
        if (!didWork) std::this_thread::sleep_for(5ms);
    }
}

} // namespace fiddler::audio
