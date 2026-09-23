# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.24)

project(obs-dpdfnet VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(DPDFNET_FETCH_KISSFFT "Fetch KissFFT at configure time" ON)
option(DPDFNET_BUILD_MODEL_SMOKE "Build the standalone DPDFNet model smoke test" OFF)
option(DPDFNET_BUILD_STREAM_DUMP "Build the standalone DPDFNet stream parity dumper" OFF)
option(DPDFNET_BUILD_QUALITY_BENCHMARK "Build the opt-in local quality benchmark" OFF)
option(DPDFNET_BUILD_PROCESSOR_BENCHMARK "Build the processor timing benchmark" OFF)
option(DPDFNET_BUILD_TESTS "Build processor, model contract, and filter lifecycle tests" OFF)
option(DPDFNET_COPY_RUNTIME_DEPENDENCIES "Copy ONNX Runtime shared libraries next to CMake-built binaries" ON)
option(DPDFNET_INSTALL_RUNTIME_DEPENDENCIES "Install ONNX Runtime shared libraries with the OBS plugin" ON)

set(DPDFNET_PLUGIN_VERSION "${PROJECT_VERSION}")
set(DPDFNET_PLUGIN_DESTINATION "obs-plugins/64bit" CACHE STRING "Install destination for the OBS plugin binary")
set(DPDFNET_DATA_DESTINATION "data/obs-plugins/obs-dpdfnet" CACHE STRING "Install destination for OBS plugin data")

set(DPDFNET_PLATFORM_COMPILE_DEFINITIONS)
if(WIN32)
  list(APPEND DPDFNET_PLATFORM_COMPILE_DEFINITIONS NOMINMAX)
endif()

find_package(libobs CONFIG REQUIRED)
find_package(ONNXRuntime REQUIRED)
find_package(Threads REQUIRED)

# The plugin links and ships these. On Linux the bundled ONNX Runtime gets a
# private soname: the dynamic loader shares libraries by soname across all
# plugins, so another ONNX-based plugin's libonnxruntime.so.1 would otherwise
# replace ours, or ours theirs, whichever loads first.
set(DPDFNET_ONNXRUNTIME_TARGET ONNXRuntime::ONNXRuntime)
set(DPDFNET_PLUGIN_RUNTIME_DEPENDENCIES ${ONNXRUNTIME_RUNTIME_DEPENDENCIES})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND DPDFNET_INSTALL_RUNTIME_DEPENDENCIES)
  find_program(DPDFNET_PATCHELF patchelf)
  if(DPDFNET_PATCHELF)
    set(private_onnxruntime_name "libonnxruntime_dpdfnet.so")
    set(private_onnxruntime
      "${CMAKE_BINARY_DIR}/onnxruntime-private/${private_onnxruntime_name}")
    file(REAL_PATH "${ONNXRUNTIME_LIBRARY}" stock_onnxruntime)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/onnxruntime-private")
    file(COPY_FILE "${stock_onnxruntime}" "${private_onnxruntime}")
    execute_process(
      COMMAND "${DPDFNET_PATCHELF}" --set-soname "${private_onnxruntime_name}"
        "${private_onnxruntime}"
      RESULT_VARIABLE patchelf_result
    )
    if(NOT patchelf_result EQUAL 0)
      message(FATAL_ERROR "patchelf could not rename ${private_onnxruntime}")
    endif()
    add_library(ONNXRuntime::Private SHARED IMPORTED)
    set_target_properties(ONNXRuntime::Private PROPERTIES
      IMPORTED_LOCATION "${private_onnxruntime}"
      IMPORTED_SONAME "${private_onnxruntime_name}"
      INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
    )
    set(DPDFNET_ONNXRUNTIME_TARGET ONNXRuntime::Private)
    set(DPDFNET_PLUGIN_RUNTIME_DEPENDENCIES "${private_onnxruntime}")
    if(ONNXRUNTIME_PROVIDERS_SHARED_LIBRARY)
      list(APPEND DPDFNET_PLUGIN_RUNTIME_DEPENDENCIES
        "${ONNXRUNTIME_PROVIDERS_SHARED_LIBRARY}")
    endif()
  else()
    message(WARNING "patchelf was not found, so the bundled ONNX Runtime keeps "
      "its stock soname and can clash with another ONNX-based OBS plugin. "
      "Install patchelf and reconfigure to give it a private name.")
  endif()
endif()

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

# Copies ONNX Runtime next to a target, or the given libraries instead.
function(dpdfnet_copy_runtime_dependencies target)
  if(NOT DPDFNET_COPY_RUNTIME_DEPENDENCIES)
    return()
  endif()

  set(runtime_dependencies ${ARGN})
  if(NOT runtime_dependencies)
    set(runtime_dependencies ${ONNXRUNTIME_RUNTIME_DEPENDENCIES})
  endif()
  foreach(runtime_dependency IN LISTS runtime_dependencies)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE=${runtime_dependency}"
        "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy-runtime.cmake"
      VERBATIM
    )
  endforeach()
endfunction()

add_library(obs-dpdfnet MODULE
  src/dpdfnet-filter.cpp
  src/dpdfnet-model.cpp
  src/dpdfnet-processor.cpp
  src/dpdfnet-settings.cpp
  src/plugin-main.cpp
  src/stft.cpp
)

target_compile_definitions(obs-dpdfnet PRIVATE
  PLUGIN_NAME="obs-dpdfnet"
  PLUGIN_VERSION="${DPDFNET_PLUGIN_VERSION}"
  ${DPDFNET_PLATFORM_COMPILE_DEFINITIONS}
)

target_link_libraries(obs-dpdfnet PRIVATE
  OBS::libobs
  ${DPDFNET_ONNXRUNTIME_TARGET}
  Threads::Threads
  ${DPDFNET_KISSFFT_TARGET}
)

dpdfnet_copy_runtime_dependencies(obs-dpdfnet
  ${DPDFNET_PLUGIN_RUNTIME_DEPENDENCIES})

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
    src/dpdfnet-processor.cpp
    src/stft.cpp
  )
  target_link_libraries(dpdfnet-stream-dump PRIVATE
    OBS::libobs
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

if(DPDFNET_BUILD_QUALITY_BENCHMARK)
  add_executable(dpdfnet-quality-benchmark
    tools/quality-benchmark.cpp
    src/dpdfnet-model.cpp
    src/dpdfnet-processor.cpp
    src/stft.cpp
  )
  target_link_libraries(dpdfnet-quality-benchmark PRIVATE
    OBS::libobs
    ONNXRuntime::ONNXRuntime
    ${DPDFNET_KISSFFT_TARGET}
  )
  if(DPDFNET_PLATFORM_COMPILE_DEFINITIONS)
    target_compile_definitions(dpdfnet-quality-benchmark PRIVATE
      ${DPDFNET_PLATFORM_COMPILE_DEFINITIONS}
    )
  endif()
  dpdfnet_copy_runtime_dependencies(dpdfnet-quality-benchmark)
endif()

if(DPDFNET_BUILD_PROCESSOR_BENCHMARK)
  add_executable(dpdfnet-processor-benchmark
    tools/processor-benchmark.cpp
    src/dpdfnet-model.cpp
    src/dpdfnet-processor.cpp
    src/stft.cpp
  )
  target_link_libraries(dpdfnet-processor-benchmark PRIVATE
    OBS::libobs
    ONNXRuntime::ONNXRuntime
    ${DPDFNET_KISSFFT_TARGET}
  )
  if(DPDFNET_PLATFORM_COMPILE_DEFINITIONS)
    target_compile_definitions(dpdfnet-processor-benchmark PRIVATE
      ${DPDFNET_PLATFORM_COMPILE_DEFINITIONS}
    )
  endif()
  dpdfnet_copy_runtime_dependencies(dpdfnet-processor-benchmark)
endif()

if(DPDFNET_BUILD_TESTS)
  enable_testing()
  add_executable(obs-dpdfnet-tests
    tests/dpdfnet-tests.cpp
    src/dpdfnet-model.cpp
    src/dpdfnet-processor.cpp
    src/dpdfnet-settings.cpp
    src/stft.cpp
  )
  target_link_libraries(obs-dpdfnet-tests PRIVATE
    OBS::libobs
    ONNXRuntime::ONNXRuntime
    ${DPDFNET_KISSFFT_TARGET}
  )
  if(DPDFNET_PLATFORM_COMPILE_DEFINITIONS)
    target_compile_definitions(obs-dpdfnet-tests PRIVATE
      ${DPDFNET_PLATFORM_COMPILE_DEFINITIONS}
    )
  endif()
  dpdfnet_copy_runtime_dependencies(obs-dpdfnet-tests)

  add_executable(obs-dpdfnet-filter-tests
    tests/dpdfnet-filter-tests.cpp
    src/dpdfnet-filter.cpp
    src/dpdfnet-model.cpp
    src/dpdfnet-processor.cpp
    src/dpdfnet-settings.cpp
    src/stft.cpp
  )
  target_compile_definitions(obs-dpdfnet-filter-tests PRIVATE
    PLUGIN_NAME="obs-dpdfnet"
    PLUGIN_VERSION="${DPDFNET_PLUGIN_VERSION}"
    ${DPDFNET_PLATFORM_COMPILE_DEFINITIONS}
  )
  target_link_libraries(obs-dpdfnet-filter-tests PRIVATE
    OBS::libobs
    ONNXRuntime::ONNXRuntime
    Threads::Threads
    ${DPDFNET_KISSFFT_TARGET}
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Match Windows DLL isolation: count plugin allocations, not allocations
    # inside shared dependencies that ELF symbol interposition would intercept.
    target_link_options(obs-dpdfnet-filter-tests PRIVATE
      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/filter-allocations.ld"
    )
    set_property(TARGET obs-dpdfnet-filter-tests APPEND PROPERTY LINK_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/filter-allocations.ld"
    )
  endif()
  dpdfnet_copy_runtime_dependencies(obs-dpdfnet-filter-tests)

  add_executable(dpdfnet-model-contract-test
    tests/model-contract-test.cpp
    src/dpdfnet-model.cpp
  )
  target_link_libraries(dpdfnet-model-contract-test PRIVATE
    ONNXRuntime::ONNXRuntime
  )
  if(DPDFNET_PLATFORM_COMPILE_DEFINITIONS)
    target_compile_definitions(dpdfnet-model-contract-test PRIVATE
      ${DPDFNET_PLATFORM_COMPILE_DEFINITIONS}
    )
  endif()
  dpdfnet_copy_runtime_dependencies(dpdfnet-model-contract-test)

  add_test(NAME model-contract COMMAND dpdfnet-model-contract-test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/model-contract"
    "${CMAKE_CURRENT_SOURCE_DIR}/models/dpdfnet8_48khz_hr.onnx"
    "${CMAKE_CURRENT_SOURCE_DIR}/models/dpdfnet2_48khz_hr.onnx"
  )
  add_test(NAME processor COMMAND obs-dpdfnet-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/models/dpdfnet8_48khz_hr.onnx"
    "${CMAKE_CURRENT_SOURCE_DIR}/models/dpdfnet2_48khz_hr.onnx"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/model-contract"
  )
  add_test(NAME filter-integration COMMAND obs-dpdfnet-filter-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/models/dpdfnet8_48khz_hr.onnx"
  )
  if(WIN32)
    # Use the runtime directory from the imported OBS target. This also makes
    # obs.dll's own adjacent dependencies available without relying on the
    # shell's ambient PATH or copying only part of the OBS runtime.
    set_tests_properties(processor filter-integration PROPERTIES
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>"
    )
  endif()
endif()

if(MSVC)
  target_compile_options(obs-dpdfnet PRIVATE /W4 /permissive-)
  foreach(target dpdfnet-model-smoke dpdfnet-stream-dump
                 dpdfnet-quality-benchmark dpdfnet-processor-benchmark
                 obs-dpdfnet-tests obs-dpdfnet-filter-tests
                 dpdfnet-model-contract-test)
    if(TARGET ${target})
      target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif()
  endforeach()
else()
  target_compile_options(obs-dpdfnet PRIVATE -Wall -Wextra -Wpedantic)
  foreach(target dpdfnet-model-smoke dpdfnet-stream-dump
                 dpdfnet-quality-benchmark dpdfnet-processor-benchmark
                 obs-dpdfnet-tests obs-dpdfnet-filter-tests
                 dpdfnet-model-contract-test)
    if(TARGET ${target})
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endforeach()
endif()

# Export only the OBS entry points, which obs-module.h marks explicitly. GCC
# gives inline statics such as the ONNX Runtime C++ wrapper's cached API table
# STB_GNU_UNIQUE binding, and the loader shares those across plugins even under
# RTLD_LOCAL, so another ONNX-based plugin could hand us its runtime.
set_target_properties(obs-dpdfnet PROPERTIES
  PREFIX ""
  OUTPUT_NAME "obs-dpdfnet"
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Static libraries such as KissFFT and standard-library template
  # instantiations stay private too.
  target_link_options(obs-dpdfnet PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.ld"
  )
  set_property(TARGET obs-dpdfnet APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.ld"
  )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND DPDFNET_INSTALL_RUNTIME_DEPENDENCIES)
  set_property(TARGET obs-dpdfnet APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
endif()

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

# On Windows and macOS this ships ONNX Runtime under its stock name. Unlike the
# PowerShell release path, it does not rename onnxruntime.dll, so a
# CMake-installed plugin can collide with another ONNX-based OBS plugin's
# onnxruntime.dll in the shared obs-plugins/64bit directory. Fine for isolated
# dev builds, not for shipping. Linux installs the private copy made above.
if(DPDFNET_INSTALL_RUNTIME_DEPENDENCIES)
  if(IS_ABSOLUTE "${DPDFNET_PLUGIN_DESTINATION}")
    set(runtime_destination "${DPDFNET_PLUGIN_DESTINATION}")
  else()
    set(runtime_destination "\${CMAKE_INSTALL_PREFIX}/${DPDFNET_PLUGIN_DESTINATION}")
  endif()
  foreach(runtime_dependency IN LISTS DPDFNET_PLUGIN_RUNTIME_DEPENDENCIES)
    install(CODE "file(INSTALL \"${runtime_dependency}\"
      DESTINATION \"${runtime_destination}\" FOLLOW_SYMLINK_CHAIN)")
  endforeach()
endif()
