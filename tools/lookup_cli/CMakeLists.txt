qt_add_executable(papertrail-lookup main.cpp)
target_link_libraries(papertrail-lookup PRIVATE papertrail_metadata)
