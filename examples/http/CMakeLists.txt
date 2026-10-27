cmake_minimum_required(VERSION 3.20)

# REST server example (HttpServer + Router).
add_executable(rest_server reset_server.cc)
target_link_libraries(rest_server PRIVATE cow_http)
