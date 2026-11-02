set(FLOWCONTROL_SOURCES
    token_bucket.cc
    backoff.cc
)

set(FLOWCONTROL_HEADERS
    token_bucket.h
    backoff.h
    concurrency_limiter.h
)

add_library(cox_flowcontrol STATIC ${FLOWCONTROL_SOURCES} ${FLOWCONTROL_HEADERS})

target_include_directories(cox_flowcontrol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
