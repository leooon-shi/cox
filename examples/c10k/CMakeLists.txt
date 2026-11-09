cmake_minimum_required(VERSION 3.20)

# C10K experiments: a tunable server, an epoll load generator, and a
# backpressure demo (proxy + deliberately slow sink).
#
# c10k_server links cox::metrics (which transitively brings cox::http + cox::net)
# so it can expose a Prometheus /metrics admin endpoint for load validation.
add_executable(c10k_server c10k_server.cc)
target_link_libraries(c10k_server PRIVATE cox::metrics)

# HTTP target for scenario S4 (body-limit vs big-body): HttpServer with
# --max-body + /metrics. Links. cox::metrics (-> cox::http -> cox::net).
add_executable(http_target http_target.cc)
target_link_libraries(http_target PRIVATE cox::metrics)

# Standalone on purpose: only libc/POSIX, no Cox code in the load generator.
add_executable(c10k_client c10k_client.cc)

# The liveness probe: one request/second against the data port, standalone too.
add_executable(probe_client probe_client.cc)

add_executable(backpressure_proxy backpressure_proxy.cc)
target_link_libraries(backpressure_proxy PRIVATE cox::net)

add_executable(slow_sink slow_sink.cc)
target_link_libraries(slow_sink PRIVATE cox::net)
