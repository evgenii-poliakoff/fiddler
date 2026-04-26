find_package(Catch2 3 REQUIRED)

add_executable(fiddler_tests
    test_decoder.cpp
    test_decoder_synthetic.cpp
    test_decoder_corpus.cpp
    test_ringbuffer.cpp
)

target_include_directories(fiddler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Bake the corpus directory path in so tests work regardless of CWD.
target_compile_definitions(fiddler_tests PRIVATE
    FIDDLER_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/audio"
)

target_link_libraries(fiddler_tests PRIVATE
    Catch2::Catch2WithMain
    PkgConfig::FFMPEG
)

# Pull in the audio sources we want to test directly.
target_sources(fiddler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/audio/Decoder.cpp
)

include(Catch)
catch_discover_tests(fiddler_tests)
