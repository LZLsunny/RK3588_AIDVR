include_guard(GLOBAL)

# This module is intentionally strict for cross builds:
# - only search within the provided sysroot
# - fail early with a clear message when required libs are missing
#
# For host-only builds, it stays inert unless ROCKCHIP_SDK_SYSROOT is set.

function(_aimedia_require_sysroot)
  if(NOT DEFINED ROCKCHIP_SDK_SYSROOT OR ROCKCHIP_SDK_SYSROOT STREQUAL "")
    message(FATAL_ERROR "ROCKCHIP_SDK_SYSROOT is required for SDK/cross builds (set -DROCKCHIP_SDK_SYSROOT=/path/to/sysroot).")
  endif()
  if(NOT IS_DIRECTORY "${ROCKCHIP_SDK_SYSROOT}")
    message(FATAL_ERROR "ROCKCHIP_SDK_SYSROOT does not exist: ${ROCKCHIP_SDK_SYSROOT}")
  endif()
endfunction()

function(_aimedia_import_lib target_name lib_basename)
  set(_lib_paths
      "${ROCKCHIP_SDK_SYSROOT}/usr/lib"
      "${ROCKCHIP_SDK_SYSROOT}/lib")

  find_library(_found_lib
    NAMES "${lib_basename}"
    PATHS ${_lib_paths}
    NO_DEFAULT_PATH
    NO_CMAKE_PATH
    NO_CMAKE_ENVIRONMENT_PATH
    NO_SYSTEM_ENVIRONMENT_PATH
    NO_CMAKE_SYSTEM_PATH
    NO_CMAKE_FIND_ROOT_PATH)

  if(NOT _found_lib)
    message(FATAL_ERROR "Missing SDK library '${lib_basename}' under ${ROCKCHIP_SDK_SYSROOT} (searched ${_lib_paths}).")
  endif()

  add_library(${target_name} UNKNOWN IMPORTED)
  set_target_properties(${target_name} PROPERTIES IMPORTED_LOCATION "${_found_lib}")
endfunction()

if(CMAKE_CROSSCOMPILING OR DEFINED ROCKCHIP_SDK_SYSROOT)
  _aimedia_require_sysroot()

  # Imported targets (names align with the project plan)
  # NOTE: Library basenames may vary by SDK; adjust once SDK sysroot is confirmed.
  if(NOT TARGET Rockchip::MPP)
    _aimedia_import_lib(Rockchip::MPP rockchip_mpp)
  endif()

  if(NOT TARGET Rockchip::RGA)
    _aimedia_import_lib(Rockchip::RGA rga)
  endif()

  if(NOT TARGET Rockchip::RKNN)
    _aimedia_import_lib(Rockchip::RKNN rknnrt)
  endif()

  if(NOT TARGET DRM::DRM)
    _aimedia_import_lib(DRM::DRM drm)
  endif()
endif()

