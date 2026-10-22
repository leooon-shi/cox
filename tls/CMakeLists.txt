find_package(OpenSSL REQUIRED)

set(TLS_SOURCES
    tls_context.cc
    tls_engine.cc
    tls_connection.cc
    cert_reloader.cc
    ocsp_stapler.cc
)
set(TLS_HEADERS
    tls_context.h
    tls_engine.h
    tls_connection.h
    cert_reloader.h
    ocsp_stapler.h
)

add_library(cow_tls STATIC ${TLS_SOURCES} ${TLS_HEADERS})

target_link_libraries(cow_tls PUBLIC OpenSSL::SSL OpenSSL::Crypto cow_net cow_fswatch)

target_include_directories(cow_tls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})