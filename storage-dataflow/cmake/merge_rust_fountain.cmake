# W6.8: fold the Rust LT staticlib into the normal sdf_fountain archive.
# This preserves the installed -lsdf_fountain/pkg-config/CFFI surface instead
# of leaking a second Rust archive and its link ordering into every consumer.
foreach(required SDF_C_ARCHIVE SDF_RUST_ARCHIVE SDF_COMBINED_ARCHIVE SDF_AR SDF_RANLIB)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "merge_rust_fountain: missing ${required}")
    endif()
endforeach()

get_filename_component(_work "${SDF_COMBINED_ARCHIVE}" DIRECTORY)
file(MAKE_DIRECTORY "${_work}")
file(REMOVE "${SDF_COMBINED_ARCHIVE}")
set(_mri "${SDF_COMBINED_ARCHIVE}.mri")
file(WRITE "${_mri}"
    "CREATE ${SDF_COMBINED_ARCHIVE}\n"
    "ADDLIB ${SDF_C_ARCHIVE}\n"
    "ADDLIB ${SDF_RUST_ARCHIVE}\n"
    "SAVE\nEND\n")
execute_process(
    COMMAND "${SDF_AR}" -M
    INPUT_FILE "${_mri}"
    RESULT_VARIABLE _ar_rc
    ERROR_VARIABLE _ar_err)
if(NOT _ar_rc EQUAL 0)
    message(FATAL_ERROR "merge_rust_fountain: ar -M failed: ${_ar_err}")
endif()
execute_process(
    COMMAND "${SDF_RANLIB}" "${SDF_COMBINED_ARCHIVE}"
    RESULT_VARIABLE _ranlib_rc
    ERROR_VARIABLE _ranlib_err)
if(NOT _ranlib_rc EQUAL 0)
    message(FATAL_ERROR "merge_rust_fountain: ranlib failed: ${_ranlib_err}")
endif()

# Replace only after the complete archive and index exist. The CMake target is
# rebuilt from its C objects before this step, so repeated merges cannot accrete
# duplicate Rust members.
file(RENAME "${SDF_COMBINED_ARCHIVE}" "${SDF_C_ARCHIVE}")
file(REMOVE "${_mri}")
