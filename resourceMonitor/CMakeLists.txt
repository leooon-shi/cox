add_executable(resourceMonitor main.cxx)

target_include_directories(resourceMonitor PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(resourceMonitor PRIVATE reactor ${ALLOC_LIB})