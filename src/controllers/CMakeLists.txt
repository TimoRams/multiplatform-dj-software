target_sources(${PROJECT_NAME} PRIVATE
    ControllerProfile.h
    ControllerIntegrationManager.h
    ControllerIntegrationManager.cpp
    flx10/DDJFLX10Controller.h
    flx10/DDJFLX10Controller.cpp
    flx10/Flx10HidTransport.cpp
    flx10/Flx10DisplayProtocol.cpp
)

qt_add_resources(${PROJECT_NAME} "BrockDJ_controller_mappings"
    PREFIX "/controllers"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        mappings/midi/DDJ-FLX10.brockdj.xml
)
