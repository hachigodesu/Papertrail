# Builds a pinned QuickJS-ng and defines the target QuickJS::QuickJS, the JavaScript engine that runs citeproc-js
# for CSL styles.
#
# Only the engine is built, not quickjs-libc, so a script has no way to reach files, processes or the network: all
# it gets is what Papertrail hands it. It's always built optimised, even in a debug build, since citeproc-js is
# slow enough already.
include(FetchContent)

FetchContent_Declare(quickjs
    URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.17.0.tar.gz
    URL_HASH SHA256=559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    # A folder that isn't there, so that this fetches the source without running its own CMake files.
    SOURCE_SUBDIR papertrail-builds-this-itself)
FetchContent_MakeAvailable(quickjs)

add_library(papertrail_quickjs STATIC
    ${quickjs_SOURCE_DIR}/dtoa.c
    ${quickjs_SOURCE_DIR}/libregexp.c
    ${quickjs_SOURCE_DIR}/libunicode.c
    ${quickjs_SOURCE_DIR}/quickjs.c)
add_library(QuickJS::QuickJS ALIAS papertrail_quickjs)
target_include_directories(papertrail_quickjs SYSTEM PUBLIC ${quickjs_SOURCE_DIR})
target_compile_definitions(papertrail_quickjs PRIVATE _GNU_SOURCE $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601>)
set_target_properties(papertrail_quickjs PROPERTIES C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
# Third-party code: its warnings aren't ours to fix. -O2 comes after a debug build's flags, so it wins.
target_compile_options(papertrail_quickjs PRIVATE -w -O2)
find_package(Threads REQUIRED)
target_link_libraries(papertrail_quickjs PUBLIC Threads::Threads)
if(NOT WIN32)
    target_link_libraries(papertrail_quickjs PUBLIC m ${CMAKE_DL_LIBS})
endif()

# It's built into the program, so its licence goes with it.
install(FILES ${quickjs_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/papertrail/quickjs)
