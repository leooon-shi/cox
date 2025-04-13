# buffer/CMakeLists.txt (similar for network)
file(GLOB_RECURSE BUFFER_SRC CONFIGURE_DEPENDS *.cpp *.cxx)
file(GLOB_RECURSE BUFFER_HEADERS CONFIGURE_DEPENDS *.h *.hpp)

add_library(network SHARED ${BUFFER_SRC})
target_include_directories(network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(network PUBLIC cxx_std_20)