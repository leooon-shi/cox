set(HTTP_SOURCES
    url.cc
    http_request.cc
    http_response.cc
    http_context.cc
    http_server.cc
    router.cc
    response_parser.cc
    http_client.cc
    connection_pool.cc
    multi_origin_pool.cc
)
set(HTTP_HEADERS
    url.h
    http_request.h
    http_response.h
    http_context.h
    http_server.h
    router.h
    response_parser.h
    http_client.h
    connection_pool.h
    multi_origin_pool.h
)

add_library(cox_http STATIC ${HTTP_SOURCES} ${HTTP_HEADERS})

target_link_libraries(cox_http PUBLIC cox_net cox_buffer cox_logging cox_tls)

target_include_directories(cox_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
