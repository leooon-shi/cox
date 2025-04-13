file(GLOB_RECURSE BACKOFF_SRC CONFIGURE_DEPENDS *.cpp *.cxx)
file(GLOB_RECURSE BACKOFF_HEADERS CONFIGURE_DEPENDS *.h *.hpp)

add_library(backoff SHARED ${BACKOFF_SRC})

target_include_directories(backoff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(backoff PUBLIC cxx_std_20)

# Dependencies
target_link_libraries(backoff PUBLIC reactor)