set(SOURCES 
    thread/current_thread.cc
    file/file_opts.cc
)
set(HEADERS 
    thread/current_thread.h
    thread/likely.h
)

add_library(cow_utilities STATIC ${SOURCES} ${HEADERS})

target_include_directories(cow_utilities PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

