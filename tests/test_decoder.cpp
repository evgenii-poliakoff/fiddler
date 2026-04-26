// Stub test for the Decoder. Real coverage lands in step 1, where we'll
// drop a tiny canonical WAV in tests/data/ and assert the decoder
// produces the expected sample count and sample rate.

#include "audio/Decoder.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Decoder is closed on construction", "[decoder]") {
    fiddler::audio::Decoder d;
    REQUIRE_FALSE(d.isOpen());
}

TEST_CASE("Decoder reports failure for missing file", "[decoder]") {
    fiddler::audio::Decoder d;
    REQUIRE_FALSE(d.open("/does/not/exist.wav"));
    REQUIRE_FALSE(d.lastError().empty());
}
