// RingBuffer — single-producer / single-consumer lock-free ring of floats.
//
// Producer = decoder thread. Consumer = audio callback (real-time).
// The audio callback MUST NOT lock or allocate; this is the contract.
//
// Header-only because it's small, templated only on capacity, and we
// want the compiler to inline the hot path into the audio callback.

#pragma once

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
    }

private:
    std::vector<float>      buffer_;
    std::size_t             capacity_;
    std::atomic<std::size_t> readPos_{0};
    std::atomic<std::size_t> writePos_{0};
};

} // namespace fiddler::audio
