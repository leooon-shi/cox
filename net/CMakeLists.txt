# net module.
set(NET_SOURCES
    inet_address.cc
    socket.cc
    acceptor.cc
    tcp_connection.cc
    tcp_server.cc
    net_runtime.cc
    connector.cc
    tcp_client.cc
    resolver.cc
)

set(NET_HEADERS
    inet_address.h
    socket.h
    acceptor.h
    tcp_connection.h
    tcp_server.h
    net_runtime.h
    connector.h
    tcp_client.h
    resolver.h
)

# Optional async DNS resolver via c-ares.
if (COX_HAVE_CARES)
    list(APPEND NET_SOURCES cares_resolver.cc)
    list(APPEND NET_HEADERS cares_resolver.h)
endif()

add_library(cox_net STATIC ${NET_SOURCES} ${NET_HEADERS})

target_link_libraries(cox_net PUBLIC cox_reactor cox_buffer cox_logging
                       cox_utilities cox_flowcontrol cox_concurrent)

if (COX_HAVE_CARES)
    target_compile_definitions(cox_net PUBLIC COX_HAVE_CARES)
    target_include_directories(cox_net PUBLIC $<BUILD_INTERFACE:${CARES_INCLUDE_DIR}>)
    target_link_libraries(cox_net PUBLIC ${CARES_LIB})
endif()

if(COX_INSTALL)
    cox_install_module(cox_net HEADERS ${NET_HEADERS})
endif()
