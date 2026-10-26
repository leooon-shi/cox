# Unit / integration tests.
#
# Every test is a GoogleTest binary registered with CTest, so the whole suite
# runs via a single `ctest` invocation (see docs/user-guide.md and the CI
# workflow) instead of ad-hoc loops over build/bin/*_test.
#
# Two registration modes:
#   cox_add_test()           -- gtest_discover_tests: registers each TEST() case
#                               individually, so CTest can schedule, filter and
#                               time out at case granularity
#   cox_add_test(... OPAQUE) -- one CTest entry for the whole binary. Used where
#                               build-time discovery is undesirable: tests that
#                               shell out to external tools (openssl) or bind
#                               fixed ports, which we do not want executed
#                               during the build.
include(GoogleTest)

# Timeouts: these are safety nets against a hung test (several tests bind real
# sockets or spawn helper processes), not performance targets. Sanitizer builds
# are substantially slower -- instrumented TLS tests in particular -- so the
# limits are raised when one is enabled rather than leaving CI to report
# spurious timeouts.
if (COX_SANITIZE STREQUAL "OFF")
    set(COX_TEST_TIMEOUT 120)
    set(COX_TEST_TIMEOUT_SLOW 300)
else()
    set(COX_TEST_TIMEOUT 300)
    set(COX_TEST_TIMEOUT_SLOW 600)
endif()

# cox_add_test(<name> LIBS <libs...> [OPAQUE] [SLOW])

function(cox_add_test name)
    cmake_parse_arguments(ARG "OPAQUE;SLOW" "" "LIBS" ${ARGN})

    add_executable(${name} ${name}.cc)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} gtest_main gmock pthread)

    if (ARG_SLOW)
        set(timeout ${COX_TEST_TIMEOUT_SLOW})
    else()
        set(timeout ${COX_TEST_TIMEOUT})
    endif()

    if (ARG_OPAQUE)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT ${timeout})
    else()
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT ${timeout}
            DISCOVERY_TIMEOUT 30
        )
    endif()
endfunction()

# ---- reactor core ----
cox_add_test(event_loop_test          LIBS cow_reactor)
cox_add_test(timer_test               LIBS cow_reactor)
cox_add_test(event_loop_thread_test   LIBS cow_reactor)

# ---- buffer / logging ----
cox_add_test(buffer_test              LIBS cow_buffer)
cox_add_test(logger_test              LIBS cow_logging SLOW)     # async sink timing

# ---- concurrency ----
cox_add_test(thread_pool_test         LIBS cow_concurrent)
cox_add_test(thread_pool_integration_test  LIBS cow_net cow_concurrent)

# ---- flow / overload control ----
cox_add_test(token_bucket_test        LIBS cow_flowcontrol)
cox_add_test(backoff_test             LIBS cow_flowcontrol)
cox_add_test(concurrency_limiter_test LIBS cow_flowcontrol)
cox_add_test(overload_control_test    LIBS cow_flowcontrol)

# ---- net / TCP ----
cox_add_test(inet_address_test        LIBS cow_net)
cox_add_test(socket_test              LIBS cow_net)
cox_add_test(acceptor_test            LIBS cow_net)
cox_add_test(tcp_client_test          LIBS cow_net)
cox_add_test(tcp_server_test          LIBS cow_net SLOW)
cox_add_test(unix_socket_test         LIBS cow_net)
cox_add_test(graceful_shutdown_test   LIBS cow_net)
cox_add_test(hardening_test           LIBS cow_net SLOW)
cox_add_test(resolver_test            LIBS cow_net cow_concurrent)

# ---- TLS ----
# OPAQUE: these invoke the openssl CLI to build certs/PKI and (for ocsp_test)
# start a local responder, so they must not run at build/discovery time.
cox_add_test(tls_engine_test          LIBS cow_tls OPAQUE)
cox_add_test(tls_integration_test     LIBS cow_tls cow_net OPAQUE)
cox_add_test(cert_reloader_test       LIBS cow_tls OPAQUE)
cox_add_test(sni_test                 LIBS cow_tls OPAQUE SLOW)
cox_add_test(ocsp_test                LIBS cow_tls OPAQUE SLOW)
cox_add_test(file_watcher_test        LIBS cow_fswatch)

# ---- HTTP ----
cox_add_test(http_parser_test         LIBS cow_http)
cox_add_test(http_server_test         LIBS cow_http)
cox_add_test(router_test              LIBS cow_http)
cox_add_test(url_test                 LIBS cow_http)
cox_add_test(http_client_test         LIBS cow_http)
cox_add_test(connection_pool_test     LIBS cow_http SLOW)
cox_add_test(dns_client_test          LIBS cow_http cow_concurrent)
# OPAQUE: generate certs via the openssl CLI
cox_add_test(tls_pool_test            LIBS cow_http OPAQUE SLOW)
cox_add_test(https_test               LIBS cow_http OPAQUE)

# Guards the API snippets used in docs/ and insights/features/ (also shells out
# to openssl for TLS/OCSP examples).
cox_add_test(doc_snippets_test        LIBS cow_http cow_concurrent cow_logging cow_tls OPAQUE)


