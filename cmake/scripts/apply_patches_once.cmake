# Apply patches to a fetched dependency once.
#
# CPM's PATCHES re-run `patch` whenever ExternalProject re-runs the git update step, which it does on every
# reconfigure. A second application of an already applied patch fails or, with fuzz, applies twice. This script looks
# at the tree instead of trusting a record of earlier runs. It finds the largest k for which the tree is "the first k
# patches applied" (reversing patches k..1 succeeds on a scratch copy of the patched files), then applies the rest.
# A partly applied patch or a different patch set matches no k and fails to apply, which stops the configure; an
# interrupted run, a restored checkout or a leftover stamp can't certify the wrong sources.
# Run from the source directory as:
#   cmake -DPATCH_FILES="a.patch|b.patch" -P apply_patches_once.cmake
# (package-lock.cmake also passes PATCH_SET, the patch digests, only so that the CPM source cache key changes with
# the patch contents.)
if(NOT DEFINED PATCH_FILES)
    message(FATAL_ERROR "PATCH_FILES is not set")
endif()
find_program(PATCH_EXECUTABLE patch REQUIRED)
string(REPLACE "|" ";" _patches "${PATCH_FILES}")
list(LENGTH _patches _count)
set(_refetch "Delete the source and populate directories under _deps (or the CPM cache entry) so it is fetched again.")

# Files the patches touch (their "+++ b/<path>" lines).
set(_files "")
foreach(_patch IN LISTS _patches)
    file(STRINGS "${_patch}" _targets REGEX "^\\+\\+\\+ b/")
    foreach(_target IN LISTS _targets)
        string(REGEX REPLACE "^\\+\\+\\+ b/([^\t]*).*$" "\\1" _target "${_target}")
        list(APPEND _files "${_target}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES _files)

string(RANDOM LENGTH 12 _suffix)
set(_scratch "${CMAKE_CURRENT_BINARY_DIR}/.apollo-patch-check-${_suffix}")

# Whether reversing patches k..1, in that order, succeeds on a copy of the touched files.
function(first_patches_applied k result)
    file(REMOVE_RECURSE "${_scratch}")
    file(MAKE_DIRECTORY "${_scratch}")
    foreach(_file IN LISTS _files)
        if(EXISTS "${_file}")
            get_filename_component(_dir "${_file}" DIRECTORY)
            file(COPY "${_file}" DESTINATION "${_scratch}/${_dir}")
        endif()
    endforeach()
    set(_ok TRUE)
    set(_index ${k})
    while(_ok AND _index GREATER 0)
        math(EXPR _list_index "${_index} - 1")
        list(GET _patches ${_list_index} _patch)
        execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 -R --forward --no-backup-if-mismatch --silent -i "${_patch}"
                WORKING_DIRECTORY "${_scratch}" RESULT_VARIABLE _result OUTPUT_QUIET ERROR_QUIET)
        if(NOT _result EQUAL 0)
            set(_ok FALSE)
        endif()
        math(EXPR _index "${_index} - 1")
    endwhile()
    file(REMOVE_RECURSE "${_scratch}")
    set(${result} ${_ok} PARENT_SCOPE)
endfunction()

set(_applied 0)
set(_k ${_count})
while(_k GREATER 0)
    first_patches_applied(${_k} _ok)
    if(_ok)
        set(_applied ${_k})
        break()
    endif()
    math(EXPR _k "${_k} - 1")
endwhile()

set(_index ${_applied})
while(_index LESS _count)
    list(GET _patches ${_index} _patch)
    # --forward makes patch refuse, rather than reverse-apply, a patch that looks applied already.
    execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 --forward --dry-run --silent -i "${_patch}"
            RESULT_VARIABLE _result OUTPUT_QUIET ERROR_QUIET)
    if(_result EQUAL 0)
        execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 --forward --no-backup-if-mismatch --silent -i "${_patch}"
                RESULT_VARIABLE _result)
    endif()
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${_patch} is neither applied nor applicable: the source tree is partly patched or was "
                "patched differently. ${_refetch}")
    endif()
    math(EXPR _index "${_index} + 1")
endwhile()

# Earlier versions of this script kept a stamp file; the tree itself is the record now.
file(REMOVE ".apollo-patches-applied")
math(EXPR _new "${_count} - ${_applied}")
if(_new EQUAL 0)
    message(STATUS "Patches already applied")
else()
    message(STATUS "Applied ${_new} patch(es)")
endif()
