add_executable(fiddler
    main.cpp
    ui/MainWindow.cpp
    ui/MainWindow.h
    audio/Decoder.cpp
    audio/Decoder.h
    audio/RingBuffer.h
    audio/Player.cpp
    audio/Player.h
    audio/Stretcher.cpp
    audio/Stretcher.h
    audio/WaveformOverview.cpp
    audio/WaveformOverview.h
    util/Log.cpp
    util/Log.h
)

target_include_directories(fiddler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fiddler PRIVATE
    Qt6::Widgets
    PkgConfig::FFMPEG
    PkgConfig::PORTAUDIO
    PkgConfig::RUBBERBAND
    PkgConfig::SPDLOG
)
