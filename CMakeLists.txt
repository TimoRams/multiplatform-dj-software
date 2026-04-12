cmake_minimum_required(VERSION 3.22)

set(PROJECT_NAME "RamsbrockDJ")
project(${PROJECT_NAME} VERSION 0.0.1 LANGUAGES C CXX)

# Ensure Qt can be found when installed via pacman
list(APPEND CMAKE_PREFIX_PATH "/usr/lib/cmake" "/usr/local/lib/cmake")

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache compiler launcher for faster rebuilds")
endif()

# Qt setup
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Quick3D Sql)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
include(ProcessorCount)
ProcessorCount(_cpu_count)
if(_cpu_count GREATER 1)
    # Let Qt's autogen (moc/uic/rcc) run in parallel on multicore systems.
    set(CMAKE_AUTOGEN_PARALLEL ${_cpu_count})
endif()

# TagLib für Cover-Art-Extraction
find_package(PkgConfig REQUIRED)
pkg_check_modules(TAGLIB    REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(KEYFINDER REQUIRED IMPORTED_TARGET libkeyfinder)
pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

add_subdirectory(libs/JUCE)

# Ableton Link — header-only library for tempo/beat sync
# Disable tests and examples; we only need the Ableton::Link imported target.
set(LINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
include(libs/link/AbletonLinkConfig.cmake)

juce_add_console_app(${PROJECT_NAME}
    PRODUCT_NAME "RamsbrockDJ"
    COMPANY_NAME "Ramsbrock.net"
)

# Pack QML files into the binary under URI "DJSoftware"
qt_add_qml_module(${PROJECT_NAME}
    URI DJSoftware
    VERSION 1.0
    QML_FILES
        src/qml/main.qml
        src/qml/TopHeader.qml
        src/qml/EnlargedWaveform.qml
        src/qml/OverallWaveform.qml
        src/qml/SegmentBar.qml
        src/qml/DeckControl.qml
        src/qml/TrackInfoDisplay.qml
        src/qml/Library.qml
        src/qml/PerformancePads.qml
        src/qml/TurntableIndicator.qml
        src/qml/MixerSection.qml
        src/qml/SettingsWindow.qml
        src/qml/FxUnit.qml
        src/qml/FxBar.qml
)

target_sources(${PROJECT_NAME} PRIVATE
    src/main.cpp
    src/audio/DjEngine.cpp
    src/analysis/PhraseAnalyzer.h
    src/analysis/PhraseAnalyzer.cpp
    src/rendering/RgbWaveformItem.h
    src/rendering/RgbWaveformItem.cpp
    src/rendering/WaveformItem.cpp
    src/rendering/ScrollingWaveformItem.cpp
    src/rendering/WaveformAnalyzer.cpp
    src/rendering/WaveformCache.h
    src/rendering/WaveformCache.cpp
    src/domain/TrackData.h
    src/domain/TrackSegment.h
    src/library/LibraryManager.h
    src/library/LibraryManager.cpp
    src/library/CoverArtExtractor.h
    src/library/CoverArtExtractor.cpp
    src/library/CoverArtProvider.h
    src/fx/FxManager.h
    src/fx/FxManager.cpp
    src/fx/FxProcessor.h
    src/fx/FxProcessor.cpp
    src/link/LinkManager.h
    src/link/LinkManager.cpp
    src/app/SystemMonitor.h
    src/app/SystemMonitor.cpp
    src/midi/ParameterStore.h
    src/midi/ParameterStore.cpp
    src/midi/MidiControllerManager.h
    src/midi/MidiControllerManager.cpp
    src/app/SettingsManager.h
    src/app/SettingsManager.cpp
    src/library/TrackIdGenerator.h
    src/library/LibraryDatabase.h
    src/library/LibraryDatabase.cpp
    src/library/LibraryTableModel.h
    src/library/LibraryTableModel.cpp
    src/fx/BrickwallLimiter.h
    src/fx/BrickwallLimiter.cpp
    src/audio/ScratchEngine.h
    src/audio/ScratchEngine.cpp
    src/audio/TurntableSimulation.h
    src/audio/TurntableSimulation.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    src/audio
    src/rendering
    src/midi
    src/analysis
    src/domain
    src/app
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_ALSA=1
    JUCE_ALSA_SEQ=1
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    juce::juce_core
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_events
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Quick3D
    Qt6::Sql
    PkgConfig::TAGLIB
    PkgConfig::KEYFINDER
    PkgConfig::RUBBERBAND
    PkgConfig::ALSA
    Ableton::Link
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)