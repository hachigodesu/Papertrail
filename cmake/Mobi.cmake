# Builds a pinned libmobi and defines the target Mobi::Mobi. It reads Kindle's KF8 format, which MuPDF
# can't. MSYS2 and most distros don't package it, so it's built from source everywhere.
#
# Only its library is built, not its command-line tools. It uses its own bundled miniz rather than
# zlib, and its own XML writer rather than libxml2, so it adds nothing else to install. Encryption is
# left out because Papertrail doesn't handle DRM.
include(FetchContent)

FetchContent_Declare(libmobi
    URL https://github.com/bfabiszewski/libmobi/archive/refs/tags/v0.12.tar.gz
    URL_HASH SHA256=78826d161c02ce93ff1cd62838b4d749df754f52185474b82e4093badf4689c1
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    # A folder that isn't there, so that this fetches the source without running its own CMake files.
    SOURCE_SUBDIR papertrail-builds-this-itself)
FetchContent_MakeAvailable(libmobi)

set(mobi_source_dir ${libmobi_SOURCE_DIR}/src)
add_library(papertrail_mobi STATIC
    ${mobi_source_dir}/buffer.c
    ${mobi_source_dir}/compression.c
    ${mobi_source_dir}/debug.c
    ${mobi_source_dir}/index.c
    ${mobi_source_dir}/memory.c
    ${mobi_source_dir}/meta.c
    ${mobi_source_dir}/miniz.c
    ${mobi_source_dir}/opf.c
    ${mobi_source_dir}/parse_rawml.c
    ${mobi_source_dir}/read.c
    ${mobi_source_dir}/structure.c
    ${mobi_source_dir}/util.c
    ${mobi_source_dir}/write.c
    ${mobi_source_dir}/xmlwriter.c)
add_library(Mobi::Mobi ALIAS papertrail_mobi)
target_include_directories(papertrail_mobi SYSTEM PUBLIC ${mobi_source_dir})
target_compile_definitions(papertrail_mobi PRIVATE
    USE_MINIZ
    USE_XMLWRITER
    MOBI_INLINE=inline
    HAVE_STRDUP
    PACKAGE_VERSION="0.12")
set_target_properties(papertrail_mobi PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
# Third-party code: its warnings aren't ours to fix.
target_compile_options(papertrail_mobi PRIVATE -w)
if(NOT WIN32)
    target_link_libraries(papertrail_mobi PRIVATE m)
endif()
