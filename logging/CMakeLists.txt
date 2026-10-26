set(LOGGING_SOURCES
    formatter.cc
    json_formatter.cc
    default_formatter.cc
    sink.cc
    async_sink.cc
    logger.cc
    timestamp.cc
)

set(LOGGING_HEADERS
    formatter.h
    json_formatter.h
    default_formatter.h
    log_record.h
    sink.h
    async_sink.h
    logger.h
    timestamp.h
)

add_library(cow_logging STATIC ${LOGGING_SOURCES} ${LOGGING_HEADERS})

message(STATUS "Include Path: " ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cow_logging PUBLIC cow_utilities)

target_include_directories(cow_logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})