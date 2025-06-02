cmake_minimum_required(VERSION 3.20)

# Executables
add_executable(reactor_benchmark reactor_benchmark.cc)
target_link_libraries(reactor_benchmark PRIVATE cow_reactor cow_logging)
# target_compile_options(reactor_benchmark PRIVATE -pthread)

