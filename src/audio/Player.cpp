#include "audio/Player.h"

#include <portaudio.h>

namespace fiddler::audio {

Player::Player() {
    Pa_Initialize(); // TODO step 1: check return code, log on failure.
}

Player::~Player() {
    unload();
    Pa_Terminate();
}

bool Player::load(const std::filesystem::path& path) {
    // TODO step 1:
    //   1. decoder_.open(path)
    //   2. allocate ring_ (e.g. 2 seconds @ 48 kHz stereo = 192k samples)
    //   3. Pa_OpenDefaultStream(&stream_, 0, channels, paFloat32,
    //                           sampleRate, framesPerBuffer,
    //                           &Player::paCallback, this)
    //   4. start decoder thread to keep the ring buffer fed
    (void)path;
    return false;
}

void Player::unload() {
    decoderRunning_.store(false);
    if (decoderThread_.joinable()) decoderThread_.join();
    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    decoder_.close();
    ring_.reset();
    state_.store(TransportState::Stopped);
    framesPlayed_.store(0);
}

void Player::play()  { /* TODO step 1: Pa_StartStream(stream_); state_ = Playing */ }
void Player::pause() { /* TODO step 1: Pa_StopStream(stream_);  state_ = Paused  */ }
void Player::stop()  { /* TODO step 1: stop + seek(0)                            */ }

void Player::seek(std::chrono::milliseconds position) {
    // TODO step 1: pause callback consumption, decoder_.seek(position),
    //              ring_->reset(), update framesPlayed_, resume.
    (void)position;
}

std::chrono::milliseconds Player::position() const noexcept {
    if (!decoder_.isOpen()) return std::chrono::milliseconds::zero();
    const auto sr = decoder_.outputFormat().sampleRate;
    if (sr <= 0) return std::chrono::milliseconds::zero();
    return std::chrono::milliseconds{
        framesPlayed_.load() * 1000 / sr};
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

    const std::size_t samplesNeeded = frameCount * self->decoder_.outputFormat().channels;
    std::size_t got = 0;
    if (self->ring_) {
        got = self->ring_->read({out, samplesNeeded});
    }
    // Pad with silence if underrun.
    for (std::size_t i = got; i < samplesNeeded; ++i) out[i] = 0.0f;

    self->framesPlayed_.fetch_add(static_cast<std::int64_t>(frameCount));
    return 0; // paContinue
}

void Player::decoderThread() {
    // TODO step 1: while running, if ring_->writeAvailable() > threshold,
    //              decode a chunk into a scratch buffer and ring_->write it.
}

} // namespace fiddler::audio
