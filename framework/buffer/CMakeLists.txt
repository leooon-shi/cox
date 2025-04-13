option(USE_JEMALLOC "use jemalloc" ON)
option(USE_TCMALLOC "use tcmalloc" ON)

add_library(buffer STATIC 
            #source files
)

# in source source code
# #ifdef USE_JEMALLOC
#    ...Concrete logic....
# #endif 
#  #if defined(USE_TCMALLOC) && defined(CTEST)
# #endif