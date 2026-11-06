# Builds a pinned SQLite from its amalgamation and defines the target SQLite::SQLite.
#
# Using one pinned build means every platform gets the same version and FTS5 setup,
# instead of whatever each distro ships.
include(FetchContent)

FetchContent_Declare(sqlite
    URL https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(sqlite)

add_library(papertrail_sqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
add_library(SQLite::SQLite ALIAS papertrail_sqlite)
target_include_directories(papertrail_sqlite SYSTEM PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(papertrail_sqlite PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_OMIT_DEPRECATED)
# Third-party code: its warnings aren't ours to fix.
target_compile_options(papertrail_sqlite PRIVATE -w)

find_package(Threads REQUIRED)
target_link_libraries(papertrail_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(NOT WIN32)
    target_link_libraries(papertrail_sqlite PUBLIC m)
endif()
