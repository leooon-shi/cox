#include "tcp_client.h"

#include "event_loop.h"
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

TcpClient::TcpClient(reactor::EventLoop* loop, const InetAddress& server_addr,
                     std::string name)
    : loop_(loop),
      connector_(std::make_shared<Connector>(loop, server_addr)),
      name_(std::move(name)) {
    connector_->setNewConnectionCallback(
        [this](int sockfd) { newConnection(sockfd); });
}

TcpClient::TcpClient(reactor::EventLoop* loop,
                     const std::vector<InetAddress> candidates,
                     std::string name)
    : loop_(loop),
      connector_(std::make_shared<Connector>(loop, std::move(candidates))),
      name_(std::move(name)) {
    connector_->setNewConnectionCallback(
        [this](int sockfd) { newConnection(sockfd); });
}

TcpClient::~TcpClient() {
    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }

    if (conn) {
        // Tear the connection down on its loop. If we're already on the loop
        // thread (e.g. destroyed from within a callback after quit()), run it
        // directly -- runInLoop would just queue a task the stopped loop will
        // never execute, leaving the connection half-torn-down.
        auto* loop = conn->getLoop();
        auto teardown = [conn]() { conn->connectDestroyed(); };
        if (loop->isInLoopThread()) {
            teardown();
        } else {
            loop->runInLoop(teardown);
        }
        // Release our own reference: connectDestroyed() runs the close
        // bookkeeping but does NOT drop this member, so without the reset the
        // TcpConnection (and its input/output Buffers) outlives the client.
        // LeakSanitizer found this as an unreclaimed TcpConnection.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connection_.reset();
        }
        // Stop the connector too: on this path it was left running, so its
        // channel/socket state was never released either.
        connector_->stop();
    } else {
        connector_->stop();
    }
}

void TcpClient::connect() {
    connect_ = true;
    connector_->start();
}

void TcpClient::disconnect() {
    connect_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_) {
        connection_->shutdown();
    }
}

void TcpClient::stop() {
    connect_ = false;
    connector_->stop();
}

TcpConnectionPtr TcpClient::connection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_;
}

void TcpClient::newConnection(int sockfd) {
    loop_->assertInLoopThread();
    InetAddress peer = sockets::getPeerAddr(sockfd);
    InetAddress local = sockets::getLocalAddr(sockfd);
    std::string conn_name =
        name_ + "-" + peer.toIpPort() + "#" + std::to_string(next_conn_id_++);
    log().info("TcpClient::newConnection [" + name_ + "] " + conn_name);

    auto conn =
        std::make_shared<TcpConnection>(loop_, conn_name, sockfd, local, peer);
    conn->setConnectionCallback(connection_callback_);
    conn->setMessageCallback(message_callback_);
    conn->setWriteCompleteCallback(write_complete_callback_);
    conn->setCloseCallback(
        [this](const TcpConnectionPtr& c) { removeConnection(c); });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;
    }
    conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_.reset();
    }
    conn->getLoop()->runInLoop([conn]() { conn->connectDestroyed(); });
    if (retry_ && connect_) {
        log().info("TcpClient::removeConnection [" + name_ +
                   "] reconnecting to " +
                   connector_->serverAddress().toIpPort());
        connector_->restart();
    }
}
}  // namespace net
