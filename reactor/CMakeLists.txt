set(REACTOR_SOURCES 
    poller/io_poller.h
    poller/io_poller_factory.h
    poller/io_poller_factory.cxx
    io_channel.h
    io_channel.cxx
    event_loop.h
    event_loop.cxx
)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Platform: " ${CMAKE_SYSTEM_NAME})
    list(APPEND REACTOR_SOURCES poller/epoll_poller.h poller/epoll_poller.cxx)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND REACTOR_SOURCES poller/kqueue_poller.h poller/kqueue_poller.cxx)
    message(STATUS "Platform: " ${CMAKE_SYSTEM_NAME})
else()
    message(FATAL_ERROR "Unsupported plantform: " ${CMAKE_SYSTEM_NAME})
endif()

add_library(cow_reactor STATIC ${REACTOR_SOURCES})

target_link_libraries(cow_reactor PUBLIC cow_logging cow_utilities)
message(STATUS "Include Path: " ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(cow_reactor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

