set(CONCURRENT_SOURCES
    thread_pool.cc
)
set(CONCURRENT_HEADERS
    thread_pool.h
)

add_library(cow_concurrent STATIC ${CONCURRENT_SOURCES} ${CONCURRENT_HEADERS})

target_link_directories(cow_concurrent PUBLIC pthread)

target_include_directories(cow_concurrent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
