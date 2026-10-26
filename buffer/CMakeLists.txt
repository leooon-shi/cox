set(BUFFER_SOURCES
    buffer.cc
)

set(BUFFER_HEADERS
    bufer.h
)

add_library(cow_buffer STATIC ${BUFFER_SOURCES} ${BUFFER_HEADERS})

target_include_directories(cow_buffer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})