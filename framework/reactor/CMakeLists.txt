file(GLOB_RECURSE REACTOR_SRC CONFIGURE_DEPENDS *.cpp *.cxx)
file(GLOB_RECURSE REACTOR_HEADERS CONFIGURE_DEPENDS *.h *.hpp)

add_library(reactor SHARED ${REACTOR_SRC})

target_include_directories(reactor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(reactor PUBLIC cxx_std_20)

# Dependencies
target_link_libraries(reactor PUBLIC poller)