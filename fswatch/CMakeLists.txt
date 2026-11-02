set(FSWATCH_SOURCES
    file_watcher.cc
)
set(FSWATCH_HEADERS
    file_watcher.h
)

add_library(cox_fswatch STATIC ${FSWATCH_SOURCES} ${FSWATCH_HEADERS})

target_link_libraries(cox_fswatch PUBLIC cox_reactor cox_logging)

if(COX_INSTALL)
    cox_install_module(cox_fswatch HEADERS ${FSWATCH_HEADERS})
endif()
