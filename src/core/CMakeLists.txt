add_library(papertrail_core STATIC
    db/Database.cpp
    db/Database.h
    db/Schema.cpp
    db/Schema.h
    model/Identifiers.cpp
    model/Identifiers.h
    model/Publication.cpp
    model/PublicationFields.cpp
    model/PublicationFields.h
    model/Publication.h
    query/LibraryQuery.cpp
    query/LibraryQuery.h
    query/PublicationFilter.h
    query/SearchParser.cpp
    query/SearchParser.h
    repo/FileRepository.cpp
    repo/FileRepository.h
    repo/LookupRepository.cpp
    repo/LookupRepository.h
    repo/PublicationMerge.cpp
    repo/PublicationMerge.h
    repo/PublicationRepository.cpp
    repo/PublicationRepository.h
    repo/ReadingStateRepository.cpp
    repo/ReadingStateRepository.h
    repo/TagRepository.cpp
    repo/TagRepository.h
    repo/CollectionRepository.cpp
    repo/CollectionRepository.h
    settings/SettingsStore.cpp
    settings/SettingsStore.h)

target_include_directories(papertrail_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_core PUBLIC Qt6::Core PRIVATE SQLite::SQLite)
