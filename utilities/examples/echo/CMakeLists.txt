cmake_minimum_required(VERSION 3.20)

# Executables
add_executable(echo_main echo_main.cc)
target_link_libraries(echo_main PRIVATE cow_reactor cow_logging cow_utilities)
# target_compile_options(reactor_benchmark PRIVATE -pthread)
