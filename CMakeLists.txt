cmake_minimum_required(VERSION 3.22)

set(PROJECT_NAME "MultiPlatformDJ")
project(${PROJECT_NAME} VERSION 0.1.0 LANGUAGES C CXX)

# Ensure Qt can be found when installed via pacman
list(APPEND CMAKE_PREFIX_PATH "/usr/lib/cmake" "/usr/local/lib/cmake")

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache for faster compilation")
endif()

# Qt setup
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Quick3D)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# TagLib für Cover-Art-Extraction
find_package(PkgConfig REQUIRED)
pkg_check_modules(TAGLIB    REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(AUBIO     REQUIRED IMPORTED_TARGET aubio)
pkg_check_modules(KEYFINDER REQUIRED IMPORTED_TARGET libkeyfinder)

add_subdirectory(libs/JUCE)

juce_add_console_app(${PROJECT_NAME}
    PRODUCT_NAME "Multiplatform DJ Software"
    COMPANY_NAME "Ramsbrock.net"
)

# Pack QML files into the binary under URI "DJSoftware"
qt_add_qml_module(${PROJECT_NAME}
    URI DJSoftware
    VERSION 1.0
    QML_FILES
        src/main.qml
        src/TopHeader.qml
        src/EnlargedWaveform.qml
        src/OverallWaveform.qml
        src/DeckControl.qml
        src/TrackInfoDisplay.qml
        src/Library.qml
        src/PerformancePads.qml
        src/MixerSection.qml
        src/SettingsWindow.qml
        src/FxUnit.qml
        src/FxBar.qml
)

target_sources(${PROJECT_NAME} PRIVATE
    src/main.cpp
    src/DjEngine.cpp
    src/WaveformItem.cpp
    src/ScrollingWaveformItem.cpp
    src/WaveformAnalyzer.cpp
    src/TrackData.h
    src/LibraryManager.h
    src/LibraryManager.cpp
    src/CoverArtExtractor.h
    src/CoverArtExtractor.cpp
    src/CoverArtProvider.h
    src/FxManager.h
    src/FxManager.cpp
    src/FxProcessor.h
    src/FxProcessor.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_MP3AUDIOFORMAT=1
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
    PkgConfig::TAGLIB
    PkgConfig::AUBIO
    PkgConfig::KEYFINDER
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)