cmake_minimum_required(VERSION 3.25)

project(Papertrail
    VERSION 1.4.0
    DESCRIPTION "Library manager and reader for academic publications"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# Qt 6.12 everywhere, except where a package's platform doesn't have it yet: the Flatpak builds on Flathub's newest KDE
# runtime, which can lag a version behind. Nothing here needs 6.12 itself.
set(PAPERTRAIL_QT_MINIMUM 6.12 CACHE STRING "The oldest Qt to build with")
find_package(Qt6 ${PAPERTRAIL_QT_MINIMUM} REQUIRED COMPONENTS Core Gui Widgets Network Test)

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

# Where the source is, which the About box links to, as the AGPL asks of anything distributed.
set(PAPERTRAIL_SOURCE_URL "https://github.com/hachigodesu/Papertrail")
add_compile_definitions(PAPERTRAIL_SOURCE_URL="${PAPERTRAIL_SOURCE_URL}")

# Papertrail's licence and a list of what packages bundle, which About can show. Packages add the licence
# texts of the libraries they bundle beside these.
install(FILES LICENSE packaging/THIRD-PARTY.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/papertrail)
# Declared before anything uses it, since a first configure has nothing in its cache yet.
option(PAPERTRAIL_WITH_OCR "Recognise text in scanned pages with Tesseract" ON)
# Reading documents aloud with Qt's text-to-speech, and on Linux the flite voice that comes with Papertrail.
option(PAPERTRAIL_WITH_SPEECH "Read documents aloud with Qt TextToSpeech" ON)
if(PAPERTRAIL_WITH_SPEECH)
    # Here, since every program linking the static papertrail_ui needs to see these targets.
    find_package(Qt6 ${PAPERTRAIL_QT_MINIMUM} REQUIRED COMPONENTS TextToSpeech Multimedia)
endif()
# Kokoro's natural voices for reading aloud, made by sherpa-onnx in the worker, with the voices fetched when asked for.
option(PAPERTRAIL_WITH_NATURAL_VOICE "Read aloud with Kokoro's natural voices, through sherpa-onnx" ON)
if(PAPERTRAIL_WITH_NATURAL_VOICE AND NOT PAPERTRAIL_WITH_SPEECH)
    message(FATAL_ERROR "Natural voices need reading aloud: configure with -DPAPERTRAIL_WITH_NATURAL_VOICE=OFF too")
endif()
# A package to give out, whose self-test fails if anything optional, such as recognising text, isn't in it.
option(PAPERTRAIL_PACKAGE "Build a package, whose self-test requires every optional feature" OFF)

# sherpa-onnx's licence, and those of what's built into it and into ONNX Runtime, which its releases don't include.
if(PAPERTRAIL_WITH_NATURAL_VOICE)
    install(DIRECTORY packaging/licenses/sherpa-onnx DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/papertrail)
endif()

# English, for recognising text, found beside the program by ui::bundledTessdata.
if(PAPERTRAIL_WITH_OCR)
    install(FILES packaging/tessdata/eng.traineddata DESTINATION ${CMAKE_INSTALL_DATADIR}/papertrail/tessdata)
    install(FILES packaging/tessdata/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/papertrail/tessdata)
endif()

# What Linux desktops and software centres need, named by the app id.
if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/io.github.hachigodesu.Papertrail.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/linux/io.github.hachigodesu.Papertrail.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    install(FILES packaging/linux/io.github.hachigodesu.Papertrail.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()

option(PAPERTRAIL_BUILD_FORMAT_PROBE "Build the document format probe tool" OFF)

include(SQLite)
include(Mobi)
include(Keychain)
if(PAPERTRAIL_WITH_NATURAL_VOICE)
    include(SherpaOnnx)
endif()

add_subdirectory(src)
add_subdirectory(tools/gen_library)
add_subdirectory(tools/import_cli)
add_subdirectory(tools/lookup_cli)
add_subdirectory(tools/ui_preview)

if(PAPERTRAIL_BUILD_FORMAT_PROBE)
    add_subdirectory(tools/format_probe)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
