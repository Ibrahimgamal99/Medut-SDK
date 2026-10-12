# medut_merge(OUTPUT <path> INPUTS <lib...> DEPENDS <target...>)
#
# Merges one or more static archives into a single archive.
# Uses platform-appropriate tooling:
#   Apple  → libtool -static     (handles duplicate member names)
#   MSVC   → lib.exe /OUT:
#   GNU/LLVM → ar MRI script     (addlib preserves per-source namespacing)
#
# The resulting archive is placed at OUTPUT.
# A custom target "medut" is added so `cmake --build . --target medut` works.

function(medut_merge)
  cmake_parse_arguments(_M "" "OUTPUT" "INPUTS;DEPENDS" ${ARGN})

  if(NOT _M_OUTPUT)
    message(FATAL_ERROR "medut_merge: OUTPUT is required")
  endif()
  if(NOT _M_INPUTS)
    message(FATAL_ERROR "medut_merge: INPUTS is required")
  endif()

  # -----------------------------------------------------------------------
  # Apple: libtool -static
  # -----------------------------------------------------------------------
  if(APPLE)
    add_custom_command(
      OUTPUT  "${_M_OUTPUT}"
      COMMAND libtool -static -o "${_M_OUTPUT}" ${_M_INPUTS}
      DEPENDS ${_M_DEPENDS} ${_M_INPUTS}
      COMMENT "Merging static archives into ${_M_OUTPUT} (libtool)"
      VERBATIM
    )

  # -----------------------------------------------------------------------
  # MSVC: lib.exe /OUT:
  # -----------------------------------------------------------------------
  elseif(MSVC)
    add_custom_command(
      OUTPUT  "${_M_OUTPUT}"
      COMMAND lib.exe /NOLOGO "/OUT:${_M_OUTPUT}" ${_M_INPUTS}
      DEPENDS ${_M_DEPENDS} ${_M_INPUTS}
      COMMENT "Merging static archives into ${_M_OUTPUT} (lib.exe)"
      VERBATIM
    )

  # -----------------------------------------------------------------------
  # GNU / LLVM ar: MRI script approach
  #   addlib preserves per-archive member namespacing → avoids tls.o collisions
  # -----------------------------------------------------------------------
  else()
    # Prefer llvm-ar from NDK if available (set by toolchain), else system ar
    if(CMAKE_AR)
      set(_AR "${CMAKE_AR}")
    else()
      set(_AR "ar")
    endif()

    if(CMAKE_RANLIB)
      set(_RANLIB "${CMAKE_RANLIB}")
    else()
      set(_RANLIB "ranlib")
    endif()

    # Build MRI script content
    set(_MRI "CREATE ${_M_OUTPUT}\n")
    foreach(_lib ${_M_INPUTS})
      string(APPEND _MRI "ADDLIB ${_lib}\n")
    endforeach()
    string(APPEND _MRI "SAVE\nEND\n")

    set(_MRI_FILE "${CMAKE_BINARY_DIR}/medut_merge.mri")

    # Write MRI script at configure time so the build command can reference it
    file(WRITE "${_MRI_FILE}" "${_MRI}")

    add_custom_command(
      OUTPUT  "${_M_OUTPUT}"
      COMMAND "${_AR}" -M < "${_MRI_FILE}"
      COMMAND "${_RANLIB}" "${_M_OUTPUT}"
      DEPENDS ${_M_DEPENDS} ${_M_INPUTS} "${_MRI_FILE}"
      COMMENT "Merging static archives into ${_M_OUTPUT} (ar MRI)"
      VERBATIM
    )
  endif()

  add_custom_target(medut ALL DEPENDS "${_M_OUTPUT}")
endfunction()
