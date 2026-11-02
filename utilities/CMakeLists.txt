set(SOURCES 
    thread/current_thread.cc
    file/file_opts.cc
)

set(HEADERS 
    thread/current_thread.h
    thread/likely.h
    file/file_opts.h
)

add_library(cox_utilities STATIC ${SOURCES} ${HEADERS})

if(COX_INSTALL)
    cox_install_module(cox_utilities HEADERS ${HEADERS})
endif()
