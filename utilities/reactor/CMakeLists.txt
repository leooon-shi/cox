set(REACTOR_SOURCES 
    poller/io_poller_factory.cc
    io_channel.cc
    event_loop.cc
)
set(REACTOR_HEADERS 
    poller/io_poller.h
    poller/io_poller_factory.h
    io_channel.h
    event_loop.h
)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Platform: " ${CMAKE_SYSTEM_NAME})
    list(APPEND REACTOR_SOURCES poller/epoll_poller.cc)
    list(APPEND REACTOR_HEADERS poller/epoll_poller.h)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND REACTOR_SOURCES poller/kqueue_poller.cc)
    list(APPEND REACTOR_HEADERS poller/kqueue_poller.h)
    message(STATUS "Platform: " ${CMAKE_SYSTEM_NAME})
else()
    message(FATAL_ERROR "Unsupported plantform: " ${CMAKE_SYSTEM_NAME})
endif()

add_library(cow_reactor STATIC ${REACTOR_SOURCES} ${REACTOR_HEADERS})

target_link_libraries(cow_reactor PUBLIC cow_logging cow_utilities)
message(STATUS "Include Path: " ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(cow_reactor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

