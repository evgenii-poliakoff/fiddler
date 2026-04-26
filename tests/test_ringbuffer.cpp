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
