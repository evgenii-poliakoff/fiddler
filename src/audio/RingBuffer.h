// RingBuffer — single-producer / single-consumer lock-free ring of floats.
//
// Producer = decoder thread. Consumer = audio callback (real-time).
// The audio callback MUST NOT lock or allocate; this is the contract.
//
// Header-only because it's small, templated only on capacity, and we
// want the compiler to inline the hot path into the audio callback.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace fiddler::audio {

class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity)
        : buffer_(capacity)
        , capacity_(capacity) {}

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Number of samples currently readable.
    [[nodiscard]] std::size_t readAvailable() const noexcept {
        const auto w = writePos_.load(std::memory_order_acquire);
        const auto r = readPos_.load(std::memory_order_relaxed);
        return w - r;
    }

    [[nodiscard]] std::size_t writeAvailable() const noexcept {
        return capacity_ - readAvailable();
    }

    // Returns number of samples actually written.
    std::size_t write(std::span<const float> in) noexcept {
        const std::size_t avail = writeAvailable();
        const std::size_t n     = std::min(in.size(), avail);
        const auto w = writePos_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i) {
            buffer_[(w + i) % capacity_] = in[i];
        }
        writePos_.store(w + n, std::memory_order_release);
        return n;
    }

    // Returns number of samples actually read.
    std::size_t read(std::span<float> out) noexcept {
        const std::size_t avail = readAvailable();
        const std::size_t n     = std::min(out.size(), avail);
        const auto r = readPos_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = buffer_[(r + i) % capacity_];
        }
        readPos_.store(r + n, std::memory_order_release);
        return n;
    }

    void reset() noexcept {
        readPos_.store(0,  std::memory_order_relaxed);
        writePos_.store(0, std::memory_order_relaxed);
        discardThreshold_.store(0, std::memory_order_relaxed);
    }

    // ---- writer-side discard (issue #40) ------------------------------
    //
    // After the writer (decoder thread) has seeked the source and is
    // about to start producing audio from the new position, it calls
    // publishDiscard() to mark everything currently in the ring as
    // stale. The next reader pass silently consumes those samples
    // (outputting silence) instead of playing them, so the audio
    // callback rides a seek without needing Pa_StopStream /
    // Pa_StartStream around it.
    //
    // SPSC-safe: the writer reads its own writePos_ (no contention);
    // the reader reads discardThreshold_ via acquire (synchronises
    // with the writer's release here).
    void publishDiscard() noexcept {
        discardThreshold_.store(
            writePos_.load(std::memory_order_relaxed),
            std::memory_order_release);
    }

    // Reader-side counterpart. Advances readPos_ ALL THE WAY past
    // any pending discard in a single call, capped at writePos_ so
    // we never overshoot the writer. Returns the number of stale
    // frames dropped — the caller MUST NOT count them toward
    // framesPlayed_ (they never reached the speakers).
    //
    // MEMO[#40]: must drop the entire stale region at once, not
    // throttled to the current callback's output size. Throttling
    // makes the silence after a seek scale with ring occupancy:
    // a full 2-second ring took ~2 s to drain at PortAudio's
    // playback rate of one callback buffer per callback. The
    // SPSC invariant holds because only the reader writes
    // readPos_, even when the jump is large.
    std::size_t flushDiscardPending() noexcept {
        const auto thresh = discardThreshold_.load(std::memory_order_acquire);
        const auto r      = readPos_.load(std::memory_order_relaxed);
        if (r >= thresh) return 0;
        const auto w      = writePos_.load(std::memory_order_acquire);
        const auto target = std::min(thresh, w);
        if (r >= target) return 0;
        const auto skip   = target - r;
        readPos_.store(target, std::memory_order_release);
        return skip;
    }

private:
    std::vector<float>      buffer_;
    std::size_t             capacity_;
    std::atomic<std::size_t> readPos_{0};
    std::atomic<std::size_t> writePos_{0};
    // Writer publishes a snapshot of writePos_ here on every seek.
    // Reader silently drops everything with read-index < threshold.
    std::atomic<std::size_t> discardThreshold_{0};
};

} // namespace fiddler::audio
