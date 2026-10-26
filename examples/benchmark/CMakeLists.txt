cmake_minimum_required(VERSION 3.20)

# Reactor task-scheduling micro-benchmark (existing).
add_executable(reactor_benchmark reactor_benchmark.cc)
target_link_libraries(reactor_benchmark PRIVATE cow_reactor cow_logging)

# TCP echo throughput & latency benchmark (QPS, MB/s, p50/p90/p99).
add_executable(echo_throughput_bench echo_throughput_bench.cc)
target_link_libraries(echo_throughput_bench PRIVATE cow_net)

# Buffer micro-benchmark.
add_executable(buffer_bench buffer_bench.cc)
target_link_libraries(buffer_bench PRIVATE cow_buffer)

# ThreadPool task-throughput micro-benchmark (sweeps take-batch size).
add_executable(thread_pool_bench thread_pool_bench.cc)
target_link_libraries(thread_pool_bench PRIVATE cow_concurrent)
