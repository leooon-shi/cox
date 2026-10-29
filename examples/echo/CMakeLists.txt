cmake_minimum_required(VERSION 3.20)

# TCP echo server and client built on the net library.
add_executable(echo_server echo_server.cc)
target_link_libraries(echo_server PRIVATE cow_net)

add_executable(echo_client echo_client.cc)
target_link_libraries(echo_client PRIVATE cow_net)

# runInLoop(): marshal work from other threads onto the loop thread.
add_executable(cross_thread_tasks cross_thread_tasks.cc)
target_link_libraries(cross_thread_tasks PRIVATE cow_reactor cow_logging cow_utilities)
