qt_add_executable(papertrail-worker main.cpp)

target_link_libraries(papertrail-worker PRIVATE papertrail_engine papertrail_import Qt6::Core)
target_compile_definitions(papertrail-worker PRIVATE
    PAPERTRAIL_VERSION="${PROJECT_VERSION}")

install(TARGETS papertrail-worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
