add_library(papertrail_metadata STATIC
    LookupEngine.cpp
    LookupEngine.h
    Matching.cpp
    Matching.h
    NoticeChecker.cpp
    NoticeChecker.h
    Providers.cpp
    Providers.h
    Transport.cpp
    Transport.h)

target_include_directories(papertrail_metadata PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_metadata PUBLIC papertrail_core papertrail_import Qt6::Core Qt6::Network)
