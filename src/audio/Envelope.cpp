#include "audio/Envelope.h"

namespace fiddler::audio {

namespace {
// Fixed attack/release durations for Step 6.3 — feel chosen by ear
// during prototype testing. Articulation profiles in Step 7+ will
// pass these as parameters; today they're constants so the synth
// has a consistent feel across click / hover / pulse / continuous.
constexpr double kAttackMs  = 10.0;
constexpr double kReleaseMs = 30.0;
} // namespace

void Envelope::setSampleRate(double sampleRate) noexcept {
    if (sampleRate > 0.0) sampleRate_ = sampleRate;
}

void Envelope::enterAttack() noexcept {
    attackFrames_ = static_cast<int>(kAttackMs * sampleRate_ / 1000.0);
    if (attackFrames_ < 1) attackFrames_ = 1;
    releaseFrames_ = static_cast<int>(kReleaseMs * sampleRate_ / 1000.0);
    if (releaseFrames_ < 1) releaseFrames_ = 1;
    attackCounter_ = 0;
    state_         = State::Attack;
    gain_          = 0.0f;
}

void Envelope::enterRelease() noexcept {
    releaseCounter_  = 0;
    releaseFromGain_ = gain_;
    state_           = State::Release;
}

void Envelope::noteOnContinuous() noexcept {
    enterAttack();
    isPulse_       = false;
    sustainFrames_ = 0;
}

void Envelope::noteOnPulse(int sustainFrames) noexcept {
    enterAttack();
    isPulse_        = true;
    sustainFrames_  = sustainFrames > 0 ? sustainFrames : 0;
    sustainCounter_ = 0;
}

void Envelope::noteOff() noexcept {
    if (state_ == State::Idle || state_ == State::Release) return;
    enterRelease();
}

float Envelope::nextSample() noexcept {
    switch (state_) {
    case State::Idle:
        gain_ = 0.0f;
        return 0.0f;

    case State::Attack:
        // Linear ramp from 0 to 1 over attackFrames_. A linear
        // attack at 10 ms is short enough that any "click" is
        // imperceptible; a curve isn't worth the cost here.
        gain_ = static_cast<float>(attackCounter_)
              / static_cast<float>(attackFrames_);
        if (++attackCounter_ >= attackFrames_) {
            gain_  = 1.0f;
            state_ = State::Sustain;
        }
        return gain_;

    case State::Sustain:
        // Pulse mode counts down to auto-release; continuous mode
        // stays here until an explicit noteOff().
        gain_ = 1.0f;
        if (isPulse_) {
            if (++sustainCounter_ >= sustainFrames_) {
                enterRelease();
            }
        }
        return gain_;

    case State::Release:
        // Linear ramp from releaseFromGain_ down to 0.
        gain_ = releaseFromGain_
              * (1.0f - static_cast<float>(releaseCounter_)
                       / static_cast<float>(releaseFrames_));
        if (gain_ < 0.0f) gain_ = 0.0f;
        if (++releaseCounter_ >= releaseFrames_) {
            gain_  = 0.0f;
            state_ = State::Idle;
        }
        return gain_;
    }
    return 0.0f;
}

} // namespace fiddler::audio
