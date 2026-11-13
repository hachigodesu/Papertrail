# Kokoro's natural voices through sherpa-onnx. Only the worker links this.
add_library(papertrail_voice STATIC
    Voice.cpp
    Voice.h)

target_include_directories(papertrail_voice PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_voice PUBLIC Qt6::Core PRIVATE SherpaOnnx::CApi)
