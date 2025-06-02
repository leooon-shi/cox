# Add unit test executable
set(TESTS_SOURCES
    event_loop_test.cc
)
add_executable(event_loop_test ${TESTS_SOURCES})
target_link_libraries(event_loop_test PRIVATE cow_reactor gtest gtest_main pthread)