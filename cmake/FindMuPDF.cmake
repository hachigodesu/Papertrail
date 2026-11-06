# Finds MuPDF and defines the imported target MuPDF::MuPDF.
find_path(MuPDF_INCLUDE_DIR NAMES mupdf/fitz.h)
find_library(MuPDF_LIBRARY NAMES mupdf)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MuPDF
    REQUIRED_VARS MuPDF_LIBRARY MuPDF_INCLUDE_DIR)

if(MuPDF_FOUND AND NOT TARGET MuPDF::MuPDF)
    add_library(MuPDF::MuPDF UNKNOWN IMPORTED)
    set_target_properties(MuPDF::MuPDF PROPERTIES
        IMPORTED_LOCATION "${MuPDF_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MuPDF_INCLUDE_DIR}")
endif()
