# Apply patches to a fetched dependency once.
#
# CPM's PATCHES re-run `patch` whenever ExternalProject re-runs the git update step, which it does on every
# reconfigure. A second application of an already applied patch fails or, with fuzz, applies twice. This script
# records the applied patch set (by content hash) in a stamp file in the source directory and skips it when it matches.
# Run from the source directory as:
#   cmake -DPATCH_FILES="a.patch|b.patch" -P apply_patches_once.cmake
if(NOT DEFINED PATCH_FILES)
    message(FATAL_ERROR "PATCH_FILES is not set")
endif()
find_program(PATCH_EXECUTABLE patch REQUIRED)
string(REPLACE "|" ";" _patches "${PATCH_FILES}")

set(_series "")
foreach(_patch IN LISTS _patches)
    file(SHA256 "${_patch}" _hash)
    string(APPEND _series "${_hash}\n")
endforeach()

set(_stamp ".apollo-patches-applied")
if(EXISTS "${_stamp}")
    file(READ "${_stamp}" _applied)
    if(_applied STREQUAL _series)
        message(STATUS "Patches already applied")
        return()
    endif()
    message(FATAL_ERROR "A different patch set was applied to this source tree. Delete the source and populate "
            "directories under _deps (or the CPM cache entry) so it is fetched again.")
endif()

foreach(_patch IN LISTS _patches)
    execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 --forward --force --no-backup-if-mismatch -i "${_patch}"
            RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Patch does not apply: ${_patch}")
    endif()
endforeach()
file(WRITE "${_stamp}" "${_series}")
