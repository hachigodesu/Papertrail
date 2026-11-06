find_package(MuPDF REQUIRED)
find_package(DjVuLibre REQUIRED)

add_library(papertrail_engine STATIC
    Backends.h
    DjvuDocument.cpp
    Document.h
    Engine.cpp
    MuPdfDocument.cpp)

target_include_directories(papertrail_engine PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_engine PUBLIC Qt6::Core PRIVATE MuPDF::MuPDF DjVuLibre::DjVuLibre)
