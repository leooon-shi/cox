set(CONCURRENT_SOURCES
    thread_pool.cc
)
set(CONCURRENT_HEADERS
    thread_pool.h
)

add_library(cox_concurrent STATIC ${CONCURRENT_SOURCES} ${CONCURRENT_HEADERS})

target_link_libraries(cox_concurrent PUBLIC pthread)

if(COX_INSTALL)
    cox_install_module(cox_concurrent HEADERS ${CONCURRENT_HEADERS})
endif()
