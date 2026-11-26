# Apply patches to a fetched dependency once.
#
# CPM's PATCHES re-run `patch` whenever ExternalProject re-runs the git update step, which it does on every
# reconfigure. A second application of an already applied patch fails or, with fuzz, applies twice. This script
# records each applied patch (by content hash) in a stamp file in the source directory and skips the recorded ones.
# Run from the source directory as:
#   cmake -DPATCH_FILES="a.patch|b.patch" -P apply_patches_once.cmake
# (package-lock.cmake also passes PATCH_SET, the patch digests, only so that the CPM source cache key changes with
# the patch contents.)
if(NOT DEFINED PATCH_FILES)
    message(FATAL_ERROR "PATCH_FILES is not set")
endif()
find_program(PATCH_EXECUTABLE patch REQUIRED)
string(REPLACE "|" ";" _patches "${PATCH_FILES}")

set(_stamp ".apollo-patches-applied")
set(_applied "")
if(EXISTS "${_stamp}")
    file(STRINGS "${_stamp}" _applied)

    # A stamp in a tree whose tracked files are unmodified is stale (e.g. the checkout was reset but the untracked
    # stamp stayed): apply the patches again rather than build unpatched sources.
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS ".git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                RESULT_VARIABLE _git_result OUTPUT_VARIABLE _git_changes ERROR_QUIET)
        if(_git_result EQUAL 0 AND _git_changes STREQUAL "")
            message(STATUS "Patch stamp found in an unmodified checkout, applying the patches again")
            set(_applied "")
            file(REMOVE "${_stamp}")
        endif()
    endif()
endif()

# The stamp lists the applied patches in order. It must be a prefix of this patch set: anything else means the tree
# was patched with different patches.
set(_index 0)
list(LENGTH _applied _applied_count)
foreach(_patch IN LISTS _patches)
    file(SHA256 "${_patch}" _hash)
    if(_index LESS _applied_count)
        list(GET _applied ${_index} _recorded)
        if(NOT _recorded STREQUAL _hash)
            message(FATAL_ERROR "A different patch set was applied to this source tree. Delete the source and populate "
                    "directories under _deps (or the CPM cache entry) so it is fetched again.")
        endif()
        math(EXPR _index "${_index} + 1")
        continue()
    endif()

    execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 --forward --force --no-backup-if-mismatch -i "${_patch}"
            RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Patch does not apply: ${_patch}")
    endif()
    # Record each patch as soon as it is in, so an interrupted run resumes after it instead of applying it twice.
    file(APPEND "${_stamp}" "${_hash}\n")
    math(EXPR _index "${_index} + 1")
endforeach()

if(_applied_count GREATER _index)
    message(FATAL_ERROR "A different patch set was applied to this source tree. Delete the source and populate "
            "directories under _deps (or the CPM cache entry) so it is fetched again.")
endif()
message(STATUS "Patches applied")
