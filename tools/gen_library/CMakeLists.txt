# Builds realistic fake libraries for performance tests.
add_library(papertrail_synthetic STATIC SyntheticLibrary.cpp SyntheticLibrary.h)
target_include_directories(papertrail_synthetic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(papertrail_synthetic PUBLIC papertrail_core)

qt_add_executable(gen-library main.cpp)
target_link_libraries(gen-library PRIVATE papertrail_synthetic)
