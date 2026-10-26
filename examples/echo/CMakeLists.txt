cmake_minimum_required(VERSION 3.20)

# TCP echo server and client built on the net library.
add_executable(echo_server echo_server.cc)
target_link_libraries(echo_server PRIVATE cow_net)

add_executable(echo_client echo_client.cc)
target_link_libraries(echo_client PRIVATE cow_net)

# Legacy task-scheduling demo (not an echo server; kept for reference).
add_executable(echo_scheduling_demo echo_main.cc)
target_link_libraries(echo_scheduling_demo PRIVATE cow_reactor cow_logging cow_utilities)
