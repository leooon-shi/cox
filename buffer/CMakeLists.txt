set(BUFFER_SOURCES
    buffer.cc
)

set(BUFFER_HEADERS
    buffer.h
)

add_library(cox_buffer STATIC ${BUFFER_SOURCES} ${BUFFER_HEADERS})

if(COX_INSTALL)
    cox_install_module(cox_buffer HEADERS ${BUFFER_HEADERS})
endif()
