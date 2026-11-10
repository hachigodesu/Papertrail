add_library(papertrail_citation STATIC
    CitationItem.cpp
    CitationItem.h
    Exporters.cpp
    Exporters.h
    Formatting.cpp
    Formatting.h
    Importers.cpp
    Importers.h
    Styles.cpp
    Styles.h)

target_include_directories(papertrail_citation PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_citation PUBLIC papertrail_core Qt6::Core)
