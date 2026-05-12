# tools/screenshots — generates the user-manual's PNGs from the
# running app under offscreen Qt. Each scene is described in
# scenes.yaml; the C++ code is pure infrastructure (parser,
# action registry, capture runner). Adding a scene is a YAML
# edit, not a C++ edit.
#
# Output lands in docs/img/. Re-run after any UI change that
# affects a documented control.

add_executable(fiddler-screenshots
    main.cpp
    manifest.h
    manifest.cpp
    scene_actions.h
    scene_actions.cpp
    scene_runner.h
    scene_runner.cpp
    screenshot_fixture.h
    screenshot_fixture.cpp
)

target_include_directories(fiddler-screenshots PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

# Reach into the same UI / model / audio sources as the app so the
# tool can construct a real MainWindow with a real file loaded.
# Mirrors the test target's pattern (see tests/CMakeLists.txt).
target_sources(fiddler-screenshots PRIVATE
    ${CMAKE_SOURCE_DIR}/src/audio/Decoder.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/Player.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/Stretcher.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/WaveformOverview.cpp
    ${CMAKE_SOURCE_DIR}/src/score/BarlineModel.cpp
    ${CMAKE_SOURCE_DIR}/src/score/LoopModel.cpp
    ${CMAKE_SOURCE_DIR}/src/score/MarkerModel.cpp
    ${CMAKE_SOURCE_DIR}/src/score/NoteModel.cpp
    ${CMAKE_SOURCE_DIR}/src/score/Pitch.cpp
    ${CMAKE_SOURCE_DIR}/src/score/Serialize.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/LoopCountdownWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/MainWindow.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ProjectViewerDock.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ScoreOverlayBase.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/StaffWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/WaveformWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Log.cpp
)

target_link_libraries(fiddler-screenshots PRIVATE
    Qt6::Widgets
    Qt6::Test                 # QTest::keyClick for synthetic key events
                              # in the setup-action library
    PkgConfig::FFMPEG
    PkgConfig::PORTAUDIO
    PkgConfig::RUBBERBAND
    PkgConfig::SPDLOG
    PkgConfig::YAML_CPP
)

# Default paths baked at configure time so a bare run from anywhere
# does the right thing. CLI flags --output / --manifest override.
target_compile_definitions(fiddler-screenshots PRIVATE
    FIDDLER_SCREENSHOTS_DEFAULT_OUTPUT="${CMAKE_SOURCE_DIR}/docs/img"
    FIDDLER_SCREENSHOTS_DEFAULT_MANIFEST="${CMAKE_SOURCE_DIR}/tools/screenshots/scenes.yaml"
)
