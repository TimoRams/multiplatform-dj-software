# Build ORT in an isolated CMake project: its compiler flags, dependencies and
# install rules must not leak into the application or its packaging.
include(ExternalProject)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
if(NOT CMAKE_HOST_WIN32)
    find_program(Patch_EXECUTABLE NAMES patch HINTS "$ENV{HOME}/.local/bin")
    find_package(Patch REQUIRED)
endif()
set(BROCKDJ_ONNXRUNTIME_BUILD_JOBS 2 CACHE STRING "Parallel jobs for the ONNX Runtime source build")
set(_ort_source "${PROJECT_SOURCE_DIR}/libs/onnxruntime-src")
if(NOT EXISTS "${_ort_source}/cmake/CMakeLists.txt")
    message(FATAL_ERROR "ONNX Runtime submodule missing. Run git submodule update --init --recursive, or disable BROCKDJ_ENABLE_STEMS.")
endif()
if(CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR "Building bundled ONNX Runtime requires CMake 3.28 or newer")
endif()
set(_ort_prefix "${CMAKE_BINARY_DIR}/onnxruntime")
set(_ort_lib_dir "${_ort_prefix}/lib")
set(_ort_args
    -DPython_EXECUTABLE:FILEPATH=${Python3_EXECUTABLE}
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_CXX_STANDARD=20
    -DCMAKE_INSTALL_PREFIX:PATH=${_ort_prefix}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
    -Donnxruntime_BUILD_SHARED_LIB=ON
    -Donnxruntime_BUILD_UNIT_TESTS=OFF
    -Donnxruntime_BUILD_BENCHMARKS=OFF
    -Donnxruntime_ENABLE_PYTHON=OFF
    -Donnxruntime_USE_CUDA=OFF
    -Donnxruntime_USE_TENSORRT=OFF
    -Donnxruntime_USE_VCPKG=OFF
    -Donnxruntime_ENABLE_LTO=OFF)
if(Patch_EXECUTABLE)
    list(APPEND _ort_args -DPatch_EXECUTABLE:FILEPATH=${Patch_EXECUTABLE})
endif()
foreach(_var CMAKE_TOOLCHAIN_FILE CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT CMAKE_SYSROOT CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER)
    if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
        string(REPLACE ";" "|" _value "${${_var}}")
        list(APPEND _ort_args "-D${_var}:STRING=${_value}")
    endif()
endforeach()
if(APPLE)
    list(APPEND _ort_args -Donnxruntime_USE_COREML=ON)
endif()
if(WIN32)
    set(ONNXRUNTIME_LIBRARY "${_ort_lib_dir}/onnxruntime.lib")
    set(ONNXRUNTIME_RUNTIME_DLL "${_ort_prefix}/bin/onnxruntime.dll")
else()
    set(ONNXRUNTIME_LIBRARY "${_ort_lib_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}onnxruntime${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()
ExternalProject_Add(brockdj_onnxruntime
    SOURCE_DIR "${_ort_source}"
    SOURCE_SUBDIR cmake
    BINARY_DIR "${CMAKE_BINARY_DIR}/onnxruntime-build"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
    LIST_SEPARATOR |
    CMAKE_ARGS --compile-no-warning-as-error ${_ort_args}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --parallel ${BROCKDJ_ONNXRUNTIME_BUILD_JOBS}
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release
    INSTALL_BYPRODUCTS "${ONNXRUNTIME_LIBRARY}" ${ONNXRUNTIME_RUNTIME_DLL})
# ExternalProject stamps must depend on local sources, rather than forcing
# build/install on every application build. The inner build system handles
# recompilation and CMake regeneration once a source actually changes.
file(GLOB_RECURSE _ort_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${_ort_source}/cmake/*"
    "${_ort_source}/include/*"
    "${_ort_source}/onnxruntime/*"
    "${_ort_source}/tools/*")
list(APPEND _ort_inputs "${_ort_source}/VERSION_NUMBER")
# A changed file list also invalidates the step when files are added/deleted.
string(JOIN "\n" _ort_input_manifest ${_ort_inputs})
set(_ort_manifest "${CMAKE_CURRENT_BINARY_DIR}/onnxruntime-source-files.txt")
file(GENERATE OUTPUT "${_ort_manifest}" CONTENT "${_ort_input_manifest}\n")
ExternalProject_Add_Step(brockdj_onnxruntime source_changes
    COMMAND ${CMAKE_COMMAND} -E true
    DEPENDEES configure
    DEPENDERS build
    DEPENDS ${_ort_inputs} "${_ort_manifest}")
# Installation may preserve timestamps with coarser precision than Ninja.
# Publish a revision only after an actual install, so consumers relink even
# for rapid successive edits, while idle builds leave the revision untouched.
set(_ort_revision "${CMAKE_CURRENT_BINARY_DIR}/onnxruntime-installed.stamp")
ExternalProject_Add_Step(brockdj_onnxruntime published
    COMMAND ${CMAKE_COMMAND} -E touch "${_ort_revision}"
    DEPENDEES install
    BYPRODUCTS "${_ort_revision}")
# Public headers are already present in the submodule before ORT is built.
set(ONNXRUNTIME_INCLUDE_DIR "${_ort_source}/include/onnxruntime/core/session")
_rdbj_define_imported_binary_target(ThirdPartyOnnxRuntime "${ONNXRUNTIME_INCLUDE_DIR}" "${ONNXRUNTIME_LIBRARY}")
add_dependencies(ThirdPartyOnnxRuntime brockdj_onnxruntime)
set_property(TARGET ThirdPartyOnnxRuntime APPEND PROPERTY INTERFACE_LINK_DEPENDS "${_ort_revision}")
set(BROCKDJ_HAS_ONNXRUNTIME 1)
message(STATUS "Stems: building bundled ONNX Runtime from source")
