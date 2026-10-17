add_executable(timer_handler main.cc)

target_include_directories(timer_handler PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(timer_handler PRIVATE reactor ${ALLOC_LIB})