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
papertrail_core_test(test_references)
papertrail_core_test(test_library_query)

papertrail_core_test(test_extractors papertrail_import)
papertrail_core_test(test_import papertrail_import)
papertrail_core_test(test_citations papertrail_citation)
target_compile_definitions(test_citations PRIVATE PAPERTRAIL_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
papertrail_core_test(test_transfer papertrail_transfer papertrail_import)
target_compile_definitions(test_transfer PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples"
    PAPERTRAIL_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_dependencies(test_transfer papertrail-worker)
target_compile_definitions(test_import PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_import papertrail-worker)

# Every sample, damaged in many ways, through the import's worker and the reader's process.
qt_add_executable(test_damaged_files test_damaged_files.cpp)
target_link_libraries(test_damaged_files PRIVATE papertrail_ui Qt6::Test)
target_compile_definitions(test_damaged_files PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_damaged_files papertrail-worker)
add_test(NAME test_damaged_files COMMAND test_damaged_files)
set_tests_properties(test_damaged_files PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 1200)

qt_add_executable(test_reader test_reader.cpp)
target_link_libraries(test_reader PRIVATE papertrail_ui Qt6::Test)
target_compile_definitions(test_reader PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_reader papertrail-worker)
add_test(NAME test_reader COMMAND test_reader)
set_tests_properties(test_reader PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)

qt_add_executable(test_summary test_summary.cpp)
target_link_libraries(test_summary PRIVATE papertrail_ui Qt6::Test Qt6::Network)
target_compile_definitions(test_summary PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_summary papertrail-worker)
add_test(NAME test_summary COMMAND test_summary)
set_tests_properties(test_summary PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)

qt_add_executable(test_annotations test_annotations.cpp)
target_link_libraries(test_annotations PRIVATE papertrail_ui Qt6::Test)
target_compile_definitions(test_annotations PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_annotations papertrail-worker)
add_test(NAME test_annotations COMMAND test_annotations)
set_tests_properties(test_annotations PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)

qt_add_executable(test_fulltext test_fulltext.cpp)
target_link_libraries(test_fulltext PRIVATE papertrail_ui Qt6::Test)
target_compile_definitions(test_fulltext PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_fulltext papertrail-worker)
add_test(NAME test_fulltext COMMAND test_fulltext)
set_tests_properties(test_fulltext PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)

qt_add_executable(test_volumes test_volumes.cpp)
target_link_libraries(test_volumes PRIVATE papertrail_ui Qt6::Test)
target_compile_definitions(test_volumes PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_dependencies(test_volumes papertrail-worker)
add_test(NAME test_volumes COMMAND test_volumes)
set_tests_properties(test_volumes PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)

qt_add_executable(test_metadata test_metadata.cpp)
target_link_libraries(test_metadata PRIVATE papertrail_ui Qt6::Test Qt6::Network)
target_compile_definitions(test_metadata PRIVATE
    PAPERTRAIL_WORKER_PATH="$<TARGET_FILE:papertrail-worker>"
    PAPERTRAIL_TEST_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples"
    PAPERTRAIL_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_dependencies(test_metadata papertrail-worker)
add_test(NAME test_metadata COMMAND test_metadata)
set_tests_properties(test_metadata PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)

# Times real queries on a 100,000-publication library. Takes minutes rather than seconds, so it's
# labelled: `ctest -LE perf` skips it, and `ctest -L perf` runs only this.
papertrail_core_test(test_query_perf papertrail_synthetic)
set_tests_properties(test_query_perf PROPERTIES LABELS perf TIMEOUT 1800)

# Times exporting and importing a 100,000-publication library.
papertrail_core_test(test_transfer_perf papertrail_synthetic papertrail_transfer)
set_tests_properties(test_transfer_perf PROPERTIES LABELS perf TIMEOUT 1800)

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

# On Windows, QtTest prints nothing unless it's told to, which leaves CI logs with no way of saying
# what failed.
get_property(all_tests DIRECTORY PROPERTY TESTS)
foreach(one_test IN LISTS all_tests)
    set_property(TEST ${one_test} APPEND PROPERTY ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
endforeach()
