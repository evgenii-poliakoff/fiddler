#include "audio/RingBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using fiddler::audio::RingBuffer;

TEST_CASE("RingBuffer basic write/read", "[ringbuffer]") {
    RingBuffer rb(8);
    REQUIRE(rb.readAvailable()  == 0);
    REQUIRE(rb.writeAvailable() == 8);

    std::array<float, 4> in{1.f, 2.f, 3.f, 4.f};
    REQUIRE(rb.write(in) == 4);
    REQUIRE(rb.readAvailable() == 4);

    std::array<float, 4> out{};
    REQUIRE(rb.read(out) == 4);
    REQUIRE(out == in);
    REQUIRE(rb.readAvailable() == 0);
}

TEST_CASE("RingBuffer wraps around capacity", "[ringbuffer]") {
    RingBuffer rb(4);
    std::array<float, 3> a{1.f, 2.f, 3.f};
    std::array<float, 3> tmp{};

    REQUIRE(rb.write(a) == 3);
    REQUIRE(rb.read(tmp) == 3);

    std::array<float, 3> b{4.f, 5.f, 6.f};
    REQUIRE(rb.write(b) == 3); // wraps
    std::array<float, 3> out{};
    REQUIRE(rb.read(out) == 3);
    REQUIRE(out == b);
}

TEST_CASE("RingBuffer rejects overflow", "[ringbuffer]") {
    RingBuffer rb(4);
    std::array<float, 6> in{1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
    REQUIRE(rb.write(in) == 4); // only 4 fit
}

// ---- writer-side discard (#40) ----------------------------------------
//
// After the decoder seeks the source, it calls publishDiscard() and
// then keeps producing audio from the new position. The reader's
// flushDiscardPending() drops all stale frames in one shot — never
// at playback rate — so the post-seek silence is bounded by decoder
// refill, not by how full the ring was.

TEST_CASE("RingBuffer publishDiscard marks pre-publish samples stale",
          "[ringbuffer][discard]") {
    RingBuffer rb(16);

    std::array<float, 4> stale{1.f, 2.f, 3.f, 4.f};
    REQUIRE(rb.write(stale) == 4);

    rb.publishDiscard();

    // Reader drops all 4 stale frames in a single call.
    REQUIRE(rb.flushDiscardPending() == 4);
    REQUIRE(rb.readAvailable() == 0);

    // A second call is a no-op (threshold already consumed).
    REQUIRE(rb.flushDiscardPending() == 0);
}

TEST_CASE("RingBuffer post-publish writes survive the discard",
          "[ringbuffer][discard]") {
    RingBuffer rb(16);

    std::array<float, 3> pre{9.f, 9.f, 9.f};
    REQUIRE(rb.write(pre) == 3);

    rb.publishDiscard();

    std::array<float, 4> post{10.f, 11.f, 12.f, 13.f};
    REQUIRE(rb.write(post) == 4);

    // Reader drops the 3 pre-publish samples in one shot.
    REQUIRE(rb.flushDiscardPending() == 3);

    // The 4 post-publish samples remain readable, in order.
    std::array<float, 4> out{};
    REQUIRE(rb.read(out) == 4);
    REQUIRE(out == post);
}

TEST_CASE("RingBuffer flushDiscardPending drops everything stale in one call",
          "[ringbuffer][discard]") {
    RingBuffer rb(16);
    std::array<float, 8> stale{1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
    REQUIRE(rb.write(stale) == 8);
    rb.publishDiscard();

    // The pre-#40 throttled version would silence 3-frame batches;
    // the new contract drops the whole stale region at once.
    REQUIRE(rb.flushDiscardPending() == 8);
    REQUIRE(rb.readAvailable() == 0);
    REQUIRE(rb.flushDiscardPending() == 0);
}

TEST_CASE("RingBuffer reset clears pending discard",
          "[ringbuffer][discard]") {
    RingBuffer rb(8);
    std::array<float, 4> in{1.f, 2.f, 3.f, 4.f};
    rb.write(in);
    rb.publishDiscard();

    rb.reset();

    // After reset, fresh writes are immediately readable — the prior
    // discard threshold must not bleed through.
    std::array<float, 2> fresh{42.f, 43.f};
    REQUIRE(rb.write(fresh) == 2);
    REQUIRE(rb.flushDiscardPending() == 0);
    std::array<float, 2> out{};
    REQUIRE(rb.read(out) == 2);
    REQUIRE(out == fresh);
}
