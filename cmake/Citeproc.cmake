# Fetches a pinned citeproc-js, which renders CSL styles, and sets CITEPROC_SCRIPT to its CommonJS build. It's built
# into papertrail_citation as a resource, beside Papertrail's own script that drives it (src/citation/csl/).
#
# citeproc-js is offered under the CPAL version 1 or later, or the AGPL version 3 or later. Papertrail uses it under
# the AGPL.
include(FetchContent)

FetchContent_Declare(citeproc
    URL https://registry.npmjs.org/citeproc/-/citeproc-2.4.63.tgz
    URL_HASH SHA256=7f2b7714bfd1f4480aad9c34972eb5d48dc1b85876ba21b5aa9aee74156e2ccf
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR papertrail-builds-this-itself)
FetchContent_MakeAvailable(citeproc)

set(CITEPROC_SCRIPT ${citeproc_SOURCE_DIR}/citeproc_commonjs.js)
set(CITEPROC_LICENSE ${citeproc_SOURCE_DIR}/LICENSE)
if(NOT EXISTS ${CITEPROC_SCRIPT})
    message(FATAL_ERROR "citeproc-js's archive has no citeproc_commonjs.js at ${CITEPROC_SCRIPT}")
endif()

# Built into the program, with the CSL styles and locales, so their licences go with it.
install(FILES ${CITEPROC_LICENSE} DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/papertrail/citeproc-js)
install(FILES ${CMAKE_SOURCE_DIR}/data/csl/SOURCE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/papertrail/csl-styles)
