cmake_minimum_required(VERSION 3.24)

set(PROJECT_NAME "BrockDJ")
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
        "Minimum supported macOS version")
endif()
project(${PROJECT_NAME} VERSION 0.0.1 LANGUAGES C CXX)
set(BROCKDJ_PROJECT_VERSION "${PROJECT_VERSION}" CACHE STRING "BrockDJ project version" FORCE)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BROCKDJ_ENABLE_NATIVE_ARCH "Enable target-specific -march=native code generation for local release builds" OFF)
unset(RDBJ_ENABLE_NATIVE_SIMD CACHE)
option(RDBJ_ENABLE_QML_CACHEGEN "Compile QML to C++ cache files. Disable for faster local dev builds." ON)

# Ensure Qt can be found when installed via pacman (Linux only)
if(UNIX AND NOT APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/usr/lib/cmake" "/usr/local/lib/cmake")
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND NOT MSVC)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache compiler launcher for faster rebuilds")
elseif(CCACHE_PROGRAM AND MSVC)
    message(STATUS "ccache found but disabled for MSVC builds")
endif()

# Qt setup
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Sql Concurrent)
find_package(Threads REQUIRED)
if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
    qt_policy(SET QTP0004 NEW)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
include(ProcessorCount)
ProcessorCount(_cpu_count)
set(BROCKDJ_AUTOGEN_PARALLEL "4" CACHE STRING
    "Maximum parallel Qt autogen (moc/uic/rcc) jobs; 0 uses all available CPUs")
if(BROCKDJ_AUTOGEN_PARALLEL EQUAL 0)
    set(CMAKE_AUTOGEN_PARALLEL ${_cpu_count})
elseif(BROCKDJ_AUTOGEN_PARALLEL GREATER 0)
    # Qt autogen is memory-heavy. Keeping it below the compile-job count makes
    # incremental builds much more responsive on typical development machines.
    set(CMAKE_AUTOGEN_PARALLEL ${BROCKDJ_AUTOGEN_PARALLEL})
endif()

function(_rdbj_define_imported_binary_target target_name include_dir library_path)
    if(NOT TARGET ${target_name})
        add_library(${target_name} UNKNOWN IMPORTED)
        set_target_properties(${target_name} PROPERTIES
            IMPORTED_LOCATION "${library_path}"
            INTERFACE_INCLUDE_DIRECTORIES "${include_dir}")
    endif()
endfunction()

# Third-party dependencies (cross-platform)
find_package(PkgConfig QUIET)

if(UNIX AND NOT APPLE)
    pkg_check_modules(JACK QUIET IMPORTED_TARGET jack)
    if(TARGET PkgConfig::JACK)
        message(STATUS "Enabling JUCE JACK backend")
        add_compile_definitions(JUCE_JACK=1)
    endif()

    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
    if(TARGET PkgConfig::LIBUSB)
        message(STATUS "Enabling Linux controller HID support via libusb")
        add_compile_definitions(BROCKDJ_HAS_LIBUSB=1)
    endif()
endif()

# ---- TagLib ---------------------------------------------------------------
if(NOT TARGET ThirdParty::TagLib)
    add_library(ThirdParty::TagLib INTERFACE IMPORTED)
endif()

set(_taglib_found FALSE)
find_package(TagLib QUIET CONFIG)
if(TARGET TagLib::TagLib)
    target_link_libraries(ThirdParty::TagLib INTERFACE TagLib::TagLib)
    set(_taglib_found TRUE)
elseif(TARGET TagLib::tag)
    target_link_libraries(ThirdParty::TagLib INTERFACE TagLib::tag)
    set(_taglib_found TRUE)
endif()

if(NOT _taglib_found AND PkgConfig_FOUND)
    pkg_check_modules(TAGLIB QUIET IMPORTED_TARGET taglib)
    if(TARGET PkgConfig::TAGLIB)
        target_link_libraries(ThirdParty::TagLib INTERFACE PkgConfig::TAGLIB)
        set(_taglib_found TRUE)
    endif()
endif()

if(NOT _taglib_found)
    find_path(TAGLIB_INCLUDE_DIR NAMES taglib/tag.h)
    find_library(TAGLIB_LIBRARY NAMES taglib)
    if(TAGLIB_INCLUDE_DIR AND TAGLIB_LIBRARY)
        _rdbj_define_imported_binary_target(ThirdPartyTagLibBinary "${TAGLIB_INCLUDE_DIR}" "${TAGLIB_LIBRARY}")
        target_link_libraries(ThirdParty::TagLib INTERFACE ThirdPartyTagLibBinary)
        set(_taglib_found TRUE)
    endif()
endif()

if(NOT _taglib_found)
    message(FATAL_ERROR "TagLib not found. Install taglib development files for your platform.")
endif()

# ---- KeyFinder ------------------------------------------------------------
if(NOT TARGET ThirdParty::KeyFinder)
    add_library(ThirdParty::KeyFinder INTERFACE IMPORTED)
endif()

set(_keyfinder_found FALSE)
find_package(KeyFinder QUIET CONFIG)
if(TARGET KeyFinder::KeyFinder)
    target_link_libraries(ThirdParty::KeyFinder INTERFACE KeyFinder::KeyFinder)
    set(_keyfinder_found TRUE)
elseif(TARGET KeyFinder::keyfinder)
    target_link_libraries(ThirdParty::KeyFinder INTERFACE KeyFinder::keyfinder)
    set(_keyfinder_found TRUE)
endif()

if(NOT _keyfinder_found AND PkgConfig_FOUND)
    pkg_check_modules(KEYFINDER QUIET IMPORTED_TARGET libkeyfinder)
    if(TARGET PkgConfig::KEYFINDER)
        target_link_libraries(ThirdParty::KeyFinder INTERFACE PkgConfig::KEYFINDER)
        set(_keyfinder_found TRUE)
    endif()
endif()

if(NOT _keyfinder_found)
    find_path(KEYFINDER_INCLUDE_DIR NAMES keyfinder/keyfinder.h)
    find_library(KEYFINDER_LIBRARY NAMES keyfinder libkeyfinder)
    if(KEYFINDER_INCLUDE_DIR AND KEYFINDER_LIBRARY)
        _rdbj_define_imported_binary_target(ThirdPartyKeyFinderBinary "${KEYFINDER_INCLUDE_DIR}" "${KEYFINDER_LIBRARY}")
        target_link_libraries(ThirdParty::KeyFinder INTERFACE ThirdPartyKeyFinderBinary)
        set(_keyfinder_found TRUE)
    endif()
endif()

if(NOT _keyfinder_found)
    message(FATAL_ERROR "libkeyfinder not found. Install libkeyfinder development files for your platform.")
endif()

# ---- RubberBand -----------------------------------------------------------
if(NOT TARGET ThirdParty::RubberBand)
    add_library(ThirdParty::RubberBand INTERFACE IMPORTED)
endif()

set(_rubberband_found FALSE)
find_package(RubberBand QUIET CONFIG)
if(TARGET RubberBand::rubberband)
    target_link_libraries(ThirdParty::RubberBand INTERFACE RubberBand::rubberband)
    set(_rubberband_found TRUE)
endif()

if(NOT _rubberband_found AND PkgConfig_FOUND)
    pkg_check_modules(RUBBERBAND QUIET IMPORTED_TARGET rubberband)
    if(TARGET PkgConfig::RUBBERBAND)
        target_link_libraries(ThirdParty::RubberBand INTERFACE PkgConfig::RUBBERBAND)
        set(_rubberband_found TRUE)
    endif()
endif()

if(NOT _rubberband_found)
    find_path(RUBBERBAND_INCLUDE_DIR NAMES rubberband/RubberBandStretcher.h)
    find_library(RUBBERBAND_LIBRARY NAMES rubberband)
    if(RUBBERBAND_INCLUDE_DIR AND RUBBERBAND_LIBRARY)
        _rdbj_define_imported_binary_target(ThirdPartyRubberBandBinary "${RUBBERBAND_INCLUDE_DIR}" "${RUBBERBAND_LIBRARY}")
        target_link_libraries(ThirdParty::RubberBand INTERFACE ThirdPartyRubberBandBinary)
        set(_rubberband_found TRUE)
    endif()
endif()

if(NOT _rubberband_found)
    message(FATAL_ERROR "RubberBand not found. Install rubberband development files for your platform.")
endif()

# ---- ALSA (Linux only) ----------------------------------------------------
if(UNIX AND NOT APPLE)
    if(NOT TARGET ThirdParty::ALSA)
        add_library(ThirdParty::ALSA INTERFACE IMPORTED)
    endif()

    set(_alsa_found FALSE)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA QUIET IMPORTED_TARGET alsa)
        if(TARGET PkgConfig::ALSA)
            target_link_libraries(ThirdParty::ALSA INTERFACE PkgConfig::ALSA)
            set(_alsa_found TRUE)
        endif()
    endif()

    if(NOT _alsa_found)
        find_path(ALSA_INCLUDE_DIR NAMES alsa/asoundlib.h)
        find_library(ALSA_LIBRARY NAMES asound)
        if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
            _rdbj_define_imported_binary_target(ThirdPartyALSABinary "${ALSA_INCLUDE_DIR}" "${ALSA_LIBRARY}")
            target_link_libraries(ThirdParty::ALSA INTERFACE ThirdPartyALSABinary)
            set(_alsa_found TRUE)
        endif()
    endif()

    if(NOT _alsa_found)
        message(FATAL_ERROR "ALSA not found on Linux. Install alsa development files.")
    endif()
endif()

# ---- Signalsmith (header-only, MIT) ---------------------------------------
# Stretch fetches Linear only when its local CMake target does not exist.  Keep
# this order so an initialized submodule checkout configures without downloads.
set(SIGNALSMITH_USE_IPP OFF CACHE BOOL "" FORCE)
set(SIGNALSMITH_USE_PFFFT OFF CACHE BOOL "" FORCE)
set(SIGNALSMITH_USE_PFFFT_DOUBLE OFF CACHE BOOL "" FORCE)
set(SIGNALSMITH_USE_XSIMD OFF CACHE BOOL "" FORCE)
set(SIGNALSMITH_USE_XSIMD_DISPATCH OFF CACHE BOOL "" FORCE)
set(SIGNALSMITH_USE_CMSISDSP OFF CACHE BOOL "" FORCE)

add_subdirectory(libs/signalsmith-linear EXCLUDE_FROM_ALL)
add_subdirectory(libs/signalsmith-dsp EXCLUDE_FROM_ALL)
add_subdirectory(libs/signalsmith-stretch EXCLUDE_FROM_ALL)

if(NOT TARGET ThirdParty::SignalsmithLinear)
    add_library(ThirdParty::SignalsmithLinear ALIAS signalsmith-linear)
endif()
if(NOT TARGET ThirdParty::SignalsmithDSP)
    add_library(ThirdParty::SignalsmithDSP ALIAS signalsmith-dsp)
endif()
if(NOT TARGET ThirdParty::SignalsmithStretch)
    add_library(ThirdParty::SignalsmithStretch ALIAS signalsmith-stretch)
endif()

add_subdirectory(libs/JUCE)

# Ableton Link — header-only library for tempo/beat sync
# Disable tests and examples; we only need the Ableton::Link imported target.
set(LINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
include(libs/link/AbletonLinkConfig.cmake)
# Ableton Link's compatibility include sets a directory-wide C++17 default.
# BrockDJ owns the application baseline and reapplies it for all later targets.
set(CMAKE_CXX_STANDARD 23)

set(_icons_dir "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons")
set(_icons_generated "${_icons_dir}/generated")
set(_icons_ready FALSE)
if(EXISTS "${_icons_generated}/256.png")
    set(_icons_ready TRUE)
endif()

juce_add_console_app(${PROJECT_NAME}
    PRODUCT_NAME "BrockDJ"
    COMPANY_NAME "Ramsbrock.net"
)

target_sources(${PROJECT_NAME} PRIVATE src/main.cpp)

# Keep source ownership next to each production domain. These subdirectories
# only add files to the already-created application target.
add_subdirectory(src/analysis)
add_subdirectory(src/app)
add_subdirectory(src/audio)
add_subdirectory(src/controllers)
add_subdirectory(src/database)
add_subdirectory(src/domain)
add_subdirectory(src/engine)
add_subdirectory(src/fx)
add_subdirectory(src/io)
add_subdirectory(src/library)
add_subdirectory(src/link)
add_subdirectory(src/midi)
add_subdirectory(src/platform)
add_subdirectory(src/qml)
add_subdirectory(src/rendering)
add_subdirectory(src/waveform)

# Pack QML files into the binary under URI "DJSoftware".
set(_rdbj_qml_module_options)
if(NOT RDBJ_ENABLE_QML_CACHEGEN)
    list(APPEND _rdbj_qml_module_options NO_CACHEGEN)
endif()

set_source_files_properties(
    src/qml/UiTheme.qml
    src/qml/shared/UiMetrics.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(${PROJECT_NAME}
    ${_rdbj_qml_module_options}
    URI DJSoftware
    VERSION 1.0
    RESOURCE_PREFIX "/"
    QML_FILES
        ${BROCKDJ_QML_FILES}
)


target_include_directories(${PROJECT_NAME} PRIVATE
    src
    src/engine
    src/engine/scratch
    src/rendering
    src/midi
    src/controllers
    src/midi/feedback
    src/fx/dsp
    src/analysis
    src/domain
    src/app
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    BROCKDJ_VERSION="${PROJECT_VERSION}"
    BROCKDJ_BUILD_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_MP3AUDIOFORMAT=1
)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WINSOCK_DEPRECATED_NO_WARNINGS=1
    )

    target_link_libraries(${PROJECT_NAME} PRIVATE
        wininet
        ws2_32
        iphlpapi
    )

endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        JUCE_ALSA=1
        JUCE_ALSA_SEQ=1
    )
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        JUCE_ALSA=0
        JUCE_ALSA_SEQ=0
    )
endif()

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
    Qt6::QuickControls2
    Qt6::Sql
    Qt6::Concurrent
    ThirdParty::TagLib
    ThirdParty::KeyFinder
    ThirdParty::RubberBand
    ThirdParty::SignalsmithDSP
    ThirdParty::SignalsmithStretch
    Ableton::Link
)

if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ThirdParty::ALSA
    )

    if(TARGET PkgConfig::JACK)
        target_link_libraries(${PROJECT_NAME} PRIVATE
            PkgConfig::JACK
        )
    endif()

    if(TARGET PkgConfig::LIBUSB)
        target_link_libraries(${PROJECT_NAME} PRIVATE
            PkgConfig::LIBUSB
        )
    endif()
endif()

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

option(BROCKDJ_TESTS_ONLY "Exclude the GUI application from the default test build" OFF)
if(BROCKDJ_TESTS_ONLY)
    set_target_properties(${PROJECT_NAME} PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

if(BROCKDJ_ENABLE_NATIVE_ARCH)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
        target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:-march=native>
        )
    elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|i[3-6]86)$"
           AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:-march=native>
        )
    endif()
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "BrockDJ"
        MACOSX_BUNDLE_GUI_IDENTIFIER "net.ramsbrock.brockdj"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright Ramsbrock.net"
    )
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/WindowsHeaderCompat.h")
endif()

# ── App Icons ─────────────────────────────────────────────────────────────────
# Run scripts/generate_icons.sh first; then re-run CMake to pick up the icons.
if(_icons_ready)
    # All platforms: embed PNG sizes via QRC for QGuiApplication::setWindowIcon().
    target_sources(${PROJECT_NAME} PRIVATE "${_icons_dir}/icons.qrc")

    if(WIN32 AND EXISTS "${_icons_generated}/appicon.ico")
        # Windows: also embed .ico directly in the executable for Explorer/taskbar.
        target_sources(${PROJECT_NAME} PRIVATE "${_icons_dir}/windows/appicon.rc")
    endif()

    if(APPLE AND EXISTS "${_icons_generated}/AppIcon.icns")
        # macOS: bundle the .icns so the Dock / Finder show the right icon.
        target_sources(${PROJECT_NAME} PRIVATE "${_icons_generated}/AppIcon.icns")
        set_source_files_properties("${_icons_generated}/AppIcon.icns"
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            MACOSX_BUNDLE_ICON_FILE AppIcon.icns)
    endif()
else()
    message(STATUS "Icon resources not found — run scripts/generate_icons.sh then re-run CMake.")
endif()

# ── Smoke tests ───────────────────────────────────────────────────────────────
include(CTest)

if(BUILD_TESTING)

add_executable(BrockDJ_ui_scale_tests tests/ui/ui_scale_tests.cpp)
target_include_directories(BrockDJ_ui_scale_tests PRIVATE src)
target_link_libraries(BrockDJ_ui_scale_tests PRIVATE Qt6::Core)
target_compile_features(BrockDJ_ui_scale_tests PRIVATE cxx_std_23)
add_test(NAME ui_scale COMMAND BrockDJ_ui_scale_tests)

add_executable(BrockDJ_waveform_zoom_tests tests/ui/waveform_zoom_tests.cpp)
target_include_directories(BrockDJ_waveform_zoom_tests PRIVATE src)
target_link_libraries(BrockDJ_waveform_zoom_tests PRIVATE Qt6::Core)
target_compile_features(BrockDJ_waveform_zoom_tests PRIVATE cxx_std_23)
add_test(NAME waveform_zoom COMMAND BrockDJ_waveform_zoom_tests)

add_executable(BrockDJ_ui_layout_tests tests/ui/ui_layout_tests.cpp)
target_compile_features(BrockDJ_ui_layout_tests PRIVATE cxx_std_23)
add_test(NAME ui_layout COMMAND BrockDJ_ui_layout_tests)

add_executable(BrockDJ_qml_component_tests tests/ui/qml_component_tests.cpp)
target_compile_definitions(BrockDJ_qml_component_tests PRIVATE BROCKDJ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(BrockDJ_qml_component_tests PRIVATE cxx_std_23)
add_test(NAME qml_component COMMAND BrockDJ_qml_component_tests)

add_executable(BrockDJ_waveform_render_stability_tests
    tests/waveform_render_stability/waveform_render_stability_tests.cpp)
target_include_directories(BrockDJ_waveform_render_stability_tests PRIVATE src)
target_compile_features(BrockDJ_waveform_render_stability_tests PRIVATE cxx_std_23)
add_test(NAME waveform_render_stability COMMAND BrockDJ_waveform_render_stability_tests)

add_executable(BrockDJ_smoke_tests
    tests/smoke/smoke_tests.cpp
    tests/smoke/mixer_dsp_smoke.cpp
    src/analysis/AnalysisValidation.cpp
    src/audio/DeckChannelProcessor.cpp
    src/audio/internal/MixerFilterCoefficients.cpp
    src/fx/FxProcessor.cpp
    src/fx/BrickwallLimiter.cpp
)

target_include_directories(BrockDJ_smoke_tests PRIVATE
    src
    src/analysis
    src/controllers
    src/domain
    src/engine
    src/fx/dsp
)

target_link_libraries(BrockDJ_smoke_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    juce::juce_core
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_dsp
    juce::juce_events
    ThirdParty::SignalsmithDSP
)
target_compile_definitions(BrockDJ_smoke_tests PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)
target_compile_features(BrockDJ_smoke_tests PRIVATE cxx_std_23)

add_test(NAME smoke COMMAND BrockDJ_smoke_tests)

add_executable(BrockDJ_mixer_dsp_tests
    tests/mixer_dsp/mixer_dsp_tests.cpp
    src/audio/DeckChannelProcessor.cpp
    src/audio/internal/MixerFilterCoefficients.cpp
    src/fx/FxProcessor.cpp
    src/fx/BrickwallLimiter.cpp
)
target_include_directories(BrockDJ_mixer_dsp_tests PRIVATE src src/engine src/fx/dsp)
target_link_libraries(BrockDJ_mixer_dsp_tests PRIVATE Qt6::Core Qt6::Gui juce::juce_core juce::juce_data_structures juce::juce_audio_basics juce::juce_dsp juce::juce_events ThirdParty::SignalsmithDSP)
target_compile_definitions(BrockDJ_mixer_dsp_tests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_compile_features(BrockDJ_mixer_dsp_tests PRIVATE cxx_std_23)
add_test(NAME mixer_dsp COMMAND BrockDJ_mixer_dsp_tests)

add_executable(BrockDJ_deck_audio_graph_tests
    tests/deck_audio_graph/deck_audio_graph_tests.cpp
    src/audio/AudioEngine.cpp
    src/audio/MasterMixer.cpp
    src/audio/HeadphoneBus.cpp
    src/audio/AudioOutputRouter.cpp
    src/audio/DeckAudioPipeline.cpp
    src/audio/cache/AudioPageCache.cpp src/audio/cache/AudioCacheWorker.cpp src/audio/cache/CachedPlaybackAudioSource.cpp
    src/audio/RenderModeRouter.cpp src/audio/internal/ScratchResampler.cpp src/audio/internal/HermiteResamplingAudioSource.cpp
    src/engine/scratch/ScratchController.cpp src/engine/scratch/VirtualTurntable.cpp
    src/audio/TimeStretchProcessor.cpp src/audio/DeckChannelProcessor.cpp src/audio/internal/MixerFilterCoefficients.cpp
    src/fx/FxProcessor.cpp src/fx/BrickwallLimiter.cpp
)
target_include_directories(BrockDJ_deck_audio_graph_tests PRIVATE src src/engine src/fx/dsp)
target_link_libraries(BrockDJ_deck_audio_graph_tests PRIVATE Qt6::Core Qt6::Gui juce::juce_core juce::juce_data_structures juce::juce_audio_basics juce::juce_audio_formats juce::juce_audio_devices juce::juce_dsp juce::juce_events ThirdParty::RubberBand ThirdParty::SignalsmithDSP ThirdParty::SignalsmithStretch)
target_compile_definitions(BrockDJ_deck_audio_graph_tests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1)
target_compile_features(BrockDJ_deck_audio_graph_tests PRIVATE cxx_std_23)
add_test(NAME deck_audio_graph COMMAND BrockDJ_deck_audio_graph_tests)

add_executable(BrockDJ_deck_transport_tests
    tests/deck_transport/deck_transport_tests.cpp
    src/app/ControlClock.cpp
    src/engine/deck/DeckTransport.cpp src/audio/DeckAudioPipeline.cpp
    src/engine/sync/DeckSyncController.cpp src/engine/sync/SyncCoordinator.cpp
    src/audio/cache/AudioPageCache.cpp src/audio/cache/AudioCacheWorker.cpp src/audio/cache/CachedPlaybackAudioSource.cpp
    src/audio/RenderModeRouter.cpp src/audio/internal/ScratchResampler.cpp src/audio/internal/HermiteResamplingAudioSource.cpp
    src/engine/scratch/ScratchController.cpp src/engine/scratch/VirtualTurntable.cpp
    src/audio/TimeStretchProcessor.cpp src/audio/DeckChannelProcessor.cpp src/audio/internal/MixerFilterCoefficients.cpp
    src/fx/FxProcessor.cpp src/fx/BrickwallLimiter.cpp
)
target_include_directories(BrockDJ_deck_transport_tests PRIVATE src src/engine src/fx/dsp)
target_link_libraries(BrockDJ_deck_transport_tests PRIVATE Qt6::Core Qt6::Gui juce::juce_core juce::juce_data_structures juce::juce_audio_basics juce::juce_audio_formats juce::juce_audio_devices juce::juce_dsp juce::juce_events ThirdParty::RubberBand ThirdParty::SignalsmithDSP ThirdParty::SignalsmithStretch)
target_compile_definitions(BrockDJ_deck_transport_tests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1)
target_compile_features(BrockDJ_deck_transport_tests PRIVATE cxx_std_23)
add_test(NAME deck_transport COMMAND BrockDJ_deck_transport_tests)

add_executable(BrockDJ_flx10_jog_routing_tests
    tests/midi/flx10_jog_routing_tests.cpp)
target_include_directories(BrockDJ_flx10_jog_routing_tests PRIVATE src src/engine)
target_compile_features(BrockDJ_flx10_jog_routing_tests PRIVATE cxx_std_23)
add_test(NAME flx10_jog_routing COMMAND BrockDJ_flx10_jog_routing_tests)

add_executable(BrockDJ_alsa_midi_line_parser_tests
    tests/midi/alsa_midi_line_parser_tests.cpp)
target_include_directories(BrockDJ_alsa_midi_line_parser_tests PRIVATE src)
target_link_libraries(BrockDJ_alsa_midi_line_parser_tests PRIVATE Qt6::Core)
target_compile_features(BrockDJ_alsa_midi_line_parser_tests PRIVATE cxx_std_23)
add_test(NAME alsa_midi_line_parser COMMAND BrockDJ_alsa_midi_line_parser_tests)

add_executable(BrockDJ_midi_14bit_accumulator_tests
    tests/midi/midi_14bit_accumulator_tests.cpp)
target_include_directories(BrockDJ_midi_14bit_accumulator_tests PRIVATE src)
target_compile_features(BrockDJ_midi_14bit_accumulator_tests PRIVATE cxx_std_23)
add_test(NAME midi_14bit_accumulator COMMAND BrockDJ_midi_14bit_accumulator_tests)

add_executable(BrockDJ_parameter_store_tests
    tests/midi/parameter_store_tests.cpp
    src/midi/ParameterStore.cpp)
target_include_directories(BrockDJ_parameter_store_tests PRIVATE src)
target_link_libraries(BrockDJ_parameter_store_tests PRIVATE Qt6::Core)
target_compile_features(BrockDJ_parameter_store_tests PRIVATE cxx_std_23)
add_test(NAME parameter_store COMMAND BrockDJ_parameter_store_tests)

add_executable(BrockDJ_flx10_display_protocol_tests
    tests/midi/flx10_display_protocol_tests.cpp)
target_include_directories(BrockDJ_flx10_display_protocol_tests PRIVATE src)
target_link_libraries(BrockDJ_flx10_display_protocol_tests PRIVATE Qt6::Core Qt6::Gui)
target_compile_features(BrockDJ_flx10_display_protocol_tests PRIVATE cxx_std_23)
add_test(NAME flx10_display_protocol COMMAND BrockDJ_flx10_display_protocol_tests)

add_executable(BrockDJ_sync_coordinator_tests
    tests/sync/sync_coordinator_tests.cpp
    src/engine/sync/DeckSyncController.cpp
    src/engine/sync/SyncCoordinator.cpp)
target_include_directories(BrockDJ_sync_coordinator_tests PRIVATE src src/engine)
target_link_libraries(BrockDJ_sync_coordinator_tests PRIVATE Qt6::Core)
target_compile_features(BrockDJ_sync_coordinator_tests PRIVATE cxx_std_23)
add_test(NAME sync_coordinator COMMAND BrockDJ_sync_coordinator_tests)

add_executable(BrockDJ_control_clock_tests
    tests/control_clock/control_clock_tests.cpp
    src/app/ControlClock.cpp)
target_include_directories(BrockDJ_control_clock_tests PRIVATE src)
target_link_libraries(BrockDJ_control_clock_tests PRIVATE Qt6::Core)
target_compile_features(BrockDJ_control_clock_tests PRIVATE cxx_std_23)
add_test(NAME control_clock COMMAND BrockDJ_control_clock_tests)

add_executable(BrockDJ_master_bus_tests
    tests/master_bus/master_bus_tests.cpp
    src/audio/MasterMixer.cpp
    src/audio/HeadphoneBus.cpp
    src/audio/AudioOutputRouter.cpp
    src/fx/FxProcessor.cpp
    src/fx/BrickwallLimiter.cpp
)
target_include_directories(BrockDJ_master_bus_tests PRIVATE
    src src/engine src/fx/dsp)
target_link_libraries(BrockDJ_master_bus_tests PRIVATE
    Qt6::Core Qt6::Gui
    juce::juce_core juce::juce_events juce::juce_audio_basics
    juce::juce_audio_devices juce::juce_dsp ThirdParty::SignalsmithDSP)
target_compile_definitions(BrockDJ_master_bus_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1)
target_compile_features(BrockDJ_master_bus_tests PRIVATE cxx_std_23)
add_test(NAME master_bus COMMAND BrockDJ_master_bus_tests)

add_executable(BrockDJ_audio_routing_contract_tests
    tests/audio_routing/audio_routing_contract_tests.cpp
    src/audio/MasterMixer.cpp
    src/audio/HeadphoneBus.cpp
    src/audio/AudioOutputRouter.cpp
    src/fx/FxProcessor.cpp
    src/fx/BrickwallLimiter.cpp
)
target_include_directories(BrockDJ_audio_routing_contract_tests PRIVATE
    src src/engine src/fx/dsp)
target_link_libraries(BrockDJ_audio_routing_contract_tests PRIVATE
    Qt6::Core Qt6::Gui
    juce::juce_core juce::juce_events juce::juce_audio_basics
    juce::juce_audio_devices juce::juce_dsp ThirdParty::SignalsmithDSP)
target_compile_definitions(BrockDJ_audio_routing_contract_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1)
target_compile_features(BrockDJ_audio_routing_contract_tests PRIVATE cxx_std_23)
add_test(NAME audio_routing_contract COMMAND BrockDJ_audio_routing_contract_tests)

add_executable(BrockDJ_signalsmith_dependency_tests
    tests/third_party/signalsmith_dependency_tests.cpp)
target_link_libraries(BrockDJ_signalsmith_dependency_tests PRIVATE
    ThirdParty::SignalsmithDSP
    ThirdParty::SignalsmithStretch)
target_compile_features(BrockDJ_signalsmith_dependency_tests PRIVATE cxx_std_23)
add_test(NAME signalsmith_dependencies COMMAND BrockDJ_signalsmith_dependency_tests)

add_executable(BrockDJ_analysis_lifetime_tests
    tests/analysis/analysis_lifetime_tests.cpp
    src/rendering/WaveformAnalyzer.cpp
    src/rendering/WaveformEnvelopePass.cpp
    src/rendering/WaveformAnalysisOrchestrator.cpp
    src/rendering/WaveformCache.cpp
    src/domain/TrackData.cpp
    src/waveform/WaveformLineStore.cpp
    src/analysis/AnalysisFeatureExtractor.cpp
    src/analysis/TempoEstimator.cpp
    src/analysis/BeatTracker.cpp
    src/analysis/DownbeatDetector.cpp
    src/analysis/BeatGridFitter.cpp
    src/analysis/AnalysisValidation.cpp
    src/analysis/PhraseAnalyzer.cpp
)
target_include_directories(BrockDJ_analysis_lifetime_tests PRIVATE
    src src/analysis src/domain src/rendering
)
target_link_libraries(BrockDJ_analysis_lifetime_tests PRIVATE
    Qt6::Core Qt6::Gui
    juce::juce_core juce::juce_data_structures juce::juce_audio_basics
    juce::juce_audio_formats juce::juce_dsp juce::juce_events
    ThirdParty::KeyFinder
)
target_compile_definitions(BrockDJ_analysis_lifetime_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
)
target_compile_features(BrockDJ_analysis_lifetime_tests PRIVATE cxx_std_23)
add_test(NAME analysis_lifetime COMMAND BrockDJ_analysis_lifetime_tests)

add_executable(BrockDJ_analysis_snapshot_tests
    tests/analysis/analysis_snapshot_tests.cpp
    src/domain/TrackData.cpp
    src/waveform/WaveformLineStore.cpp
)
target_include_directories(BrockDJ_analysis_snapshot_tests PRIVATE src src/analysis src/domain)
target_link_libraries(BrockDJ_analysis_snapshot_tests PRIVATE Qt6::Core Qt6::Gui)
target_compile_features(BrockDJ_analysis_snapshot_tests PRIVATE cxx_std_23)
add_test(NAME analysis_snapshot COMMAND BrockDJ_analysis_snapshot_tests)

add_executable(BrockDJ_library_analysis_manager_tests
    tests/analysis/library_analysis_manager_tests.cpp)
target_include_directories(BrockDJ_library_analysis_manager_tests PRIVATE src)
target_link_libraries(BrockDJ_library_analysis_manager_tests PRIVATE Qt6::Core)
target_compile_features(BrockDJ_library_analysis_manager_tests PRIVATE cxx_std_23)
add_test(NAME library_analysis_manager COMMAND BrockDJ_library_analysis_manager_tests)

add_executable(BrockDJ_dj_engine_api_contract_tests
    tests/engine/dj_engine_api_contract_tests.cpp)
target_compile_definitions(BrockDJ_dj_engine_api_contract_tests PRIVATE
    SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(BrockDJ_dj_engine_api_contract_tests PRIVATE Qt6::Core)
target_compile_features(BrockDJ_dj_engine_api_contract_tests PRIVATE cxx_std_23)
add_test(NAME dj_engine_api_contract COMMAND BrockDJ_dj_engine_api_contract_tests)

if(UNIX)
    add_executable(BrockDJ_posix_signal_tests
        tests/platform/posix_signal_handler_tests.cpp
        src/platform/PosixSignalHandler.h
        src/platform/PosixSignalHandler.cpp
    )
    target_include_directories(BrockDJ_posix_signal_tests PRIVATE src)
    target_link_libraries(BrockDJ_posix_signal_tests PRIVATE Qt6::Core)
    target_compile_features(BrockDJ_posix_signal_tests PRIVATE cxx_std_23)
    add_test(NAME posix_signal_handler COMMAND BrockDJ_posix_signal_tests)
endif()

add_executable(BrockDJ_audio_device_service_tests
    tests/audio/audio_device_service_tests.cpp
    src/audio/device/AudioDeviceService.h
    src/audio/device/AudioDeviceService.cpp
    src/audio/device/AudioDeviceUtils.cpp
)
target_include_directories(BrockDJ_audio_device_service_tests PRIVATE src src/engine)
target_link_libraries(BrockDJ_audio_device_service_tests PRIVATE
    Qt6::Core
    juce::juce_core juce::juce_audio_basics juce::juce_audio_devices
)
target_compile_definitions(BrockDJ_audio_device_service_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
)
target_compile_features(BrockDJ_audio_device_service_tests PRIVATE cxx_std_23)
if(UNIX AND NOT APPLE)
    target_link_libraries(BrockDJ_audio_device_service_tests PRIVATE ThirdParty::ALSA)
    if(TARGET PkgConfig::JACK)
        target_link_libraries(BrockDJ_audio_device_service_tests PRIVATE PkgConfig::JACK)
    endif()
endif()
add_test(NAME audio_device_service COMMAND BrockDJ_audio_device_service_tests)

add_executable(BrockDJ_cue_loop_controller_tests
    tests/cue_loop/deck_cue_loop_controller_tests.cpp
    src/engine/deck/DeckCueLoopController.h
    src/engine/deck/DeckCueLoopController.cpp
)
target_include_directories(BrockDJ_cue_loop_controller_tests PRIVATE src)
target_link_libraries(BrockDJ_cue_loop_controller_tests PRIVATE Qt6::Core)
target_compile_features(BrockDJ_cue_loop_controller_tests PRIVATE cxx_std_23)
add_test(NAME cue_loop_controller COMMAND BrockDJ_cue_loop_controller_tests)

add_executable(BrockDJ_track_loader_tests
    tests/track_loader/deck_track_loader_tests.cpp
    src/engine/deck/DeckTrackLoader.h
    src/engine/deck/DeckTrackLoader.cpp
    src/audio/cache/AudioPageCache.cpp
    src/audio/cache/AudioCacheWorker.cpp
    src/engine/deck/MetadataUtils.cpp
    src/rendering/WaveformCache.cpp
    src/domain/TrackData.cpp
    src/waveform/WaveformLineStore.cpp
)
target_include_directories(BrockDJ_track_loader_tests PRIVATE
    src src/domain src/engine src/library src/rendering
)
target_link_libraries(BrockDJ_track_loader_tests PRIVATE
    Qt6::Core Qt6::Gui
    juce::juce_core juce::juce_data_structures juce::juce_audio_basics
    juce::juce_audio_formats juce::juce_dsp juce::juce_events
)
target_compile_definitions(BrockDJ_track_loader_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
)
target_compile_features(BrockDJ_track_loader_tests PRIVATE cxx_std_23)
add_test(NAME track_loader COMMAND BrockDJ_track_loader_tests)

add_custom_target(BrockDJ_immediate_long_track_playback_tests
    DEPENDS BrockDJ_track_loader_tests
)
add_test(NAME immediate_long_track_playback COMMAND BrockDJ_track_loader_tests)

add_executable(BrockDJ_waveform_motion_tests
    tests/waveform_motion/waveform_motion_tests.cpp
)
target_compile_features(BrockDJ_waveform_motion_tests PRIVATE cxx_std_23)
add_test(NAME waveform_motion COMMAND BrockDJ_waveform_motion_tests)

add_executable(BrockDJ_progressive_waveform_publication_tests
    tests/progressive_waveform/progressive_waveform_publication_tests.cpp
    src/domain/TrackData.cpp
    src/waveform/WaveformLineStore.cpp
)
target_include_directories(BrockDJ_progressive_waveform_publication_tests PRIVATE src src/domain)
target_link_libraries(BrockDJ_progressive_waveform_publication_tests PRIVATE Qt6::Core Qt6::Gui)
target_compile_features(BrockDJ_progressive_waveform_publication_tests PRIVATE cxx_std_23)
add_test(NAME progressive_waveform_publication COMMAND BrockDJ_progressive_waveform_publication_tests)

add_executable(BrockDJ_waveform_line_store_tests
    tests/waveform_line_store/waveform_line_store_tests.cpp
    src/waveform/WaveformLineStore.cpp
)
target_include_directories(BrockDJ_waveform_line_store_tests PRIVATE src)
target_compile_features(BrockDJ_waveform_line_store_tests PRIVATE cxx_std_23)
add_test(NAME waveform_line_store COMMAND BrockDJ_waveform_line_store_tests)

add_executable(BrockDJ_waveform_line_chunk_tests
    tests/waveform_line_chunk/waveform_line_chunk_tests.cpp
)
target_include_directories(BrockDJ_waveform_line_chunk_tests PRIVATE src)
target_compile_features(BrockDJ_waveform_line_chunk_tests PRIVATE cxx_std_23)
add_test(NAME waveform_line_chunk COMMAND BrockDJ_waveform_line_chunk_tests)

add_executable(BrockDJ_waveform_line_render_tests
    tests/waveform_line_render/waveform_line_render_tests.cpp
    src/waveform/WaveformLineStore.cpp
)
target_include_directories(BrockDJ_waveform_line_render_tests PRIVATE src)
target_compile_features(BrockDJ_waveform_line_render_tests PRIVATE cxx_std_23)
add_test(NAME waveform_line_render COMMAND BrockDJ_waveform_line_render_tests)

add_executable(BrockDJ_audio_page_cache_tests
    tests/audio_cache/audio_page_cache_tests.cpp
    src/audio/cache/AudioPage.h
    src/audio/cache/AudioCacheHandle.h
    src/audio/cache/AudioPageCache.h
    src/audio/cache/AudioPageCache.cpp
    src/audio/cache/AudioCacheWorker.h
    src/audio/cache/AudioCacheWorker.cpp
)
target_include_directories(BrockDJ_audio_page_cache_tests PRIVATE src)
target_link_libraries(BrockDJ_audio_page_cache_tests PRIVATE
    Qt6::Core juce::juce_core juce::juce_audio_basics juce::juce_audio_formats
)
target_compile_definitions(BrockDJ_audio_page_cache_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
)
target_compile_features(BrockDJ_audio_page_cache_tests PRIVATE cxx_std_23)
add_test(NAME audio_page_cache COMMAND BrockDJ_audio_page_cache_tests)

add_executable(BrockDJ_audio_cache_stress
    tests/audio_cache/audio_cache_stress.cpp
    src/audio/cache/AudioPageCache.cpp
    src/audio/cache/AudioCacheWorker.cpp
    src/audio/internal/ScratchResampler.cpp
)
target_include_directories(BrockDJ_audio_cache_stress PRIVATE src)
target_link_libraries(BrockDJ_audio_cache_stress PRIVATE
    Qt6::Core juce::juce_core juce::juce_audio_basics juce::juce_audio_formats
)
target_compile_definitions(BrockDJ_audio_cache_stress PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
)
target_compile_features(BrockDJ_audio_cache_stress PRIVATE cxx_std_23)

add_executable(BrockDJ_scratch_cache_tests
    tests/scratch_cache/scratch_cache_tests.cpp
    src/audio/cache/AudioPageCache.cpp
    src/audio/cache/AudioCacheWorker.cpp
    src/audio/internal/ScratchResampler.cpp
)
target_include_directories(BrockDJ_scratch_cache_tests PRIVATE src src/engine)
target_link_libraries(BrockDJ_scratch_cache_tests PRIVATE
    Qt6::Core juce::juce_core juce::juce_audio_basics juce::juce_audio_formats
)
target_compile_definitions(BrockDJ_scratch_cache_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
)
target_compile_features(BrockDJ_scratch_cache_tests PRIVATE cxx_std_23)
add_test(NAME scratch_cache COMMAND BrockDJ_scratch_cache_tests)

add_executable(BrockDJ_cached_playback_tests
    tests/cached_playback/cached_playback_tests.cpp
    src/audio/cache/AudioPageCache.cpp
    src/audio/cache/AudioCacheWorker.cpp
    src/audio/cache/CachedPlaybackAudioSource.cpp
)
target_include_directories(BrockDJ_cached_playback_tests PRIVATE src)
target_link_libraries(BrockDJ_cached_playback_tests PRIVATE
    Qt6::Core juce::juce_core juce::juce_audio_basics juce::juce_audio_formats
)
target_compile_definitions(BrockDJ_cached_playback_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
)
target_compile_features(BrockDJ_cached_playback_tests PRIVATE cxx_std_23)
add_test(NAME cached_playback COMMAND BrockDJ_cached_playback_tests)

add_executable(BrockDJ_time_stretch_tests
    tests/time_stretch/time_stretch_tests.cpp
    src/audio/TimeStretchProcessor.cpp
)
target_include_directories(BrockDJ_time_stretch_tests PRIVATE src)
target_link_libraries(BrockDJ_time_stretch_tests PRIVATE
    juce::juce_core juce::juce_audio_basics ThirdParty::RubberBand ThirdParty::SignalsmithStretch
)
target_compile_definitions(BrockDJ_time_stretch_tests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_compile_features(BrockDJ_time_stretch_tests PRIVATE cxx_std_23)
add_test(NAME time_stretch COMMAND BrockDJ_time_stretch_tests)

add_executable(BrockDJ_database_worker_tests
    tests/database_worker/database_worker_tests.cpp
    src/database/DatabaseWorker.cpp)
target_include_directories(BrockDJ_database_worker_tests PRIVATE src)
target_link_libraries(BrockDJ_database_worker_tests PRIVATE Qt6::Core Qt6::Sql)
target_compile_features(BrockDJ_database_worker_tests PRIVATE cxx_std_23)
add_test(NAME database_worker COMMAND BrockDJ_database_worker_tests)

add_executable(BrockDJ_media_io_scheduler_tests
    tests/media_io_scheduler/media_io_scheduler_tests.cpp
    src/io/MediaIoScheduler.cpp
    src/library/CoverArtExtractor.cpp)
target_include_directories(BrockDJ_media_io_scheduler_tests PRIVATE src)
target_link_libraries(BrockDJ_media_io_scheduler_tests PRIVATE
    Qt6::Core Qt6::Gui ThirdParty::TagLib)
target_compile_features(BrockDJ_media_io_scheduler_tests PRIVATE cxx_std_23)
add_test(NAME media_io_scheduler COMMAND BrockDJ_media_io_scheduler_tests)

# Every BrockDJ executable uses the same portable language baseline and an
# explicit native thread dependency. This avoids relying on Qt/JUCE to provide
# pthread flags transitively and keeps MSVC, AppleClang, GCC and Clang aligned.
set(_brockdj_executable_targets
    BrockDJ_ui_scale_tests
    BrockDJ_waveform_zoom_tests
    BrockDJ_ui_layout_tests
    BrockDJ_qml_component_tests
    BrockDJ_waveform_render_stability_tests
    BrockDJ_smoke_tests
    BrockDJ_mixer_dsp_tests
    BrockDJ_deck_audio_graph_tests
    BrockDJ_deck_transport_tests
    BrockDJ_flx10_jog_routing_tests
    BrockDJ_sync_coordinator_tests
    BrockDJ_control_clock_tests
    BrockDJ_master_bus_tests
    BrockDJ_signalsmith_dependency_tests
    BrockDJ_analysis_lifetime_tests
    BrockDJ_analysis_snapshot_tests
    BrockDJ_library_analysis_manager_tests
    BrockDJ_dj_engine_api_contract_tests
    BrockDJ_audio_device_service_tests
    BrockDJ_cue_loop_controller_tests
    BrockDJ_track_loader_tests
    BrockDJ_waveform_motion_tests
    BrockDJ_progressive_waveform_publication_tests
    BrockDJ_waveform_line_store_tests
    BrockDJ_waveform_line_chunk_tests
    BrockDJ_waveform_line_render_tests
    BrockDJ_audio_page_cache_tests
    BrockDJ_scratch_cache_tests
    BrockDJ_cached_playback_tests
    BrockDJ_time_stretch_tests
    BrockDJ_database_worker_tests
    BrockDJ_media_io_scheduler_tests
)
if(TARGET BrockDJ_posix_signal_tests)
    list(APPEND _brockdj_executable_targets BrockDJ_posix_signal_tests)
endif()
foreach(_target IN LISTS _brockdj_executable_targets)
    target_link_libraries(${_target} PRIVATE Threads::Threads)
endforeach()
endif()

# The application always uses worker threads, independently of whether tests
# are enabled for the current build tree.
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

include(GNUInstallDirs)
if(APPLE)
    install(TARGETS ${PROJECT_NAME} BUNDLE DESTINATION .)
else()
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
