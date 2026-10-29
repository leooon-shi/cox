cmake_minimum_required(VERSION 3.20)

# C10K experiments: a tunable server, an epoll load generator, and a
# backpressure demo (proxy + deliberately slow sink).
add_executable(c10k_server c10k_server.cc)
target_link_libraries(c10k_server PRIVATE cow_net)

# Standalone on purpose: only libc/POSIX, no Cox code in the load generator.
add_executable(c10k_client c10k_client.cc)

add_executable(backpressure_proxy backpressure_proxy.cc)
target_link_libraries(backpressure_proxy PRIVATE cow_net)


add_executable(slow_sink slow_sink.cc)
target_link_libraries(slow_sink PRIVATE cow_net)