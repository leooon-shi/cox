# Add unit test executable
add_executable(event_loop_test event_loop_test.cxx)
target_link_libraries(event_loop_test PRIVATE cow_reactor gtest gtest_main pthread)