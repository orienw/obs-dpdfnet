# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.24)

project(obs-dpdfnet VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(DPDFNET_FETCH_KISSFFT "Fetch KissFFT at configure time" ON)
option(DPDFNET_BUILD_MODEL_SMOKE "Build the standalone DPDFNet model smoke test" OFF)
option(DPDFNET_BUILD_STREAM_DUMP "Build the standalone DPDFNet stream parity dumper" OFF)
option(DPDFNET_COPY_RUNTIME_DEPENDENCIES "Copy ONNX Runtime shared libraries next to CMake-built binaries" ON)
option(DPDFNET_INSTALL_RUNTIME_DEPENDENCIES "Install ONNX Runtime shared libraries with the OBS plugin" ON)

set(DPDFNET_PLUGIN_DESTINATION "obs-plugins/64bit" CACHE STRING "Install destination for the OBS plugin binary")
set(DPDFNET_DATA_DESTINATION "data/obs-plugins/obs-dpdfnet" CACHE STRING "Install destination for OBS plugin data")

set(DPDFNET_PLATFORM_COMPILE_DEFINITIONS)
if(WIN32)
  list(APPEND DPDFNET_PLATFORM_COMPILE_DEFINITIONS NOMINMAX)
endif()

find_package(libobs CONFIG REQUIRED)
find_package(ONNXRuntime REQUIRED)

if(DPDFNET_FETCH_KISSFFT)
  include(FetchContent)
  set(KISSFFT_DATATYPE "float" CACHE STRING "" FORCE)
  set(KISSFFT_OPENMP OFF CACHE BOOL "" FORCE)
  set(KISSFFT_PKGCONFIG OFF CACHE BOOL "" FORCE)
  set(KISSFFT_STATIC ON CACHE BOOL "" FORCE)
  set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
  set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    kissfft
    GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
    GIT_TAG 131.1.0
  )
  FetchContent_MakeAvailable(kissfft)
  set(DPDFNET_KISSFFT_TARGET kissfft::kissfft)
else()
  find_package(kissfft CONFIG REQUIRED COMPONENTS float)
  set(DPDFNET_KISSFFT_TARGET kissfft::kissfft)
endif()

function(dpdfnet_copy_runtime_dependencies target)
  if(NOT DPDFNET_COPY_RUNTIME_DEPENDENCIES)
    return()
  endif()

  foreach(runtime_dependency IN LISTS ONNXRUNTIME_RUNTIME_DEPENDENCIES)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${runtime_dependency}"
        "$<TARGET_FILE_DIR:${target}>"
      VERBATIM
    )
  endforeach()
endfunction()

add_library(obs-dpdfnet MODULE
  src/dpdfnet-filter.cpp
  src/dpdfnet-model.cpp
  src/plugin-main.cpp
  src/stft.cpp
)

target_compile_definitions(obs-dpdfnet PRIVATE
  PLUGIN_NAME="obs-dpdfnet"
  PLUGIN_VERSION="${PROJECT_VERSION}"
  ${DPDFNET_PLATFORM_COMPILE_DEFINITIONS}
)

target_link_libraries(obs-dpdfnet PRIVATE
  OBS::libobs
  ONNXRuntime::ONNXRuntime
  ${DPDFNET_KISSFFT_TARGET}
)

dpdfnet_copy_runtime_dependencies(obs-dpdfnet)

if(DPDFNET_BUILD_MODEL_SMOKE)
  add_executable(dpdfnet-model-smoke
    tools/model-smoke.cpp
    src/dpdfnet-model.cpp
    src/stft.cpp
  )
  target_link_libraries(dpdfnet-model-smoke PRIVATE
    ONNXRuntime::ONNXRuntime
    ${DPDFNET_KISSFFT_TARGET}
  )
  if(DPDFNET_PLATFORM_COMPILE_DEFINITIONS)
    target_compile_definitions(dpdfnet-model-smoke PRIVATE
      ${DPDFNET_PLATFORM_COMPILE_DEFINITIONS}
    )
  endif()
  dpdfnet_copy_runtime_dependencies(dpdfnet-model-smoke)
endif()

if(DPDFNET_BUILD_STREAM_DUMP)
  add_executable(dpdfnet-stream-dump
    tools/stream-dump.cpp
    src/dpdfnet-model.cpp
    src/stft.cpp
  )
  target_link_libraries(dpdfnet-stream-dump PRIVATE
    ONNXRuntime::ONNXRuntime
    ${DPDFNET_KISSFFT_TARGET}
  )
  if(DPDFNET_PLATFORM_COMPILE_DEFINITIONS)
    target_compile_definitions(dpdfnet-stream-dump PRIVATE
      ${DPDFNET_PLATFORM_COMPILE_DEFINITIONS}
    )
  endif()
  dpdfnet_copy_runtime_dependencies(dpdfnet-stream-dump)
endif()

if(MSVC)
  target_compile_options(obs-dpdfnet PRIVATE /W4 /permissive-)
else()
  target_compile_options(obs-dpdfnet PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(obs-dpdfnet PROPERTIES
  PREFIX ""
  OUTPUT_NAME "obs-dpdfnet"
)

install(TARGETS obs-dpdfnet
  RUNTIME DESTINATION "${DPDFNET_PLUGIN_DESTINATION}"
  LIBRARY DESTINATION "${DPDFNET_PLUGIN_DESTINATION}"
)

install(DIRECTORY data/
  DESTINATION "${DPDFNET_DATA_DESTINATION}"
)

install(DIRECTORY models/
  DESTINATION "${DPDFNET_DATA_DESTINATION}/models"
  FILES_MATCHING
    PATTERN "*.onnx"
    PATTERN "manifest.json"
)

install(FILES LICENSE THIRD_PARTY.md
  DESTINATION "${DPDFNET_DATA_DESTINATION}"
)

install(DIRECTORY LICENSES
  DESTINATION "${DPDFNET_DATA_DESTINATION}"
)

# Ships ONNX Runtime under its stock name. Unlike the PowerShell release path,
# this does not rename onnxruntime.dll, so a CMake-installed plugin can collide
# with another ONNX-based OBS plugin's onnxruntime.dll in the shared
# obs-plugins/64bit directory. Fine for isolated dev builds, not for shipping.
if(DPDFNET_INSTALL_RUNTIME_DEPENDENCIES)
  foreach(runtime_dependency IN LISTS ONNXRUNTIME_RUNTIME_DEPENDENCIES)
    install(FILES "${runtime_dependency}"
      DESTINATION "${DPDFNET_PLUGIN_DESTINATION}"
    )
  endforeach()
endif()
