set(FSWATCH_SOURCES
    file_watcher.cc
)
set(FSWATCH_HEADERS
    file_watcher.h
)

add_library(cox_fswatch STATIC ${FSWATCH_SOURCES} ${FSWATCH_HEADERS})

target_link_libraries(cox_fswatch PUBLIC cox_reactor cox_logging)

target_include_directories(cox_fswatch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
