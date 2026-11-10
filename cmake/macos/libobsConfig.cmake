# SPDX-License-Identifier: GPL-2.0-or-later

# OBS publishes no development files for macOS, and OBS.app carries libobs
# without its headers. This stands in for the libobs package: the headers come
# from an OBS source tree and the library from an OBS.app.
#
#   -Dlibobs_DIR=<this directory>
#   -DOBS_SOURCE_DIR=<obs-studio source tree>
#   -DOBS_APP=<OBS.app>
#
# Take both from the oldest OBS the build should load into: OBS refuses a
# plugin built against a newer version than itself.

set(OBS_SOURCE_DIR "" CACHE PATH "OBS Studio source tree with the libobs headers")
set(OBS_APP "/Applications/OBS.app" CACHE PATH "OBS.app with libobs.framework")

set(_libobs_library
  "${OBS_APP}/Contents/Frameworks/libobs.framework/Versions/A/libobs")
if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs-module.h")
  message(FATAL_ERROR "OBS_SOURCE_DIR must point at an OBS Studio source "
    "tree, not '${OBS_SOURCE_DIR}'.")
endif()
if(NOT EXISTS "${_libobs_library}")
  message(FATAL_ERROR "OBS_APP must point at an OBS.app, not '${OBS_APP}'.")
endif()

# The libobs vector headers include SIMDe.
find_path(SIMDE_INCLUDE_DIR NAMES simde/x86/sse2.h REQUIRED)

# obs-config.h includes this header, which the OBS build generates.
set(_libobs_generated "${CMAKE_BINARY_DIR}/libobs-generated")
file(CONFIGURE OUTPUT "${_libobs_generated}/obsconfig.h" CONTENT
"#pragma once
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
")

if(NOT TARGET OBS::libobs)
  add_library(OBS::libobs SHARED IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES
    FRAMEWORK TRUE
    IMPORTED_LOCATION "${_libobs_library}"
    INTERFACE_INCLUDE_DIRECTORIES
      "${OBS_SOURCE_DIR}/libobs;${_libobs_generated};${SIMDE_INCLUDE_DIR}"
  )
endif()
