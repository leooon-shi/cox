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

add_library(cow_net STATIC ${NET_SOURCES} ${NET_HEADERS})

target_link_libraries(cow_net PUBLIC cow_reactor cow_buffer cow_logging
                      cow_utilities cow_flowcontrol cow_concurrent)

if (COX_HAVE_CARES)
    target_compile_definitions(cow_net PUBLIC COX_HAVE_CARES)
    target_include_directories(cow_net PUBLIC ${CARES_INCLUDE_DIR})
    target_link_libraries(cow_net PUBLIC ${CARES_LIB})
endif()

target_include_directories(cow_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
