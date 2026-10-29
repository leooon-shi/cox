# Cox examples

Build them with the `examples` preset (or `./build.sh --build-examples`):

```bash
cmake --preset examples && cmake --build --preset examples --parallel
# binaries land in build/examples/bin/
```

| Example | What it shows |
|---|---|
| `echo/echo_server`, `echo/echo_client` | Minimal TCP (and Unix-domain) echo; connection cap, idle timeout, graceful drain on SIGINT |
| `echo/cross_thread_tasks` | `runInLoop()`: other threads post work, every task runs on the loop thread |
| `http/rest_server` | `HttpServer` + `Router`: path params, query string, form body |
| `benchmark/*` | Micro-benchmarks: reactor task scheduling, echo QPS/latency, thread-pool batching, buffer |
| **`c10k/c10k_server`** | Tunable echo server for C10K experiments: io threads vs `SO_REUSEPORT` listeners, caps, accept rate limit, live stats |
| **`c10k/c10k_client`** | Standalone epoll load generator: N connections, paced connects, closed-loop requests, multiple source IPs, latency percentiles |
| **`c10k/backpressure_proxy`** + **`c10k/slow_sink`** | End-to-end backpressure: high-water mark → `stopRead()` → TCP window closes → fast sender throttled |

## C10K walkthrough

Raise the file-descriptor limit in **both** shells first - each connection is one fd on each side:

```bash
ulimit -n 200000          # needs a hard limit >= this (ulimit -Hn)
```

### 1. Hold 20k idle connections on one thread

```bash
./c10k_server --port 9100
./c10k_client --port 9100 --conns 20000 --msg 0 --duration 10
```
The server's stats line shows `live=20000`. One epoll loop holds them all; with thread-per-connection that would be 20k threads (8 MiB stack reservation each by default).

Measured on the study host (8 vCPU VM, kernel 6.8, Release build):
`20000 connected, 0 failed`, server `live=20000`, ~0 CPU while idle.

### 2. Why did connecting 20k take 11 s? - client-side TIME_WAIT / ephemeral ports

Run test 1 twice in a row. The second run connects at ~1.8k conn/s with a p99 connect latency near 900 ms, while a 10k run takes 0.35 s. Nothing is wrong on the server (`ListenOverflows=0`, `softnet_stat` drops 0). The previous run's sockets sit in **TIME_WAIT on the client** (`ss -tan state time-wait | wc -l` → ~40k), and the kernel has to search the 28,232-port ephemeral range (`ip_local_port_range 32768 60999`) for a free 4-tuple to 127.0.0.1:9100.

Fix it the way production clients do - more 4-tuples:
```bash
./c10k_client --port 9100 --conns 20000 --msg 0 --duration 0 \
    --src-ips 127.0.0.2,127.0.0.3,127.0.0.4,127.0.0.5
```
Measured: **20000 connected in 0.79 s (25k conn/s), p99 21 ms** - same server, same TIME_WAIT backlog. (`IP_BIND_ADDRESS_NO_PORT` lets the kernel pick ports per 4-tuple rather than per local IP.) See `docs/interview/tcp_connection_lifecycle_scaling.md` for the theory.

### 3. One loop vs io threads vs SO_REUSEPORT listeners
f
```bash
./c10k_server --port 9100 --quiet                 # 1 loop does accept + I/O
./c10k_server --port 9100 --quiet --threads 4     # 1 acceptor loop + 4 io loops (round-robin)
./c10k_server --port 9100 --quiet --reuseport 4   # 4 listeners; kernel hashes connections
./c10k_client --port 9100 --conns 10000 --msg 64 --duration 5
```
Measured (10k connections, 64 B echo, client and server on the same 8-vCPU VM, so the client competes for CPU):

| Server mode | req/s | p50 | p99 |
|---|---|---|---|
| 1 loop | 66k | 131 ms | 249 ms |
| 4 io threads | 74k | 132 ms | 155 ms |
| 4 SO_REUSEPORT listeners | 82k | 118 ms | 143 ms |

The single-threaded client is the bottleneck here (latency ≈ 10k connections ÷ client throughput - Little's law), which is itself the lesson: **check the load generator first**. Spreading the server mainly cuts tail latency. For server-bound numbers run the client on another host or several client processes, and pin them (`taskset`).

### 4. Connection cap, accept rate limit, fd exhaustion

```bash
./c10k_server --port 9100 --max-conns 5000      # extra connections: accepted and closed at once
./c10k_server --port 9100 --accept-qps 2000     # token bucket on NEW connections
(ulimit -n 1024; ./c10k_server --port 9100)     # hit EMFILE on purpose
```
With the default 1024 fd limit the server logs `Acceptor::handleRead out of fds` and keeps serving existing connections: it closes a reserved spare fd, accepts and immediately closes the pending connection, and reopens the spare, so the listen socket doesn't spin on a permanently readable event.

### 5. Backpressure

```bash
./slow_sink --port 9300 --kbps 1024                       # 1 MiB/s reader
./backpressure_proxy --listen 9200 --upstream 9300        # backpressure on (1 MiB high-water mark)
head -c 2G /dev/zero | nc -q1 127.0.0.1 9200              # fast sender
```
Then repeat with `--no-backpressure`. Measured over 6 s:

| | Sender managed to push | Proxy RSS |
|---|---|---|
| Backpressure on | 21 MiB | **5 MiB** |
| Backpressure off | 2,102 MiB | **1.9 GiB** (and growing) |

The sink received 5.9 MiB in both cases. Without backpressure the proxy reads everything the fast side sends and queues it in user space until it is killed by the OOM killer; with it, `stopRead()` leaves data in the kernel receive buffer, TCP advertises a zero window, and the sender blocks.

## Performance-tuning practice list

Exercises in increasing depth. Each names the question, the commands to drive it, what to observe, and the doc that explains the result (`docs/performance/...` and `docs/interview/...` in the study repo).

| # | Exercise | Drive it with | Observe | Expected lesson | Reference |
|---|---|---|---|---|---|
| 1 | Idle connection capacity | `c10k_client --msg 0 --conns 50000` | `free -m`, `proc/<pid>/status` VmRSS, `ss -s`, `slabtop` (`TCP`, `sock_inode_cache`) | Memory per idle connection (kernel socket + Cox buffers); fds and memory, not CPU, bound C10K | `performance/network/C10K_C10M_problem.md`, `buffer.md` |
| 2 | Ephemeral port / TIME_WAIT exhaustion | Repeat ex. 1 twice; then `--src-ips` | `ss -tan state time-wait \| wc -l`, connect p99 | Client 4-tuples run out first; fixes: keep-alive, more source IPs, `tcp_tw_reuse` | `interview/tcp_connection_lifecycle_scaling.md` |
| 3 | Accept-queue overflow | `c10k_server` with a deliberately slow accept loop (add a `usleep` in `setConnectionCallback`) + burst `c10k_client`; vary `net.core.somaxconn` | `ss -lnt` (Recv-Q vs Send-Q), `nstat -az TcpExtListenOverflows TcpExtListenDrops` | Full accept queue silently drops the final ACK → 1 s/3 s connect stalls | `performance/network/syn_attack.md`, `performance/network/practice_1.md` |
| 4 | SYN flood vs SYN cookies | `hping3 -S --flood -p 9100 127.0.0.1` against `c10k_server` (own machine only) | `nstat TcpExtSyncookiesSent`, `netstat -s \| grep -i listen`, `ss -n state syn-recv` | `tcp_syncookies`, `tcp_max_syn_backlog`, `tcp_synack_retries` trade-offs | `performance/network/syn_attack.md`, `ddos_attack.md` |
| 5 | Threading model | Ex. 3 table: `--threads` vs `--reuseport`, client on another host | `mpstat -P ALL 1`, `pidstat -t 1`, `perf top` | One loop saturates one core; SO_REUSEPORT removes the shared accept path | `performance/network/network_io_model.md`, `interview/epoll_edge_triggered.md` |
| 6 | Small-packet syscall cost | `echo_throughput_bench 100 5 16` vs `... 16384`; `c10k_client --inflight 8` | `perf stat -e syscalls:sys_enter_write,context-switches`, `strace -c -f -p` | Per-message syscall + wakeup overhead dominates small messages; pipelining/batching wins | `interview/zero_copy_small_packets.md`, `interview/io_uring_schedulers_allocators.md` |
| 7 | Nagle x delayed ACK | `c10k_server` with and without `--nodelay`; `c10k_client --conns 10 --msg 200 --split` | Request p99 | Measured: server without `TCP_NODELAY` → **p99 41 ms, 8k req/s**; with `--nodelay` → **p99 0.24 ms, 66k req/s**. The split request makes the echo leave in two segments; the server holds the second (Nagle) until the client's delayed ACK (~40 ms) | `performance/network/network_delay.md` |
| 8 | Backpressure and memory | Ex. 5 | Proxy RSS, `ss -tmi` (`Recv-Q`, `rcv_space`, `skmem`) | Bounded buffers + `stopRead()` push the problem back into TCP | `interview/tcp_connection_lifecycle_scaling.md` §5 |
| 9 | Socket buffer sizing | `backpressure_proxy` + `tc qdisc add dev lo root netem delay 50ms` | `ss -ti` (`cwnd`, `rtt`, `snd_wnd`), throughput with `net.ipv4.tcp_rmem/wmem` changes | Bandwidth-delay product drives buffer needs; autotuning limits | `performance/network/buffer.md`, `network_configuration.md` |
| 10 | Packet loss and retransmits | `tc qdisc add dev lo root netem loss 1%` + `c10k_client --msg 512` | `nstat TcpRetransSegs`, `ss -ti retrans`, p99 vs p50 | Tail latency explodes before averages move; RTO and fast retransmit | `performance/network/network_delay.md`, `packets_tools.md` |
| 11 | Where packets are dropped | Overload `c10k_server` (ex. 5 single loop at max rate) | `/proc/net/softnet_stat`, `ip -s link`, `nstat`, `perf trace -e skb:kfree_skb`, `drapwatch` | Map each drop counter to its layer | `performance/network/linux_network_stack.md`, `diagnostic_tools.md` |
| 12 | Connection tracking limits | `c10k_client --conns 50000` through a host with an iptables/nftables NAT or `conntrack` rule | `conntrack -C`, `sysctl net.netfilter.nf_conntrack_max`, `dmesg \| grep conntrack` | "table full, dropping packet" - conntrack is a hidden connection limit | `performance/network/NAT.md` |
| 13 | CPU profiling a hot server | `c10k_server --threads 4` at full load | `perf record -F 99 -g -p <pid>`, flame graph; `perf stat` IPC | Find whether time goes to syscalls, memcpy (`Buffer`), logging, or locks | `Interview/performance_profiling_methodology.md`, `performance/cpu.md` |
| 14 | Lock contention in the thread pool | `thread_pool_bench 8 300000 4` (keep tasks ≤1M under perf; the 10M default x 8 batch sizes produces GBs of trace data and looks hung) | `sudo perf lock record -o lock.data -- ./thread_pool_bench 8 300000 4` then `sudo perf lock contention -i lock.data -E 8`; `sudo perf trace -s -e futex -- ...`; `sudo perf record -e sched:sched_switch -g` + `perf report --no-children` for who blocks where. (`perf lock contention -b` needs perf built with BPF skeletons - Ubuntu's isn't.) | Measured: ~9k futex calls per worker; 78% of blocking is `__lll_lock_wait` from `ThreadPool::takeBatch` (34%) and `ThreadPool::run` (26%) - the queue mutex, on both the consumer and producer side | `interview/std_mutex_real_cost.md`, `interview/multithreading_hazards.md` |
| 15 | Allocator effect | `LD_PRELOAD=libjemalloc.so.2` or `libtcmalloc.so` on `echo_throughput_bench` / `c10k_server` | QPS, RSS, `perf top` (`malloc`/`free` share) | Per-message `std::string` allocations; allocator choice and buffer reuse | `interview/tcmalloc-rseq-vs-jemalloc-tls.md` |
| 16 | NUMA / IRQ affinity (multi-socket or real NIC) | `c10k_server` pinned with `taskset`/`numactl`; client on another host | `/proc/interrupts`, `numastat -p`, `perf c2c` | Keep NIC IRQs, loop threads and memory on one node | `interview/numa_hugepages_memory_locallity.md` |
| 17 | Sanitizer regression run | `cmake --preset tsan` / `asan`, then ex. 3 and ex. 5 | TSan/ASan reports | Concurrency changes must stay race-free under load | `docs/interview/sanitizers_hands_on.md` |

Ground rules for every exercise: change one variable at a time, record the environment (kernel, CPU, build type, client location), run each case several times, compare percentiles rather than means, and remember the client can be the bottleneck - as exerciese 3 above shows.
