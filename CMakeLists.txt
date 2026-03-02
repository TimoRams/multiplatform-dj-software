cmake_minimum_required(VERSION 3.22)

set(PROJECT_NAME "MultiPlatformDJ")
project(${PROJECT_NAME} VERSION 0.1.0 LANGUAGES C CXX)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache for faster compilation")
endif()

add_subdirectory(libs/JUCE)

juce_add_console_app(${PROJECT_NAME}
    PRODUCT_NAME "Multiplatform DJ Software"
    COMPANY_NAME "Ramsbrock.net"
)

target_sources(${PROJECT_NAME} PRIVATE
    src/main.cpp
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    juce::juce_core
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)