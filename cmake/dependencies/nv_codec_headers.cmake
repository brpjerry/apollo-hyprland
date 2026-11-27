set(SUNSHINE_PREPARED_NV_CODEC_HEADERS "${FFMPEG_PREPARED_BINARIES}/include/ffnvcodec/nvEncodeAPI.h")

# The native Linux NVENC encoder is built against SDK 13.1 (NVENC_SDK_VERSION=1301), so a prepared FFmpeg bundle's
# headers are only used when they are 13.1; otherwise the pinned 13.1 headers are fetched.
set(SUNSHINE_PREPARED_NV_CODEC_HEADERS_13_1 OFF)
if(EXISTS "${SUNSHINE_PREPARED_NV_CODEC_HEADERS}")
    file(STRINGS "${SUNSHINE_PREPARED_NV_CODEC_HEADERS}" _nvenc_major REGEX "^#define[ \t]+NVENCAPI_MAJOR_VERSION[ \t]+13([ \t]|$)")
    file(STRINGS "${SUNSHINE_PREPARED_NV_CODEC_HEADERS}" _nvenc_minor REGEX "^#define[ \t]+NVENCAPI_MINOR_VERSION[ \t]+1([ \t]|$)")
    if(_nvenc_major AND _nvenc_minor)
        set(SUNSHINE_PREPARED_NV_CODEC_HEADERS_13_1 ON)
    else()
        message(STATUS "The prepared FFmpeg binaries' NV codec headers are not SDK 13.1; fetching 13.1")
    endif()
    unset(_nvenc_major)
    unset(_nvenc_minor)
endif()

if(WIN32 OR (NOT APPLE AND NOT SUNSHINE_PREPARED_NV_CODEC_HEADERS_13_1))
    include("${CMAKE_MODULE_PATH}/dependencies/build_deps.cmake")
endif()

# Keep build-deps-owned declarations out of Sunshine's generated package lock.
set(SUNSHINE_CPM_PACKAGE_LOCK_ENABLED "${CPM_PACKAGE_LOCK_ENABLED}")
set(CPM_PACKAGE_LOCK_ENABLED OFF)

if(NOT APPLE)
    if(SUNSHINE_PREPARED_NV_CODEC_HEADERS_13_1)
        # Prepared FFmpeg bundles include the NV codec headers used to build them.
        set(NV_CODEC_HEADERS_13_1_INCLUDE_DIR "${FFMPEG_INCLUDE_DIRS}")
        message(STATUS "Using NV codec headers from prepared FFmpeg binaries")
    else()
        CPMGetPackage(nv_codec_headers_13_1)
        set(NV_CODEC_HEADERS_13_1_INCLUDE_DIR "${nv_codec_headers_13_1_SOURCE_DIR}/include")
    endif()
endif()
unset(SUNSHINE_PREPARED_NV_CODEC_HEADERS)
unset(SUNSHINE_PREPARED_NV_CODEC_HEADERS_13_1)

if(WIN32)
    CPMGetPackage(nv_codec_headers_13)
    CPMGetPackage(nv_codec_headers_11)
    CPMGetPackage(nv_codec_headers_12)

    set(NV_CODEC_HEADERS_11_INCLUDE_DIR "${nv_codec_headers_11_SOURCE_DIR}/include")
    set(NV_CODEC_HEADERS_12_INCLUDE_DIR "${nv_codec_headers_12_SOURCE_DIR}/include")
    set(NV_CODEC_HEADERS_13_INCLUDE_DIR "${nv_codec_headers_13_SOURCE_DIR}/include")
endif()

set(CPM_PACKAGE_LOCK_ENABLED "${SUNSHINE_CPM_PACKAGE_LOCK_ENABLED}")
unset(SUNSHINE_CPM_PACKAGE_LOCK_ENABLED)
