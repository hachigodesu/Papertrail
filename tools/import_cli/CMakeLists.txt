qt_add_executable(papertrail-import main.cpp)
target_link_libraries(papertrail-import PRIVATE papertrail_import)
target_compile_definitions(papertrail-import PRIVATE
    PAPERTRAIL_WORKER_BUILD_PATH="$<TARGET_FILE:papertrail-worker>")
add_dependencies(papertrail-import papertrail-worker)
