set(LOGGING_SOURCES
    formatter.cpp
    formatter.h
    json_formatter.cpp
    json_formatter.h
    default_formatter.cpp
    logger.cpp
    logger.h
    timestamp.cpp
    timestamp.h
)

add_library(cow_logging STATIC ${LOGGING_SOURCES})

message(STATUS "Include Path: " ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cow_logging PUBLIC cow_utilities)

target_include_directories(cow_logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})