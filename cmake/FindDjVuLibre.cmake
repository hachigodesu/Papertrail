# Finds DjVuLibre and defines the imported target DjVuLibre::DjVuLibre.
find_path(DjVuLibre_INCLUDE_DIR NAMES libdjvu/ddjvuapi.h)
find_library(DjVuLibre_LIBRARY NAMES djvulibre)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DjVuLibre
    REQUIRED_VARS DjVuLibre_LIBRARY DjVuLibre_INCLUDE_DIR)

if(DjVuLibre_FOUND AND NOT TARGET DjVuLibre::DjVuLibre)
    add_library(DjVuLibre::DjVuLibre UNKNOWN IMPORTED)
    set_target_properties(DjVuLibre::DjVuLibre PROPERTIES
        IMPORTED_LOCATION "${DjVuLibre_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DjVuLibre_INCLUDE_DIR}")
endif()
