set(FLOWCONTROL_SOURCES
    token_bucket.cc
    backoff.cc
)

set(FLOWCONTROL_HEADERS
    token_bucket.h
    backoff.h
    concurrency_limiter.h
)

add_library(cox_flowcontrol STATIC ${FLOWCONTROL_SOURCES} ${FLOWCONTROL_HEADERS})

if(COX_INSTALL)
    cox_install_module(cox_flowcontrol HEADERS ${FLOWCONTROL_HEADERS})
endif()
