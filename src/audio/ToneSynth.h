// ToneSynth — public face of the audio-synthesis stack.
//
// Owns its own PortAudio output stream (separate from `Player`'s so
// a tone plays simultaneously with the recording playback — the OS
// audio router mixes both streams). Step 6.3 ships a SINGLE
// reference voice used by every gesture (click-piano, hover-tone,
// pulse-on-T). Future expansion adds a VoicePool driven by
// `scheduleNote(...)` for chord playback / transcription replay.
//
// Architecture (issues #step6.3 → future):
//
//      ToneSynth (this class)
//        ├── PortAudio stream
//        └── Voice referenceVoice_      ← Step 6.3
//            ├── SoundSource (Oscillator today; Sampler future)
//            └── Envelope
//
//   Future additions, NO BREAKING API CHANGES:
//        └── VoicePool       — for chord / transcription playback
//        └── Sampler-based   — real fiddle samples, pitch-shifted
//        └── Modulator       — vibrato / tremolo LFO inside Voice
//        └── Articulator     — translates ornament hints to envelope
//
// Thread model: GUI thread calls the public API; the PortAudio
// callback runs on a real-time audio thread and reads the
// referenceVoice_'s state. Voice is allocation-free; mutations
// from the GUI thread (noteOn / setFrequency / setVolume) are
// inherently racy with the callback, but they only write small
// scalar fields whose torn reads degrade gracefully (one frame of
// slightly-wrong gain / frequency). For Step 6.3 this is fine;
// future expansion may add explicit lock-free queues.

#pragma once

#include "audio/Oscillator.h"
#include "audio/Voice.h"

#include <chrono>

// PortAudio forward declarations at GLOBAL scope (see Player.h for
// the rationale — namespaced forwards confuse Pa_OpenStream's type
// matching).
typedef void PaStream;
struct PaStreamCallbackTimeInfo;

namespace fiddler::audio {

class ToneSynth {
public:
    ToneSynth();
    ~ToneSynth();

    ToneSynth(const ToneSynth&)            = delete;
    ToneSynth& operator=(const ToneSynth&) = delete;

    // Open the PortAudio stream. Returns true on success, false on
    // hosts where Pa_OpenStream fails (CI runners, headless). When
    // false, all play* methods become no-ops; the rest of the API
    // (volume, etc.) still works.
    bool open();

    [[nodiscard]] bool hasAudioOutput() const noexcept { return stream_ != nullptr; }

    // ---- Reference-tone gestures (Step 6.3) ----

    // Start (or retune) a steady tone. Subsequent calls within the
    // same continuous gesture retune without click. `stop()` ends it.
    void playContinuous(double freqHz, Waveform waveform = Waveform::Triangle);

    // End the steady tone with a short release fade.
    void stop();

    // Play a one-shot tone with attack/sustain/release envelope.
    // The total audible length is roughly `duration` + the envelope's
    // attack + release (~40 ms total). Independent of any continuous
    // tone in flight — pulses interrupt the steady tone.
    void playPulse(double freqHz,
                   std::chrono::milliseconds duration,
                   Waveform waveform = Waveform::Triangle);

    // 0.0 (silent) – 1.0 (full). Default 0.5.
    void setVolume(float volume);
    [[nodiscard]] float volume() const noexcept;

    // Test seam: render `frameCount` MONO samples into `output`
    // bypassing PortAudio. Used by the audio unit tests to verify
    // frequency / envelope without depending on a working device.
    void renderForTest(float* output, int frameCount, double sampleRate);

private:
    static int paCallback(const void* input, void* output,
                          unsigned long frameCount,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          unsigned long statusFlags,
                          void* userData);

    PaStream* stream_      = nullptr;
    double    sampleRate_  = 44100.0;
    Voice     referenceVoice_;

    // Scratch mono buffer for the callback to fill before duplicating
    // to stereo. Sized generously (4096 frames at 44.1 kHz ≈ 93 ms);
    // PortAudio rarely asks for more in a single callback.
    static constexpr int kCallbackScratchFrames = 4096;
    float callbackScratch_[kCallbackScratchFrames]{};
};

} // namespace fiddler::audio
