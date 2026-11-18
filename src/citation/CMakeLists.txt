add_library(papertrail_citation STATIC
    CitationItem.cpp
    CitationItem.h
    CslEngine.cpp
    CslEngine.h
    CslStyles.cpp
    CslStyles.h
    CslThread.cpp
    CslThread.h
    Exporters.cpp
    Exporters.h
    Formatting.cpp
    Formatting.h
    Importers.cpp
    Importers.h
    Styles.cpp
    Styles.h)

target_include_directories(papertrail_citation PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_citation PUBLIC papertrail_core Qt6::Core PRIVATE QuickJS::QuickJS)

# The CSL styles and locales that come with Papertrail (data/csl/, see SOURCE.md there), and citeproc-js with
# Papertrail's script around it, all built in, so there's nothing to install or find.
set(csl_data ${CMAKE_SOURCE_DIR}/data/csl)
file(GLOB csl_styles CONFIGURE_DEPENDS ${csl_data}/styles/*.csl)
file(GLOB csl_locales CONFIGURE_DEPENDS ${csl_data}/locales/*.xml ${csl_data}/locales/*.json)
qt_add_resources(papertrail_citation "csl_data"
    PREFIX "/csl"
    BASE ${csl_data}
    FILES ${csl_styles} ${csl_locales})
set_source_files_properties(${CITEPROC_SCRIPT} PROPERTIES QT_RESOURCE_ALIAS citeproc.js)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/csl/papertrail.js PROPERTIES QT_RESOURCE_ALIAS papertrail.js)
qt_add_resources(papertrail_citation "csl_scripts"
    PREFIX "/csl"
    FILES ${CITEPROC_SCRIPT} ${CMAKE_CURRENT_SOURCE_DIR}/csl/papertrail.js)
