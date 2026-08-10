target_sources(${PROJECT_NAME} PRIVATE
    ParameterStore.h
    ParameterStore.cpp
    MixerParameterBridge.h
    MixerParameterBridge.cpp
    AlsaMidiOutput.h
    AlsaMidiOutput.cpp
    feedback/MidiFeedbackController.h
    feedback/MidiFeedbackController.cpp
    MidiControllerManager.h
    MidiControllerManager.cpp
    MidiDeviceEnumeration.cpp
    MidiMappingLoader.cpp
    MidiParameterDispatch.cpp
    MidiFlx10Bridge.cpp
)
