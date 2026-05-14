#include "audio/ToneSynth.h"

#include "util/Log.h"

#include <portaudio.h>

#include <algorithm>
#include <cstring>

namespace fiddler::audio {

ToneSynth::ToneSynth() {
    // Pa_Initialize is idempotent (refcounted internally); Player's
    // constructor already calls it, but ToneSynth is a separate
    // object that may outlive Player so we call here too. Pa_Terminate
    // also refcounts.
    Pa_Initialize();
}

ToneSynth::~ToneSynth() {
    if (stream_) {
        if (Pa_IsStreamActive(stream_) == 1) Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    Pa_Terminate();
}

bool ToneSynth::open() {
    if (stream_) return true;  // already open

    PaStreamParameters outParams{};
    outParams.device = Pa_GetDefaultOutputDevice();
    if (outParams.device == paNoDevice) {
        FLOG_WARN("synth",
                  "Pa_GetDefaultOutputDevice returned paNoDevice — "
                  "tone synth has no output");
        return false;
    }
    outParams.channelCount     = 2;            // stereo
    outParams.sampleFormat     = paFloat32;
    outParams.suggestedLatency =
        Pa_GetDeviceInfo(outParams.device)->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = nullptr;

    const double rate = 44100.0;
    PaError err = Pa_OpenStream(&stream_,
        nullptr, &outParams,
        rate,
        paFramesPerBufferUnspecified,
        paNoFlag, &ToneSynth::paCallback, this);
    if (err != paNoError) {
        FLOG_WARN("synth",
                  "Pa_OpenStream failed: {} — tone synth has no output",
                  Pa_GetErrorText(err));
        stream_ = nullptr;
        return false;
    }
    sampleRate_ = rate;
    referenceVoice_.setSampleRate(sampleRate_);

    if (Pa_StartStream(stream_) != paNoError) {
        FLOG_WARN("synth", "Pa_StartStream failed");
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        return false;
    }
    FLOG_INFO("synth", "opened tone-synth output {} Hz stereo", rate);
    return true;
}

// MEMO: gesture methods always mutate the Voice's state. On hosts
// without a PortAudio device the callback never fires (so nothing is
// audible) but the state still reflects what the user asked for,
// which is what renderForTest reads. Guards on `stream_` would
// silently drop test coverage — keep them out.

void ToneSynth::playContinuous(double freqHz, Waveform waveform) {
    referenceVoice_.noteOnContinuous(freqHz, waveform);
}

void ToneSynth::stop() {
    referenceVoice_.noteOff();
}

void ToneSynth::playPulse(double freqHz,
                          std::chrono::milliseconds duration,
                          Waveform waveform) {
    referenceVoice_.noteOnPulse(freqHz,
                                static_cast<int>(duration.count()),
                                waveform);
}

void ToneSynth::setVolume(float volume) {
    referenceVoice_.setVolume(volume);
}

float ToneSynth::volume() const noexcept {
    return referenceVoice_.volume();
}

void ToneSynth::renderForTest(float* output,
                              int    frameCount,
                              double sampleRate) {
    referenceVoice_.setSampleRate(sampleRate);
    // Caller-supplied buffer; we additively mix. Zero it first so
    // tests start from a clean state.
    std::memset(output, 0, sizeof(float) * static_cast<size_t>(frameCount));
    referenceVoice_.render(output, frameCount, sampleRate);
}

int ToneSynth::paCallback(const void* /*input*/, void* output,
                          unsigned long frameCount,
                          const PaStreamCallbackTimeInfo* /*timeInfo*/,
                          unsigned long /*statusFlags*/,
                          void* userData) {
    auto* self = static_cast<ToneSynth*>(userData);
    auto* out  = static_cast<float*>(output);

    // Step 6.3: render mono into scratch, duplicate to stereo. The
    // mono path is what Voice produces today; when chord playback
    // (polyphony) lands we'll sum N voices into the same scratch
    // before stereo duplication.
    unsigned long produced = 0;
    while (produced < frameCount) {
        const unsigned long chunk =
            std::min(frameCount - produced,
                     static_cast<unsigned long>(kCallbackScratchFrames));
        std::memset(self->callbackScratch_, 0, sizeof(float) * chunk);
        self->referenceVoice_.render(self->callbackScratch_,
                                     static_cast<int>(chunk),
                                     self->sampleRate_);
        for (unsigned long i = 0; i < chunk; ++i) {
            const float s = self->callbackScratch_[i];
            out[2 * (produced + i) + 0] = s;   // left
            out[2 * (produced + i) + 1] = s;   // right
        }
        produced += chunk;
    }
    return paContinue;
}

} // namespace fiddler::audio
