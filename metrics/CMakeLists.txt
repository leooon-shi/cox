# metrics module: Counter/Gauge/histogram + Registry + exposition.
#
# Core (metrics.cc, registry.cc) has NO Cox dependencies. The optional
# server_metrics.cc wires a TcpServer to standard metrics via its public hooks,
# so it depends on cox_net -- the dependency points metrics -> net, never the
# reverse, keeping the net library metrics-free.
set(METRICS_SOURCES
    metrics.cc
    registry.cc
    server_metrics.cc
)
set(METRICS_HEADERS
    metrics.h
    registry.h
    server_metrics.h
)

add_library(cox_metrics STATIC ${METRICS_SOURCES} ${METRICS_HEADERS})
target_link_libraries(cox_metrics PUBLIC cox_net cox_http)

if(COX_INSTALL)
    cox_install_module(cox_metrics HEADERS ${METRICS_HEADERS})
endif()