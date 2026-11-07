find_package(ZLIB REQUIRED)

add_library(papertrail_transfer STATIC
    LibraryTransfer.cpp
    LibraryTransfer.h
    Zip.cpp
    Zip.h)

target_include_directories(papertrail_transfer PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_transfer PUBLIC papertrail_core Qt6::Core PRIVATE ZLIB::ZLIB)
