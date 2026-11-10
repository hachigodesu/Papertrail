find_package(MuPDF REQUIRED)
find_package(DjVuLibre REQUIRED)

add_library(papertrail_engine STATIC
    Backends.h
    DjvuDocument.cpp
    Document.h
    Engine.cpp
    Kf8Converter.cpp
    MuPdfDocument.cpp
    Ocr.cpp
    Ocr.h
    WordLayer.cpp
    WordLayer.h)

target_include_directories(papertrail_engine PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_engine PUBLIC Qt6::Core PRIVATE MuPDF::MuPDF DjVuLibre::DjVuLibre Mobi::Mobi)

# Recognising text in scanned pages. Tesseract and Leptonica are only called from here, on pixels the engine
# renders itself.
if(PAPERTRAIL_WITH_OCR)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TESSERACT REQUIRED IMPORTED_TARGET tesseract lept)
    target_link_libraries(papertrail_engine PRIVATE PkgConfig::TESSERACT)
    target_compile_definitions(papertrail_engine PRIVATE PAPERTRAIL_WITH_OCR)
endif()
