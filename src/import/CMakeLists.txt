add_library(papertrail_import STATIC
    Extractors.cpp
    Extractors.h
    FullTextIndexer.cpp
    FullTextIndexer.h
    ImportService.cpp
    ImportService.h
    JournalIssues.cpp
    JournalIssues.h
    WorkerClient.cpp
    WorkerClient.h)

target_include_directories(papertrail_import PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_import PUBLIC papertrail_core Qt6::Core)
