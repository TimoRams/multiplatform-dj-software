cmake_minimum_required(VERSION 3.22)

set(PROJECT_NAME "BrockDJ")
project(${PROJECT_NAME} VERSION 0.0.1 LANGUAGES C CXX)
set(BROCKDJ_PROJECT_VERSION "${PROJECT_VERSION}" CACHE STRING "BrockDJ project version" FORCE)

set(CMAKE_CXX_STANDARD 26)
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
if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
    qt_policy(SET QTP0004 NEW)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
include(ProcessorCount)
ProcessorCount(_cpu_count)
if(_cpu_count GREATER 1)
    # Let Qt's autogen (moc/uic/rcc) run in parallel on multicore systems.
    set(CMAKE_AUTOGEN_PARALLEL ${_cpu_count})
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

# ---- Signalsmith DSP (header-only, MIT) -----------------------------------
# Vendored subset of https://github.com/Signalsmith-Audio/signalsmith-dsp
# Only dsp/delay.h is included — the Delay<> circular buffer + Linear interpolator.
set(SIGNALSMITH_DSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/signalsmith-dsp")

add_subdirectory(libs/JUCE)

# Ableton Link — header-only library for tempo/beat sync
# Disable tests and examples; we only need the Ableton::Link imported target.
set(LINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
include(libs/link/AbletonLinkConfig.cmake)

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

# Pack QML files into the binary under URI "DJSoftware".
set(_rdbj_qml_module_options)
if(NOT RDBJ_ENABLE_QML_CACHEGEN)
    list(APPEND _rdbj_qml_module_options NO_CACHEGEN)
endif()

set_source_files_properties(
    src/qml/UiTheme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(${PROJECT_NAME}
    ${_rdbj_qml_module_options}
    URI DJSoftware
    VERSION 1.0
    RESOURCE_PREFIX "/"
    QML_FILES
        src/qml/main.qml
        src/qml/UiTheme.qml
        src/qml/Knob.qml
        src/qml/Button.qml
        src/qml/Slider.qml
        src/qml/ComboBox.qml
        src/qml/TopHeader.qml
        src/qml/EnlargedWaveform.qml
        src/qml/AioWaveformInfoSlot.qml
        src/qml/BeatgridEditorPanel.qml
        src/qml/OverallWaveform.qml
        src/qml/SegmentBar.qml
        src/qml/AnalysisProgressBar.qml
        src/qml/DeckControl.qml
        src/qml/TrackInfoDisplay.qml
        src/qml/Library.qml
        src/qml/PerformancePads.qml
        src/qml/TurntableIndicator.qml
        src/qml/MixerSection.qml
        src/qml/SettingsPanel.qml
        src/qml/SettingsWindow.qml
        src/qml/MappingEditorWindow.qml
        src/qml/WelcomeScreen.qml
        src/qml/FxUnit.qml
        src/qml/FxBar.qml
        src/qml/CrossfaderBar.qml
)

qt_add_resources(${PROJECT_NAME} "BrockDJ_controller_mappings"
    PREFIX "/controllers"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/controllers"
    FILES
        src/controllers/mappings/midi/DDJ-FLX10.brockdj.xml
)

target_sources(${PROJECT_NAME} PRIVATE
    src/main.cpp
    src/app/ApplicationBootstrap.cpp
    src/app/ApplicationLifecycle.cpp
    src/audio/device/AudioDeviceService.h
    src/audio/device/AudioDeviceService.cpp
    src/audio/device/AudioDeviceServiceQueries.cpp
    src/audio/cache/AudioPage.h
    src/audio/cache/AudioCacheHandle.h
    src/audio/cache/AudioPageCache.h
    src/audio/cache/AudioPageCache.cpp
    src/audio/cache/AudioCacheWorker.h
    src/audio/cache/AudioCacheWorker.cpp
    src/audio/cache/CachedPlaybackAudioSource.h
    src/audio/cache/CachedPlaybackAudioSource.cpp
    src/platform/PosixSignalHandler.h
    src/platform/PosixSignalHandler.cpp
    src/app/MixerControl.h
    src/app/MixerControl.cpp
    src/engine/DjEngine.h
    src/engine/DjEngine_Core.cpp
    src/engine/DjEngine_CoreMeta.cpp
    src/engine/DjEngine_CoreTempo.cpp
    src/engine/DjEngine_TransportExtras.cpp
    src/engine/DjEngine_Fx.cpp
    src/engine/DjEngine_CueLoopFacade.cpp
    src/engine/deck/DeckCueLoopController.cpp
    src/engine/deck/DeckCueLoopController.h
    src/engine/deck/DeckTrackLoader.cpp
    src/engine/deck/DeckTrackLoader.h
    src/engine/DjEngine_Scratch.cpp
    src/engine/DjEngine_Settings.cpp
    src/engine/DjEngine_SettingsQuery.cpp
    src/engine/DjEngine_Sync.cpp
    src/engine/DjEngine_Transport.cpp
    src/engine/DjEngine_TransportLatency.cpp
    src/engine/DjEngine_TransportLoad.cpp
    src/engine/audio/AudioDeviceUtils.cpp
    src/engine/audio/MetadataUtils.cpp
    src/engine/DjMasterBus.h
    src/engine/DjMasterBus.cpp
    src/engine/audio/TimeStretchAudioSource.cpp
    src/engine/audio/MixerDspSource.cpp
    src/analysis/PhraseAnalyzer.h
    src/analysis/PhraseAnalyzer.cpp
    src/analysis/AnalysisCacheVersion.h
    src/analysis/AnalysisResult.h
    src/analysis/AnalysisFeatureExtractor.h
    src/analysis/AnalysisFeatureExtractor.cpp
    src/analysis/TempoEstimator.h
    src/analysis/TempoEstimator.cpp
    src/analysis/BeatTracker.h
    src/analysis/BeatTracker.cpp
    src/analysis/DownbeatDetector.h
    src/analysis/DownbeatDetector.cpp
    src/analysis/BeatGridFitter.h
    src/analysis/BeatGridFitter.cpp
    src/analysis/AnalysisValidation.h
    src/analysis/AnalysisValidation.cpp
    src/rendering/RgbWaveformItem.h
    src/rendering/RgbWaveformItem.cpp
    src/rendering/ScrollingWaveformItem.cpp
    src/rendering/WaveformAnalyzer.cpp
    src/rendering/WaveformEnvelopePass.cpp
    src/rendering/WaveformAnalysisOrchestrator.cpp
    src/rendering/WaveformCache.h
    src/rendering/WaveformCache.cpp
    src/domain/TrackData.h
    src/domain/TrackData.cpp
    src/domain/TrackSegment.h
    src/library/LibraryManager.h
    src/library/LibraryManager.cpp
    src/library/CoverArtExtractor.h
    src/library/CoverArtExtractor.cpp
    src/library/CoverArtProvider.h
    src/library/LibraryCoverService.h
    src/library/LibraryCoverService.cpp
    src/library/LibraryPreviewPlayer.h
    src/library/LibraryPreviewPlayer.cpp
    src/fx/FxManager.h
    src/fx/FxManager.cpp
    src/fx/FxProcessor.h
    src/fx/FxProcessor.cpp
    src/link/LinkManager.h
    src/link/LinkManager.cpp
    src/app/SystemMonitor.h
    src/app/SystemMonitor.cpp
    src/app/LsanSuppressions.cpp
    src/app/CursorControl.h
    src/app/CursorControl.cpp
    src/midi/ParameterStore.h
    src/midi/ParameterStore.cpp
    src/midi/MixerParameterBridge.h
    src/midi/MixerParameterBridge.cpp
    src/midi/AlsaMidiOutput.h
    src/midi/AlsaMidiOutput.cpp
    src/midi/feedback/MidiFeedbackController.h
    src/midi/feedback/MidiFeedbackController.cpp
    src/midi/MidiControllerManager.h
    src/midi/MidiControllerManager.cpp
    src/midi/MidiDeviceEnumeration.cpp
    src/midi/MidiMappingLoader.cpp
    src/midi/MidiParameterDispatch.cpp
    src/midi/MidiFlx10Bridge.cpp
    src/controllers/ControllerProfile.h
    src/controllers/ControllerIntegrationManager.h
    src/controllers/ControllerIntegrationManager.cpp
    src/controllers/flx10/DDJFLX10Controller.h
    src/controllers/flx10/DDJFLX10Controller.cpp
    src/controllers/flx10/Flx10HidTransport.cpp
    src/controllers/flx10/Flx10DisplayProtocol.cpp
    src/app/SettingsManager.h
    src/app/SettingsManager.cpp
    src/app/AppConfig.h
    src/app/AppConfig.cpp
    src/app/AppExitGate.h
    src/library/TrackIdGenerator.h
    src/library/LibraryDatabase.h
    src/library/LibraryDatabase.cpp
    src/library/LibrarySchema.cpp
    src/library/LibraryTrackStore.cpp
    src/library/LibraryCueStore.cpp
    src/library/LibraryPlaylistStore.cpp
    src/library/LibraryAnalysisManager.h
    src/library/LibraryAnalysisManager.cpp
    src/library/LibraryTableModel.h
    src/library/LibraryTableModel.cpp
    src/fx/BrickwallLimiter.h
    src/fx/BrickwallLimiter.cpp
    src/engine/audio/HermiteResamplingAudioSource.h
    src/engine/audio/HermiteResamplingAudioSource.cpp
    src/engine/scratch/ScratchSession.hpp
    src/engine/scratch/ScratchSession.cpp
    src/engine/scratch/ScratchController.hpp
    src/engine/scratch/ScratchController.cpp
    src/engine/scratch/VirtualTurntable.hpp
    src/engine/scratch/VirtualTurntable.cpp
    src/engine/audio/ScratchResampler.hpp
    src/engine/audio/ScratchResampler.cpp
    src/engine/audio/ScratchDeckBridge.hpp
    src/engine/audio/ScratchDeckBridge.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    src/engine
    src/engine/scratch
    src/engine/audio
    src/rendering
    src/midi
    src/controllers
    src/midi/feedback
    src/fx/dsp
    src/analysis
    src/domain
    src/app
    ${SIGNALSMITH_DSP_DIR}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
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

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_26)

if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Apple SDK headers included through JUCE's Objective-C++ amalgamation still
    # contain cross-enum arithmetic that Clang treats as a hard error in C++26.
    # Keep the app target on C++26, but compile these vendor bridge units as C++23
    # until the SDK/JUCE combination is C++26-clean.
    file(GLOB_RECURSE _rdbj_juce_objcxx_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/JUCE/modules/*.mm")
    set_source_files_properties(${_rdbj_juce_objcxx_sources}
        PROPERTIES COMPILE_OPTIONS "-std=c++23")
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
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE AppIcon.icns
            MACOSX_BUNDLE_BUNDLE_NAME "BrockDJ"
            MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
            MACOSX_BUNDLE_COPYRIGHT "© Ramsbrock.net"
        )
    endif()
else()
    message(STATUS "Icon resources not found — run scripts/generate_icons.sh then re-run CMake.")
endif()

# ── Smoke tests ───────────────────────────────────────────────────────────────
enable_testing()

add_executable(BrockDJ_smoke_tests
    tests/smoke/smoke_tests.cpp
    tests/smoke/mixer_dsp_smoke.cpp
    src/analysis/AnalysisValidation.cpp
    src/engine/audio/MixerDspSource.cpp
    src/fx/FxProcessor.cpp
    src/fx/BrickwallLimiter.cpp
)

target_include_directories(BrockDJ_smoke_tests PRIVATE
    src
    src/analysis
    src/controllers
    src/domain
    src/engine
    src/engine/audio
    src/fx/dsp
    ${SIGNALSMITH_DSP_DIR}
)

target_link_libraries(BrockDJ_smoke_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    juce::juce_core
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_dsp
    juce::juce_events
)
target_compile_definitions(BrockDJ_smoke_tests PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)
target_compile_features(BrockDJ_smoke_tests PRIVATE cxx_std_26)

add_test(NAME smoke COMMAND BrockDJ_smoke_tests)

add_executable(BrockDJ_analysis_lifetime_tests
    tests/analysis/analysis_lifetime_tests.cpp
    src/rendering/WaveformAnalyzer.cpp
    src/rendering/WaveformEnvelopePass.cpp
    src/rendering/WaveformAnalysisOrchestrator.cpp
    src/rendering/WaveformCache.cpp
    src/domain/TrackData.cpp
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
target_compile_features(BrockDJ_analysis_lifetime_tests PRIVATE cxx_std_26)
add_test(NAME analysis_lifetime COMMAND BrockDJ_analysis_lifetime_tests)

if(UNIX)
    add_executable(BrockDJ_posix_signal_tests
        tests/platform/posix_signal_handler_tests.cpp
        src/platform/PosixSignalHandler.h
        src/platform/PosixSignalHandler.cpp
    )
    target_include_directories(BrockDJ_posix_signal_tests PRIVATE src)
    target_link_libraries(BrockDJ_posix_signal_tests PRIVATE Qt6::Core)
    target_compile_features(BrockDJ_posix_signal_tests PRIVATE cxx_std_26)
    add_test(NAME posix_signal_handler COMMAND BrockDJ_posix_signal_tests)
endif()

add_executable(BrockDJ_audio_device_service_tests
    tests/audio/audio_device_service_tests.cpp
    src/audio/device/AudioDeviceService.h
    src/audio/device/AudioDeviceService.cpp
    src/audio/device/AudioDeviceServiceQueries.cpp
    src/engine/audio/AudioDeviceUtils.cpp
)
target_include_directories(BrockDJ_audio_device_service_tests PRIVATE src src/engine)
target_link_libraries(BrockDJ_audio_device_service_tests PRIVATE
    Qt6::Core
    juce::juce_core juce::juce_audio_basics juce::juce_audio_devices
)
target_compile_definitions(BrockDJ_audio_device_service_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
)
target_compile_features(BrockDJ_audio_device_service_tests PRIVATE cxx_std_26)
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
target_compile_features(BrockDJ_cue_loop_controller_tests PRIVATE cxx_std_26)
add_test(NAME cue_loop_controller COMMAND BrockDJ_cue_loop_controller_tests)

add_executable(BrockDJ_track_loader_tests
    tests/track_loader/deck_track_loader_tests.cpp
    src/engine/deck/DeckTrackLoader.h
    src/engine/deck/DeckTrackLoader.cpp
    src/engine/audio/MetadataUtils.cpp
    src/library/CoverArtExtractor.cpp
    src/rendering/WaveformAnalyzer.cpp
    src/rendering/WaveformEnvelopePass.cpp
    src/rendering/WaveformAnalysisOrchestrator.cpp
    src/rendering/WaveformCache.cpp
    src/domain/TrackData.cpp
    src/analysis/AnalysisFeatureExtractor.cpp
    src/analysis/TempoEstimator.cpp
    src/analysis/BeatTracker.cpp
    src/analysis/DownbeatDetector.cpp
    src/analysis/BeatGridFitter.cpp
    src/analysis/AnalysisValidation.cpp
    src/analysis/PhraseAnalyzer.cpp
)
target_include_directories(BrockDJ_track_loader_tests PRIVATE
    src src/analysis src/domain src/engine src/library src/rendering
)
target_link_libraries(BrockDJ_track_loader_tests PRIVATE
    Qt6::Core Qt6::Gui
    juce::juce_core juce::juce_data_structures juce::juce_audio_basics
    juce::juce_audio_formats juce::juce_dsp juce::juce_events
    ThirdParty::TagLib ThirdParty::KeyFinder
)
target_compile_definitions(BrockDJ_track_loader_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
)
target_compile_features(BrockDJ_track_loader_tests PRIVATE cxx_std_26)
add_test(NAME track_loader COMMAND BrockDJ_track_loader_tests)

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
target_compile_features(BrockDJ_audio_page_cache_tests PRIVATE cxx_std_26)
add_test(NAME audio_page_cache COMMAND BrockDJ_audio_page_cache_tests)

add_executable(BrockDJ_scratch_cache_tests
    tests/scratch_cache/scratch_cache_tests.cpp
    src/audio/cache/AudioPageCache.cpp
    src/audio/cache/AudioCacheWorker.cpp
    src/engine/audio/ScratchResampler.cpp
)
target_include_directories(BrockDJ_scratch_cache_tests PRIVATE src src/engine)
target_link_libraries(BrockDJ_scratch_cache_tests PRIVATE
    Qt6::Core juce::juce_core juce::juce_audio_basics juce::juce_audio_formats
)
target_compile_definitions(BrockDJ_scratch_cache_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
)
target_compile_features(BrockDJ_scratch_cache_tests PRIVATE cxx_std_26)
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
target_compile_features(BrockDJ_cached_playback_tests PRIVATE cxx_std_26)
add_test(NAME cached_playback COMMAND BrockDJ_cached_playback_tests)
