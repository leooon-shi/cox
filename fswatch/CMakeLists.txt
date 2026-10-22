set(FSWATCH_SOURCES
    file_watcher.cc
)
set(FSWATCH_HEADERS
    file_watcher.h
)

add_library(cow_fswatch STATIC ${FSWATCH_SOURCES} ${FSWATCH_HEADERS})

target_link_libraries(cow_fswatch PUBLIC cow_reactor cow_logging)

target_include_directories(cow_fswatch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})