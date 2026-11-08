add_library(papertrail_summary STATIC
    SummaryBatches.cpp
    SummaryBatches.h
    SummaryEngine.cpp
    SummaryEngine.h
    SummaryRequest.cpp
    SummaryRequest.h
    SummarySpending.cpp
    SummarySpending.h)

target_include_directories(papertrail_summary PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(papertrail_summary PUBLIC papertrail_core papertrail_metadata Qt6::Core Qt6::Network)
set_target_properties(papertrail_summary PROPERTIES AUTOMOC ON)
