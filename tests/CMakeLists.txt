function(papertrail_core_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE papertrail_core Qt6::Test ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

qt_add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE papertrail_ui Qt6::Test)
add_test(NAME smoke COMMAND test_smoke)
set_tests_properties(smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME worker_starts COMMAND papertrail-worker --version)

qt_add_executable(test_library_ui test_library_ui.cpp)
target_link_libraries(test_library_ui PRIVATE papertrail_ui Qt6::Test)
target_compile_definitions(test_library_ui PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_library_ui papertrail-worker)
add_test(NAME test_library_ui COMMAND test_library_ui)
set_tests_properties(test_library_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)

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

qt_add_executable(test_reader test_reader.cpp)
target_link_libraries(test_reader PRIVATE papertrail_ui Qt6::Test)
target_compile_definitions(test_reader PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_reader papertrail-worker)
add_test(NAME test_reader COMMAND test_reader)
set_tests_properties(test_reader PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)

# Times real queries on a 100,000-publication library. Takes minutes rather than seconds, so it's
# labelled: `ctest -LE perf` skips it, and `ctest -L perf` runs only this.
papertrail_core_test(test_query_perf papertrail_synthetic)
set_tests_properties(test_query_perf PROPERTIES LABELS perf TIMEOUT 1800)

# Times the library window on a 100,000-publication library. Also labelled, for the same reason.
qt_add_executable(test_ui_perf test_ui_perf.cpp)
target_link_libraries(test_ui_perf PRIVATE papertrail_ui papertrail_synthetic Qt6::Test)
add_test(NAME test_ui_perf COMMAND test_ui_perf)
set_tests_properties(test_ui_perf PROPERTIES LABELS perf TIMEOUT 1800 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Times the reader on a 3,000-page book. Also labelled.
qt_add_executable(test_reader_perf test_reader_perf.cpp)
target_link_libraries(test_reader_perf PRIVATE papertrail_ui Qt6::Test)
target_compile_definitions(test_reader_perf PRIVATE PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>")
add_dependencies(test_reader_perf papertrail-worker)
add_test(NAME test_reader_perf COMMAND test_reader_perf)
set_tests_properties(test_reader_perf PROPERTIES LABELS perf TIMEOUT 900 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
