add_library(papertrail_ui STATIC
    MainWindow.cpp
    MainWindow.h)

target_include_directories(papertrail_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_ui PUBLIC Qt6::Widgets)
target_compile_definitions(papertrail_ui PRIVATE
    PAPERTRAIL_VERSION="${PROJECT_VERSION}")
