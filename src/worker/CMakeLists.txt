qt_add_executable(papertrail-worker main.cpp)

target_link_libraries(papertrail-worker PRIVATE papertrail_engine papertrail_import Qt6::Core)
target_compile_definitions(papertrail-worker PRIVATE
    PAPERTRAIL_VERSION="${PROJECT_VERSION}")

# The app looks for its worker beside itself, as it is once installed. This makes that true in a
# build folder too. Windows builds already put every program in one folder.
if(NOT WIN32)
    set_target_properties(papertrail-worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/app")
endif()

install(TARGETS papertrail-worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
