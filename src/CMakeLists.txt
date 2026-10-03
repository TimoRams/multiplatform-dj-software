# Explicit production sources grouped by domain; the root owns target creation.
target_sources(${PROJECT_NAME} PRIVATE
    # Analysis
    analysis/internal/FeatureAnalysis.h
    analysis/internal/FeatureAnalysis.cpp
    analysis/AnalysisTypes.h
    analysis/internal/AnalysisWorkingData.h
    analysis/internal/BeatAnalysis.h
    analysis/internal/BeatAnalysis.cpp
    analysis/internal/AnalysisOrchestrator.h
    analysis/internal/AnalysisOrchestrator.cpp

    # Application
    app/ApplicationBootstrap.cpp
    app/CiSmokeTest.cpp
    app/ApplicationLifecycle.cpp
    app/ControlClock.cpp
    app/RenderPressurePolicy.h
    app/RenderPressurePolicy.cpp
    app/UiPreferences.h
    app/MixerControl.h
    app/MixerControl.cpp
    app/LsanSuppressions.cpp
    app/CursorControl.h
    app/SettingsManager.h
    app/SettingsManager.cpp

    # Audio
    audio/device/AudioDeviceService.h
    audio/device/AudioDeviceService.cpp
    audio/cache/AudioCacheTypes.h
    audio/cache/AudioPageCache.h
    audio/cache/AudioPageCache.cpp
    audio/cache/CachedPlaybackAudioSource.h
    audio/cache/CachedPlaybackAudioSource.cpp
    audio/DeckAudioPipeline.cpp
    audio/device/AudioDeviceUtils.cpp
    audio/AudioEngine.h
    audio/AudioEngine.cpp
    audio/AudioBusMixer.h
    audio/AudioBusMixer.cpp
    audio/TimeStretchProcessor.cpp
    audio/DeckChannelProcessor.cpp
    audio/internal/BrickwallLimiter.h
    audio/internal/BrickwallLimiter.cpp
    audio/internal/HermiteResamplingAudioSource.h
    audio/internal/HermiteResamplingAudioSource.cpp
    audio/internal/ScratchResampler.h
    audio/internal/ScratchResampler.cpp
    audio/RenderModeRouter.h
    audio/RenderModeRouter.cpp

    # Controllers
    controllers/ControllerIntegrationManager.h
    controllers/ControllerIntegrationManager.cpp
    controllers/midi/ParameterStore.h
    controllers/midi/AlsaMidiTransport.h
    controllers/midi/AlsaMidiTransport.cpp
    controllers/midi/MidiFeedbackController.h
    controllers/midi/MidiFeedbackController.cpp
    controllers/midi/LevelMeterBallistics.h
    controllers/midi/MidiInputState.h
    controllers/midi/MidiControllerManager.h
    controllers/midi/MidiParameterDispatch.h
    controllers/midi/MidiControllerManager.cpp
    controllers/midi/MidiDeviceEnumeration.cpp
    controllers/midi/MidiMappingLoader.cpp
    controllers/midi/MidiParameterDispatch.cpp
    controllers/flx10/Flx10ControllerIdentity.h
    controllers/flx10/DDJFLX10Controller.h
    controllers/flx10/DDJFLX10Controller.cpp
    controllers/flx10/Flx10HidTransport.cpp
    controllers/flx10/Flx10Display.cpp
    controllers/flx10/Flx10MidiBridge.cpp

    # Decks
    deck/DjEngine.h
    deck/DjEngine.cpp
    deck/DjEngineTransport.cpp
    deck/DjEngineCues.cpp
    deck/DjEnginePerformance.cpp
    deck/DeckCueLoopController.cpp
    deck/DeckCueLoopController.h
    deck/DeckTrackLoader.cpp
    deck/DeckTrackLoader.h
    deck/DeckTransport.cpp
    deck/DeckTransport.h
    deck/sync/DeckSync.cpp
    deck/sync/DeckSync.h
    deck/scratch/ScratchSession.h
    deck/scratch/ScratchSession.cpp
    deck/scratch/ScratchController.h
    deck/scratch/ScratchController.cpp
    deck/scratch/ScratchInput.h

    # Domain values
    domain/TrackData.h
    domain/TrackData.cpp
    domain/DomainTypes.h

    # Effects
    fx/FxTypes.h
    fx/FxPrimitives.h
    fx/FxManager.h
    fx/FxManager.cpp
    fx/FxProcessor.h
    fx/FxProcessor.cpp

    # Library and device sources
    library/MediaIoScheduler.h
    library/MediaIoScheduler.cpp
    library/LibraryManager.h
    library/LibraryManager.cpp
    library/CoverArtExtractor.h
    library/CoverArtExtractor.cpp
    library/LibraryCoverService.h
    library/LibraryCoverService.cpp
    library/LibraryPreviewPlayer.h
    library/LibraryPreviewPlayer.cpp
    library/LibraryDatabase.h
    library/LibraryDatabase.cpp
    library/LibraryPersistence.cpp
    library/persistence/DatabaseWorker.h
    library/persistence/DatabaseWorker.cpp
    library/LibraryAnalysisManager.h
    library/LibraryAnalysisManager.cpp
    library/LibraryTableModel.h
    library/LibraryTableModel.cpp
    library/devices/DeviceLibraryManager.h
    library/devices/DeviceLibraryManager.cpp
    library/devices/rekordbox/RekordboxTypes.h
    library/devices/rekordbox/RekordboxPdbReader.cpp
    library/devices/rekordbox/RekordboxAnalysisReader.cpp
    library/devices/rekordbox/RekordboxDeviceSource.h
    library/devices/rekordbox/RekordboxDeviceSource.cpp

    # Link and platform adapters
    link/LinkManager.h
    link/LinkManager.cpp
    platform/AudioThreadScheduling.h
    platform/AudioThreadScheduling.cpp
    platform/FileManagerLaunch.h
    platform/PosixSignalHandler.h
    platform/PosixSignalHandler.cpp
    platform/SystemMonitor.h
    platform/SystemMonitor.cpp

    # Waveform analysis and rendering
    waveform/WaveformTypes.h
    waveform/WaveformLineBuilder.h
    waveform/WaveformVisualStyle.h
    waveform/WaveformAggregator.h
    waveform/WaveformAggregator.cpp
    waveform/WaveformLineStore.cpp
    waveform/WaveformAnalyzer.h
    waveform/WaveformAnalyzer.cpp
    waveform/WaveformCache.h
    waveform/WaveformCache.cpp
    waveform/internal/WaveformEnvelopePass.h
    waveform/internal/WaveformEnvelopePass.cpp
    waveform/render/OverviewWaveformItem.h
    waveform/render/OverviewWaveformItem.cpp
    waveform/render/ScrollingWaveformItem.h
    waveform/render/ScrollingWaveformItem.cpp
    waveform/render/WaveformRenderMath.h
    waveform/render/WaveformTileRasterizer.h
    waveform/render/WaveformTileRasterizer.cpp
)

# Preserve the public QRC path despite the centralized manifest's source scope.
set_source_files_properties(controllers/mappings/DDJ-FLX10.brockdj.xml PROPERTIES
    QT_RESOURCE_ALIAS "mappings/midi/DDJ-FLX10.brockdj.xml")
qt_add_resources(${PROJECT_NAME} "BrockDJ_controller_mappings"
    PREFIX "/controllers"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/controllers"
    FILES controllers/mappings/DDJ-FLX10.brockdj.xml
)

# The root owns qt_add_qml_module(); its file paths are root-relative.
set(BROCKDJ_QML_FILES
    src/qml/main.qml
    src/qml/components/UiTheme.qml
    src/qml/components/Knob.qml
    src/qml/components/Button.qml
    src/qml/components/Slider.qml
    src/qml/components/ComboBox.qml
    src/qml/deck/DeckControl.qml
    src/qml/deck/DeckTrackInfoPanel.qml
    src/qml/waveform/EnlargedWaveform.qml
    src/qml/mixer/MixerSection.qml
    src/qml/mixer/FxBar.qml
    src/qml/performance/PerformanceWorkspace.qml
    src/qml/performance/PerformanceWaveformScreen.qml
    src/qml/performance/PerformancePads.qml
    src/qml/library/Library.qml
    src/qml/library/SourcePage.qml
    src/qml/settings/SettingsPanel.qml
    src/qml/settings/MappingEditorWindow.qml
    src/qml/shell/AppOverlays.qml
    src/qml/shell/TopHeader.qml
    src/qml/development/DevelopmentControlsWindow.qml
    PARENT_SCOPE
)
