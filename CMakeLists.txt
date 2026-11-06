cmake_minimum_required(VERSION 3.25)

project(Papertrail
    VERSION 0.1.0
    DESCRIPTION "Library manager and reader for academic publications"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets Test)

# Qt 6.12 is the last release that supports Windows 10, so Windows builds must
# stay on it. Linux builds may use newer versions (Arch moves ahead of 6.12).
if(WIN32 AND Qt6_VERSION VERSION_GREATER_EQUAL 6.13)
    message(FATAL_ERROR
        "Found Qt ${Qt6_VERSION}. Windows builds must use Qt 6.12 to keep "
        "Windows 10 support. See docs/PLAN.md.")
endif()

qt_standard_project_setup()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(PAPERTRAIL_BUILD_FORMAT_PROBE "Build the document format probe tool" OFF)

add_subdirectory(src)

if(PAPERTRAIL_BUILD_FORMAT_PROBE)
    add_subdirectory(tools/format_probe)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
