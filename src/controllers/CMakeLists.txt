target_sources(${PROJECT_NAME} PRIVATE
    ControllerIntegrationManager.h
    ControllerIntegrationManager.cpp
    midi/ParameterStore.h
    midi/ParameterStore.cpp
    midi/MixerParameterBridge.h
    midi/MixerParameterBridge.cpp
    midi/AlsaMidiInput.h
    midi/AlsaMidiInput.cpp
    midi/AlsaMidiOutput.h
    midi/AlsaMidiOutput.cpp
    midi/feedback/MidiFeedbackController.h
    midi/feedback/MidiFeedbackController.cpp
    midi/MidiControllerManager.h
    midi/MidiControllerManager.cpp
    midi/MidiDeviceEnumeration.cpp
    midi/MidiMappingLoader.cpp
    midi/MidiParameterDispatch.cpp
    flx10/DDJFLX10Controller.h
    flx10/DDJFLX10Controller.cpp
    flx10/Flx10HidTransport.cpp
    flx10/Flx10Display.cpp
    flx10/Flx10MidiBridge.cpp
)

qt_add_resources(${PROJECT_NAME} "BrockDJ_controller_mappings"
    PREFIX "/controllers"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        mappings/midi/DDJ-FLX10.brockdj.xml
)
