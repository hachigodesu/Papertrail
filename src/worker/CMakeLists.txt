qt_add_executable(papertrail-worker main.cpp ReaderHost.cpp ReaderHost.h RecognisedText.cpp RecognisedText.h VoiceHost.cpp
    VoiceHost.h)

target_link_libraries(papertrail-worker PRIVATE papertrail_engine papertrail_import Qt6::Core)
target_compile_definitions(papertrail-worker PRIVATE
    PAPERTRAIL_VERSION="${PROJECT_VERSION}")

# The app looks for its worker beside itself, as it is once installed. This makes that true in a
# build folder too. Windows builds already put every program in one folder.
if(NOT WIN32)
    set_target_properties(papertrail-worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/app")
endif()

install(TARGETS papertrail-worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Natural voices. sherpa-onnx's libraries go beside the worker in a build folder and on Windows, and in a folder of
# Papertrail's own on Linux, which the installed worker looks in first.
if(PAPERTRAIL_WITH_NATURAL_VOICE)
    target_link_libraries(papertrail-worker PRIVATE papertrail_voice)
    target_compile_definitions(papertrail-worker PRIVATE PAPERTRAIL_WITH_NATURAL_VOICE)
    add_custom_command(TARGET papertrail-worker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PAPERTRAIL_SHERPA_ONNX_RUNTIME} $<TARGET_FILE_DIR:papertrail-worker>
        VERBATIM)
    if(WIN32)
        install(FILES ${PAPERTRAIL_SHERPA_ONNX_RUNTIME} DESTINATION ${CMAKE_INSTALL_BINDIR})
    else()
        install(FILES ${PAPERTRAIL_SHERPA_ONNX_RUNTIME} DESTINATION ${CMAKE_INSTALL_LIBDIR}/papertrail)
        set_target_properties(papertrail-worker PROPERTIES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/papertrail")
    endif()
endif()
