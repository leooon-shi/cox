set(HTTP_SOURCES
    url.cc
    http_request.cc
    http_response.cc
    http_context.cc
    http_server.cc
    server_stream.cc
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
    server_stream.h
    router.h
    response_parser.h
    http_client.h
    connection_pool.h
    multi_origin_pool.h
)

add_library(cox_http STATIC ${HTTP_SOURCES} ${HTTP_HEADERS})

target_link_libraries(cox_http PUBLIC cox_net cox_buffer cox_logging cox_tls)

# HTTP/2 (opt-in). nghttp2 comes from a vendored submodule when one is checked
# out at third_party/nghttp2 (third_party/CMakeLists.txt defines the target),
# otherwise from the system package via pkg-config. Either way the rest of the
# tree only sees the nghttp2::nghttp2 target.
if (COX_WITH_HTTP2)
    if(NOT TARGET nghttp2::nghttp2)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2>=1.43)
        add_library(nghttp2::nghttp2 ALIAS PkgConfig::NGHTTP2)
    endif()
    target_sources(cox_http PRIVATE http2_session.cc http2_session.h
                   http2_client_session.cc http2_client_session.h
                   http2_client.cc http2_client.h)
    target_link_libraries(cox_http PUBLIC nghttp2::nghttp2)
    target_compile_definitions(cox_http PUBLIC COX_WITH_HTTP2)
    list(APPEND HTTP_HEADERS http2_session.h http2_client_session.h
        http2_client.h)
endif()

if(COX_INSTALL)
    cox_install_module(cox_http HEADERS ${HTTP_HEADERS})
endif()
