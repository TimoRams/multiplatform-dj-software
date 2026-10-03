target_sources(${PROJECT_NAME} PRIVATE
    ControllerIntegrationManager.h
    ControllerIntegrationManager.cpp
    midi/ParameterStore.h
    midi/AlsaMidiTransport.h
    midi/AlsaMidiTransport.cpp
    midi/MidiFeedbackController.h
    midi/MidiFeedbackController.cpp
    midi/LevelMeterBallistics.h
    midi/MidiInputState.h
    midi/MidiControllerManager.h
    midi/MidiParameterDispatch.h
    midi/MidiControllerManager.cpp
    midi/MidiDeviceEnumeration.cpp
    midi/MidiMappingLoader.cpp
    midi/MidiParameterDispatch.cpp
    flx10/Flx10ControllerIdentity.h
    flx10/DDJFLX10Controller.h
    flx10/DDJFLX10Controller.cpp
    flx10/Flx10HidTransport.cpp
    flx10/Flx10Display.cpp
    flx10/Flx10MidiBridge.cpp
)

set_source_files_properties(mappings/DDJ-FLX10.brockdj.xml PROPERTIES
    QT_RESOURCE_ALIAS "mappings/midi/DDJ-FLX10.brockdj.xml")

qt_add_resources(${PROJECT_NAME} "BrockDJ_controller_mappings"
    PREFIX "/controllers"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        mappings/DDJ-FLX10.brockdj.xml
)
