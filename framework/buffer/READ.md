Help me to generate a project Cow, project is compile with cxx/clang/apple-clang, CXX_Flags are -Wall,-Wshallow, -Wunused -O3, if the compile mode is debug, use -o1 instead of -o3, release mode is by default. There is a option to determine lib use jemalloc or tcmalloc. 
libraries are all shared lib, reactor lib depends on poller, backoff lib depends on reactor.
each cmake file should have a solid layout, e.g., has separate section for header files, linked libs, source files list.
for main.cxx underMonitor, It linked lib reactor, in this source files, it wil include like reactor.h file with a relative path, like #include "framework/reactor/reactor.h". The same as other libs. For reactor.h, it will include the poller header file like #include "poller/poller.h"
Cow
  - framework
    - buffer
    - reactor
    - poller
    - network
    - backoff
  - net
    - tcp
    - udp
    - http2
  - resourceMonitor
    - main.cxx
  - examples
    - echo
      - main.cxx