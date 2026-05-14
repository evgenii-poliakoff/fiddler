// Envelope — amplitude shaper for a single Voice.
//
// State machine: Idle → Attack → Sustain → Release → Idle.
// Two activation modes:
//
//   • Continuous (Step 6.3): `noteOnContinuous()` runs attack →
//     sustains indefinitely → release fires only on `noteOff()`.
//   • Pulse      (Step 6.3): `noteOnPulse(durationFrames)` runs
//     attack → sustains for the prescribed time → release runs
//     automatically. No explicit noteOff needed.
//
// `nextSample()` returns a gain in [0, 1] for the current frame and
// advances state. `isActive()` returns false once the release fade
// completes — the Voice layer can then mark itself idle for the
// voice pool (future) to recycle.
//
// MEMO[#step6.3]: attack/release defaults are fixed (10 ms / 30 ms).
// Future articulation (#step7+) will accept profile arguments —
// staccato = short release, legato = no release between linked notes,
// portato = release ≈ attack — without changing the public API.

#pragma once

namespace fiddler::audio {

class Envelope {
public:
    void setSampleRate(double sampleRate) noexcept;

    // Start a continuous note: attack → sustain (indefinite) →
    // wait for noteOff.
    void noteOnContinuous() noexcept;

    // Start a pulse note: attack → sustain for sustainFrames →
    // release. After `attackFrames + sustainFrames + releaseFrames`
    // total frames, the envelope is back to Idle.
    void noteOnPulse(int sustainFrames) noexcept;

    // Force the envelope into Release (from any non-Idle state).
    // The current gain is held as the release-from value; from there
    // we ramp down linearly over `releaseFrames`.
    void noteOff() noexcept;

    [[nodiscard]] bool isActive() const noexcept { return state_ != State::Idle; }

    // Returns the gain for the current frame and advances state.
    [[nodiscard]] float nextSample() noexcept;

private:
    enum class State { Idle, Attack, Sustain, Release };

    void enterAttack() noexcept;
    void enterRelease() noexcept;

    double sampleRate_   = 44100.0;
    State  state_        = State::Idle;
    // Current gain (always written last by nextSample so noteOff
    // can hold the ramp-from value).
    float  gain_         = 0.0f;
    // Frame budgets configured at noteOn time so the envelope
    // adapts to the live sample rate without re-computing per call.
    int    attackFrames_  = 0;
    int    sustainFrames_ = 0;  // 0 in continuous mode → sustain held
    int    releaseFrames_ = 0;
    // Per-state progress counters.
    int    attackCounter_  = 0;
    int    sustainCounter_ = 0;
    int    releaseCounter_ = 0;
    // True for pulse mode (sustainFrames_ acts as a budget, after
    // which we enter Release automatically). False for continuous
    // (sustain stays until noteOff).
    bool   isPulse_        = false;
    // Gain captured at the moment noteOff fires — Release ramps
    // from this value to 0 so an interrupted note doesn't click.
    float  releaseFromGain_ = 0.0f;
};

} // namespace fiddler::audio
