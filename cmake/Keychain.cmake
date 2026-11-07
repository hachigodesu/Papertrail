# Builds a pinned QtKeychain and defines the target Papertrail::Keychain. It keeps the Claude API key in the
# operating system's credential store. It's built from source everywhere, like libmobi, because a packaged copy
# would be tied to whichever Qt its distro has, and Windows and Ubuntu's are held at 6.12.
include(FetchContent)

set(BUILD_WITH_QT6 ON CACHE BOOL "" FORCE)
set(BUILD_TEST_APPLICATION OFF CACHE BOOL "" FORCE)
set(BUILD_TRANSLATIONS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# On Linux, this is what reaches GNOME's keyring, and QtKeychain refuses to configure if it's asked for and missing.
# Without it, the key lasts only until the app closes on those desktops, which is worth knowing but not worth failing.
set(papertrail_has_libsecret OFF)
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PAPERTRAIL_LIBSECRET QUIET libsecret-1)
        set(papertrail_has_libsecret ${PAPERTRAIL_LIBSECRET_FOUND})
    endif()
    if(NOT papertrail_has_libsecret)
        message(WARNING "libsecret-1 wasn't found, so the Claude API key can't be kept in GNOME's keyring.")
    endif()
endif()
set(LIBSECRET_SUPPORT ${papertrail_has_libsecret} CACHE BOOL "" FORCE)

FetchContent_Declare(qtkeychain
    URL https://github.com/frankosterfeld/qtkeychain/archive/refs/tags/0.15.0.tar.gz
    URL_HASH SHA256=f4254dc8f0933b06d90672d683eab08ef770acd8336e44dfa030ce041dc2ca22
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/KeychainPatch.cmake
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(qtkeychain)

add_library(papertrail_keychain INTERFACE)
target_link_libraries(papertrail_keychain INTERFACE qt6keychain)
target_include_directories(papertrail_keychain INTERFACE ${qtkeychain_SOURCE_DIR}/qtkeychain ${qtkeychain_BINARY_DIR}/qtkeychain)
add_library(Papertrail::Keychain ALIAS papertrail_keychain)
