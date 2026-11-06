function(papertrail_core_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE papertrail_core Qt6::Test ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

qt_add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE papertrail_ui Qt6::Test)
add_test(NAME smoke COMMAND test_smoke)
set_tests_properties(smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME worker_starts COMMAND papertrail-worker)

papertrail_core_test(test_database)
papertrail_core_test(test_repositories)
papertrail_core_test(test_settings)
papertrail_core_test(test_search_parser)
papertrail_core_test(test_library_query)

papertrail_core_test(test_extractors papertrail_import)
papertrail_core_test(test_import papertrail_import)
target_compile_definitions(test_import PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_import papertrail-worker)

# Times real queries on a 100,000-publication library. Takes minutes rather than seconds, so it's
# labelled: `ctest -LE perf` skips it, and `ctest -L perf` runs only this.
papertrail_core_test(test_query_perf papertrail_synthetic)
set_tests_properties(test_query_perf PROPERTIES LABELS perf TIMEOUT 1800)
