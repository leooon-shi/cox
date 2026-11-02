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
    log_macros.h
    timestamp.h
)

add_library(cox_logging STATIC ${LOGGING_SOURCES} ${LOGGING_HEADERS})

message(STATUS "Include Path: " ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cox_logging PUBLIC cox_utilities)

if(COX_INSTALL)
    cox_install_module(cox_logging HEADERS ${LOGGING_HEADERS})
endif()
