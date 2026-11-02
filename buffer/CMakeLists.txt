set(BUFFER_SOURCES
    buffer.cc
)

set(BUFFER_HEADERS
    buffer.h
)

add_library(cox_buffer STATIC ${BUFFER_SOURCES} ${BUFFER_HEADERS})

target_include_directories(cox_buffer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
