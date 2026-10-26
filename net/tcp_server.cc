#include "tcp_server.h"

#include "acceptor.h"
#include "event_loop.h"
#include "event_loop_thread_pool.h"
#include "logger.h"
#include "socket.h"

#include <string>
#include <utility>
#include <vector>

namespace net {
namespace {
logging::Logger& log() {
    static logging::Logger logger;
    return logger;
}
}  // namespace

TcpServer::TcpServer(reactor::EventLoop* loop, const InetAddress& listen_addr,
                     std::string name, Option option)
    : loop_(loop),
      ip_port_(listen_addr.toIpPort()),
      name_(std::move(name)),
      acceptor_(std::make_unique<Acceptor>(loop, listen_addr,
                                           option == Option::kReusePort)),
      thread_pool_(
          std::make_shared<reactor::EventLoopThreadPool>(loop, name_)) {
    acceptor_->setNewConnectionCallback(
        [this](int sockfd, const InetAddress& peer) {
            newConnection(sockfd, peer);
        });
}

TcpServer::~TcpServer() {
    loop_->assertInLoopThread();
    log().info("TcpServer::dtor [" + name_ + "]");
    for (auto& [name, conn] : connections_) {
        TcpConnectionPtr c(conn);
        conn.reset();
        c->getLoop()->runInLoop([c]() { c->connectDestroyed(); });
    }
}

void TcpServer::setThreadNum(int num_threads) {
    thread_pool_->setThreadNum(num_threads);
}

void TcpServer::setAcceptRateLimit(double qps, double burst) {
    if (qps > 0) {
        accept_limiter_ =
            std::make_unique<flowcontrol::TokenBucket>(qps, burst);
    } else {
        accept_limiter_.reset();
    }
}

void TcpServer::start() {
    if (started_.exchange(true)) {
        return;  // already started.
    }
    thread_pool_->start(thread_init_callback_);
    loop_->runInLoop([this]() { acceptor_->listen(); });
}

void TcpServer::newConnection(int sockfd, const InetAddress& peer) {
    loop_->assertInLoopThread();

    // During graceful drain, refuse new connections outright.
    if (draining_) {
        sockets::close(sockfd);
        return;
    }

    // Accept-rate limit: shed early when the new-connection rate exceeds the
    // configured budget (absorbs accept storms without unbounded work).
    if (accept_limiter_ && !accept_limiter_->tryAcquire()) {
        log().warning("TcpServer::newConnection [" + name_ +
                      "] accept rate limit exceeded; shedding " +
                      peer.toIpPort());
        sockets::close(sockfd);
        if (overload_callback_) {
            overload_callback_(peer);
        }
        return;
    }

    // Enforce the connection cap: reject beyond the limit by closing the newly
    // accepted fd immediately. This keeps the accept queue moving (no wedge)
    // while bounding resource use.
    if (max_connections_ > 0 && connections_.size() >= max_connections_) {
        log().warning("TcpServer::newConnection [" + name_ +
                      "] connection cap " + std::to_string(max_connections_) +
                      " reached; rejecting " + peer.toIpPort());
        sockets::close(sockfd);
        if (overload_callback_) {
            overload_callback_(peer);
        }
        return;
    }
    std::string conn_name =
        name_ + "-" + ip_port_ + "#" + std::to_string(next_conn_id_++);
    log().debug("TcpServer::newConnection [" + name_ + "] new connection " +
                conn_name + " from " + peer.toIpPort());
    // Assign this connection to an io loop (round-robin). In single-threaded
    // mode this is the base loop.
    reactor::EventLoop* io_loop = thread_pool_->getNextLoop();

    InetAddress local = sockets::getLocalAddr(sockfd);
    auto conn = std::make_shared<TcpConnection>(io_loop, conn_name, sockfd,
                                                local, peer);
    connections_[conn_name] = conn;  // map is owned by the base loop
    conn->setConnectionCallback(connection_callback_);
    conn->setMessageCallback(message_callback_);
    conn->setWriteCompleteCallback(write_complete_callback_);
    conn->setCloseCallback(
        [this](const TcpConnectionPtr& c) { removeConnection(c); });
    if (idle_seconds_ > 0) {
        conn->setIdleTimeout(idle_seconds_);
    }
    if (write_seconds_ > 0) {
        conn->setWriteTimeout(write_seconds_);
    }
    if (max_input_buffer_size_ > 0) {
        conn->setMaxInputBufferSize(max_input_buffer_size_);
    }
    if (max_age_seconds_ > 0) {
        conn->setMaxConnectionAge(max_age_seconds_);
    }

    // Establish the connection on its own io loop thread.
    io_loop->runInLoop([conn]() { conn->connectEstablished(); });
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    // May be invoked from the connection's loop; hop to the server loop to
    // mutate the connection map safely.
    loop_->runInLoop([this, conn]() { removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    log().debug("TcpServer::removeConnectionInLoop [" + name_ + "] " +
                conn->name());

    connections_.erase(conn->name());
    conn->getLoop()->runInLoop([conn]() { conn->connectDestroyed(); });

    // If we are draining and this was the last connection, drain is complete.
    if (draining_) {
        maybeFinishDrain();
    }
}

void TcpServer::shutdown(uint32_t drain_deadline_seconds,
                         DrainCompleteCallback on_complete) {
    // Marshal onto the base loop; all shutdown state lives there.
    loop_->runInLoop(
        [this, drain_deadline_seconds, cb = std::move(on_complete)]() mutable {
            if (draining_.exchange(true)) {
                return;  // already draining
            }
            drain_complete_callback_ = std::move(cb);
            shutdownInLoop(drain_deadline_seconds);
        });
}

void TcpServer::shutdownInLoop(uint32_t drain_deadline_seconds) {
    loop_->assertInLoopThread();
    log().info("TcpServer::shutdown [" + name_ + "] draining " +
               std::to_string(connections_.size()) +
               " connection(s), deadline " +
               std::to_string(drain_deadline_seconds) + "s");

    // 1. Stop accepting new connections.
    acceptor_->stopListening();

    // 2. Ask each live connection to finish and half-close. Copy the pointers
    //    first since callbacks may mutate the map.
    std::vector<TcpConnectionPtr> conns;
    conns.reserve(connections_.size());
    for (auto& [name, conn] : connections_) {
        conns.push_back(conn);
    }
    for (const auto& conn : conns) {
        conn->getLoop()->runInLoop([this, conn]() {
            if (drain_callback_) {
                drain_callback_(conn);  // app-level orderly close hook
            }
            conn->shutdown();  // half-close: peer sees EOF once output drains
        });
    }
    // 3. If there were no connections, we're already done.
    if (connections_.empty()) {
        maybeFinishDrain();
        return;
    }

    // 4. Arm the force-close deadline. Any connections still alive when it
    //    fires are forcibly closed.
    if (drain_deadline_seconds > 0) {
        loop_->runAfter(static_cast<uint64_t>(drain_deadline_seconds) * 1000,
                        [this]() { forceCloseAllInLoop(); });
    }
}

void TcpServer::forceCloseAllInLoop() {
    loop_->assertInLoopThread();
    if (connections_.empty()) {
        return;
    }
    log().warning("TcpServer::forceCloseAll [" + name_ +
                  "] deadline reached; " + "force-closing " +
                  std::to_string(connections_.size()) + " connection(s)");
    std::vector<TcpConnectionPtr> conns;
    conns.reserve(connections_.size());
    for (auto& [name, conn] : connections_) {
        conns.push_back(conn);
    }

    for (const auto& conn : conns) {
        conn->forceClose();  // marshals to the connection's own loop
    }
}

void TcpServer::maybeFinishDrain() {
    loop_->assertInLoopThread();
    if (draining_ && connections_.empty() && drain_complete_callback_) {
        log().info("TcpServer::shutdown [" + name_ + "] drain complete");
        DrainCompleteCallback cb;
        cb.swap(drain_complete_callback_);  // fire once
        cb();
    }
}

}  // namespace net
