find_package(MuPDF REQUIRED)
find_package(DjVuLibre REQUIRED)

qt_add_executable(format-probe main.cpp)
target_link_libraries(format-probe PRIVATE
    Qt6::Core MuPDF::MuPDF DjVuLibre::DjVuLibre)
