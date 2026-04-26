find_package(Catch2 3 REQUIRED)

add_executable(fiddler_tests
    test_decoder.cpp
    test_ringbuffer.cpp
)

target_include_directories(fiddler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(fiddler_tests PRIVATE
    Catch2::Catch2WithMain
    PkgConfig::FFMPEG
)

# Pull in the audio sources we want to test directly. (For step 0 these
# don't do much; this scaffolding is here so step 1 has somewhere to land.)
target_sources(fiddler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/audio/Decoder.cpp
)

include(Catch)
catch_discover_tests(fiddler_tests)
