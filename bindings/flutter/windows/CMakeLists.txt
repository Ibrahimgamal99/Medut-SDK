# Windows: the Dart side drives the C library over dart:ffi
# (lib/src/ffi_backend.dart), so there is nothing to compile here. The app's
# runner installs the prebuilt core next to the .exe, where
# DynamicLibrary.open('medut.dll') finds it.
#
# medut.dll is committed by .github/workflows/windows-dll.yml (commit=true);
# scripts/build-windows.ps1 refreshes this copy on a local build.
cmake_minimum_required(VERSION 3.14)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/medut.dll")
  message(FATAL_ERROR "medut: ${CMAKE_CURRENT_SOURCE_DIR}/medut.dll is missing; "
    "run the windows-dll.yml workflow with commit=true, or scripts/build-windows.ps1")
endif()

set(medut_bundled_libraries
  "${CMAKE_CURRENT_SOURCE_DIR}/medut.dll"
  PARENT_SCOPE
)
