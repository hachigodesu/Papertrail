qt_add_executable(papertrail WIN32 main.cpp)

target_link_libraries(papertrail PRIVATE papertrail_ui)
add_dependencies(papertrail papertrail-worker)
target_compile_definitions(papertrail PRIVATE
    PAPERTRAIL_VERSION="${PROJECT_VERSION}")

install(TARGETS papertrail RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
