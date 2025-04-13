add_executable(echo_server main.cxx)

target_include_directories(echo_server PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(echo_server PRIVATE reactor ${ALLOC_LIB})