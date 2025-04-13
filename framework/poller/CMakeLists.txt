file(GLOB_RECURSE POLLER_SRC CONFIGURE_DEPENDS *.cpp *.cxx)
file(GLOB_RECURSE POLLER_HEADERS CONFIGURE_DEPENDS *.h *.hpp)

add_library(poller SHARED ${POLLER_SRC})

target_include_directories(poller PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(poller PUBLIC cxx_std_20)

# Optional: You can add linked libraries here