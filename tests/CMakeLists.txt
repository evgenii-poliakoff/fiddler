find_package(Catch2 3 REQUIRED)

add_executable(fiddler_tests
    test_barline_model.cpp
    test_decoder.cpp
    test_decoder_synthetic.cpp
    test_decoder_corpus.cpp
    test_log.cpp
    test_main_window.cpp
    test_ringbuffer.cpp
    test_stretcher.cpp
    test_waveform_overview.cpp
    test_waveform_widget.cpp
    qt_test_app.cpp
    qt_test_app.h
)

target_include_directories(fiddler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Bake the corpus directory path in so tests work regardless of CWD.
target_compile_definitions(fiddler_tests PRIVATE
    FIDDLER_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/audio"
)

target_link_libraries(fiddler_tests PRIVATE
    # Catch2 (not Catch2WithMain) — qt_test_app.cpp owns main() so the
    # QApplication lives on the stack for the test session, avoiding
    # destruction-order issues with Qt's globals.
    Catch2::Catch2
    Qt6::Widgets
    Qt6::Test
    PkgConfig::FFMPEG
    PkgConfig::PORTAUDIO
    PkgConfig::RUBBERBAND
    PkgConfig::SPDLOG
)

# Pull in the source files we want to test directly. Qt-bearing
# sources (WaveformWidget, MainWindow) trigger AUTOMOC for the test
# target.
target_sources(fiddler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/audio/Decoder.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/Player.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/Stretcher.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/WaveformOverview.cpp
    ${CMAKE_SOURCE_DIR}/src/score/BarlineModel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/MainWindow.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/WaveformWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Log.cpp
)

include(Catch)
# Run every Catch2 test under the Qt "offscreen" platform plugin so
# the GUI-bearing tests work headless. Setting it via PROPERTIES gets
# the env var in place before the test process starts; relying solely
# on qputenv() from inside the process is too late — Qt resolves
# QT_QPA_PLATFORM during QApplication construction, and on a
# display-less host without the env var pre-set it tries xcb/wayland
# and segfaults on shutdown.
catch_discover_tests(fiddler_tests
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
