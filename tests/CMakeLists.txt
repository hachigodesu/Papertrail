qt_add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE papertrail_ui Qt6::Test)

add_test(NAME smoke COMMAND test_smoke)
set_tests_properties(smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME worker_starts COMMAND papertrail-worker)
