# SPDX-License-Identifier: GPL-2.0-or-later

include(FindPackageHandleStandardArgs)

set(_ONNXRUNTIME_HINTS)
if(DEFINED ONNXRUNTIME_ROOT)
  list(APPEND _ONNXRUNTIME_HINTS "${ONNXRUNTIME_ROOT}")
endif()
if(DEFINED ENV{ONNXRUNTIME_ROOT})
  list(APPEND _ONNXRUNTIME_HINTS "$ENV{ONNXRUNTIME_ROOT}")
endif()

find_path(ONNXRUNTIME_INCLUDE_DIR
  NAMES onnxruntime_cxx_api.h
  HINTS ${_ONNXRUNTIME_HINTS}
  PATH_SUFFIXES include include/onnxruntime/core/session
)

find_library(ONNXRUNTIME_LIBRARY
  NAMES onnxruntime libonnxruntime
  HINTS ${_ONNXRUNTIME_HINTS}
  PATH_SUFFIXES lib lib64
)

find_file(ONNXRUNTIME_RUNTIME_LIBRARY
  NAMES onnxruntime.dll libonnxruntime.so libonnxruntime.dylib
  HINTS ${_ONNXRUNTIME_HINTS}
  PATH_SUFFIXES bin lib lib64
)

find_package_handle_standard_args(ONNXRuntime
  REQUIRED_VARS ONNXRUNTIME_INCLUDE_DIR ONNXRUNTIME_LIBRARY
)

if(ONNXRuntime_FOUND AND NOT TARGET ONNXRuntime::ONNXRuntime)
  add_library(ONNXRuntime::ONNXRuntime UNKNOWN IMPORTED)
  set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
  )
endif()
