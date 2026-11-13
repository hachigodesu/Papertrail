# sherpa-onnx's prebuilt C library, for Kokoro's natural voices, and defines the target SherpaOnnx::CApi and
# PAPERTRAIL_SHERPA_ONNX_RUNTIME, the libraries that go beside the worker and are installed with it.
#
# Pinned at 1.13.8, the last release that falls back on espeak-ng for words its word lists don't have: 2.x leaves them
# out without a word, numbers and names among them. On Windows it's the build with Microsoft's C++ runtime inside it,
# which needs only Windows' own libraries, so MinGW programs can call its C API. Packages that can't download pass the
# unpacked archive as FETCHCONTENT_SOURCE_DIR_SHERPAONNX.
include(FetchContent)

set(PAPERTRAIL_SHERPA_ONNX_VERSION 1.13.8)
set(sherpa_release https://github.com/k2-fsa/sherpa-onnx/releases/download/v${PAPERTRAIL_SHERPA_ONNX_VERSION})
if(WIN32)
    set(sherpa_url ${sherpa_release}/sherpa-onnx-v${PAPERTRAIL_SHERPA_ONNX_VERSION}-win-x64-shared-MT-Release.tar.bz2)
    set(sherpa_hash 6dffdc715a4465b989446a6105265d2cb345e7101591a17d35534b6758f6e8df)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(sherpa_url ${sherpa_release}/sherpa-onnx-v${PAPERTRAIL_SHERPA_ONNX_VERSION}-linux-x64-shared.tar.bz2)
    set(sherpa_hash c0bdb7907d3a74bba1d55d22bf4d9fa75586cf1530614ebe88a27b9118e015c4)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(sherpa_url ${sherpa_release}/sherpa-onnx-v${PAPERTRAIL_SHERPA_ONNX_VERSION}-linux-aarch64-shared-cpu.tar.bz2)
    set(sherpa_hash 4e3734f82bc1379fd91f219f5869c7e9d03b7a4f7561907d8abca4849c51a789)
else()
    message(FATAL_ERROR "There's no prebuilt sherpa-onnx for this system: configure with -DPAPERTRAIL_WITH_NATURAL_VOICE=OFF")
endif()

FetchContent_Declare(sherpaonnx
    URL ${sherpa_url}
    URL_HASH SHA256=${sherpa_hash}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    # A folder that isn't there, so nothing in the archive is run.
    SOURCE_SUBDIR papertrail-uses-it-as-it-is)
FetchContent_MakeAvailable(sherpaonnx)

set(sherpa_lib ${sherpaonnx_SOURCE_DIR}/lib)
if(NOT EXISTS ${sherpaonnx_SOURCE_DIR}/include/sherpa-onnx/c-api/c-api.h)
    message(FATAL_ERROR "sherpa-onnx's C API header isn't in ${sherpaonnx_SOURCE_DIR}")
endif()

add_library(SherpaOnnx::CApi SHARED IMPORTED GLOBAL)
set_target_properties(SherpaOnnx::CApi PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${sherpaonnx_SOURCE_DIR}/include)
if(WIN32)
    set_target_properties(SherpaOnnx::CApi PROPERTIES
        IMPORTED_LOCATION ${sherpa_lib}/sherpa-onnx-c-api.dll
        IMPORTED_IMPLIB ${sherpa_lib}/sherpa-onnx-c-api.lib)
    set(PAPERTRAIL_SHERPA_ONNX_RUNTIME
        ${sherpa_lib}/sherpa-onnx-c-api.dll
        ${sherpa_lib}/onnxruntime.dll
        ${sherpa_lib}/onnxruntime_providers_shared.dll)
else()
    set_target_properties(SherpaOnnx::CApi PROPERTIES
        IMPORTED_LOCATION ${sherpa_lib}/libsherpa-onnx-c-api.so
        IMPORTED_SONAME libsherpa-onnx-c-api.so)
    set(PAPERTRAIL_SHERPA_ONNX_RUNTIME
        ${sherpa_lib}/libsherpa-onnx-c-api.so
        ${sherpa_lib}/libonnxruntime.so)
endif()
