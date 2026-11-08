qt_add_executable(papertrail WIN32 main.cpp SelfTest.cpp SelfTest.h)

if(WIN32)
    # The icon and version details Windows shows.
    set(PAPERTRAIL_ICO "${CMAKE_SOURCE_DIR}/packaging/windows/papertrail.ico")
    configure_file(papertrail.rc.in "${CMAKE_CURRENT_BINARY_DIR}/papertrail.rc" @ONLY)
    target_sources(papertrail PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/papertrail.rc")
endif()

target_link_libraries(papertrail PRIVATE papertrail_ui Qt6::Network)
add_dependencies(papertrail papertrail-worker)
target_compile_definitions(papertrail PRIVATE
    PAPERTRAIL_VERSION="${PROJECT_VERSION}")

# The window's icon, drawn by packaging/make_icons.py.
qt_add_resources(papertrail icons
    PREFIX /icons
    BASE icons
    FILES
        icons/papertrail-16.png
        icons/papertrail-32.png
        icons/papertrail-48.png
        icons/papertrail-64.png
        icons/papertrail-128.png
        icons/papertrail-256.png)

# Small documents for --self-test, which packages run to prove a fresh install can read each kind of file.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/tests/samples/article.pdf PROPERTIES QT_RESOURCE_ALIAS article.pdf)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/tests/samples/book.epub PROPERTIES QT_RESOURCE_ALIAS book.epub)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/tests/samples/with_text.djvu PROPERTIES QT_RESOURCE_ALIAS with_text.djvu)
qt_add_resources(papertrail selftest
    PREFIX /selftest
    FILES
        ${CMAKE_SOURCE_DIR}/tests/samples/article.pdf
        ${CMAKE_SOURCE_DIR}/tests/samples/book.epub
        ${CMAKE_SOURCE_DIR}/tests/samples/with_text.djvu)

install(TARGETS papertrail RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
