#include "tls_connection.h"

#include "buffer.h"
#include "logger.h"
#include "tcp_connection.h"

#include <string>
#include <utility>

namespace tls {
namespace {
logging::Logger& log() {
    static logging::Logger logger;
    return logger;
}
}  // namespace

TlsConnection::TlsConnection(std::shared_ptr<net::TcpConnection> conn,
                             const TlsContextPtr& ctx, Role role)
    : conn_(std::move(conn)), engine_(*ctx, role) {}

TlsConnection::TlsConnection(std::shared_ptr<net::TcpConnection> conn,
                             const TlsContextPtr& ctx, Role role,
                             const std::string& server_hostname)
    : conn_(std::move(conn)), engine_(*ctx, role, server_hostname) {}

TlsConnection::~TlsConnection() = default;

void TlsConnection::start() {
    // Intercept the underlying TCP connection's byte stream.
    //
    // WEAK capture: this object holds conn_ (a shared_ptr to the TcpConnection)
    // so installing callbacks that own a shared_ptr back to this TlsConnection
    // forms a cycle and neither is ever freed -- along with the SSL object and
    // its buffers. LeakSanitizer reported this as ~900KB across ~12900
    // allocations in the HTTPS tests, dominated by OpenSSL state hanging off
    // the unreclaimed engine.
    std::weak_ptr<TlsConnection> weak = weak_from_this();
    conn_->setMessageCallback(
        [weak](const net::TcpConnectionPtr&, buffer::Buffer* buf) {
            if (auto self = weak.lock()) self->onTcpMessage(buf);
        });
    conn_->setConnectionCallback([weak](const net::TcpConnectionPtr& c) {
        if (c->connected()) return;
        if (auto self = weak.lock()) self->onTcpClose();
    });

    // A TLS client drives the handshake first (ClientHello); a server waits for
    // the client's ClientHello to arrive. Either way, advancing now flushes any
    // initial bytes the engine wants to send.
    driveHandshake();
}

void TlsConnection::driveHandshake() {
    if (handshake_done_ || closed_) {
        return;
    }

    TlsEngine::Status s = engine_.handshake();
    flushOutbound();  // send whatever handshake bytes were produced.

    if (s == TlsEngine::Status::kError) {
        log().error("TlsConnection handshake error: " + engine_.lastError());
        conn_->forceClose();
        return;
    }

    if (engine_.handshakeDone()) {
        handshake_done_ = true;
        log().info("TLS handshake complete: " + engine_.protocolVersion() +
                   " " + engine_.cipherName());
        if (handshake_done_callback_) {
            handshake_done_callback_(shared_from_this());
        }
    }
    // else kWantIo: waiting for more bytes from the peer (onTcpMessage will
    // drive again).
}

void TlsConnection::onTcpMessage(buffer::Buffer* buf) {
    // Feed all received encrypted bytes into the engine.
    const size_t n = buf->readableBytes();
    if (n > 0) {
        engine_.feedEncrypted(buf->peek(), n);
        buf->retrieve(n);
    }

    if (!handshake_done_) {
        driveHandshake();
        if (!handshake_done_) {
            return;  // still handshaking
        }
        // Handshake just finished; fall through to read any early app data.
    }

    // Decrypt available application plaintext.
    std::string plaintext;
    TlsEngine::Status s = engine_.readPlaintext(plaintext);
    // readPlaintext may also produce outbound bytes (e.g. TLS 1.3 session
    // tickets, renegotiation); flush them.
    flushOutbound();

    if (!plaintext.empty() && message_callback_) {
        message_callback_(shared_from_this(), plaintext);
    }
    if (s == TlsEngine::Status::kClosed) {
        log().info("TLS peer sent close_notify");
        conn_->shutdown();
    } else if (s == TlsEngine::Status::kError) {
        log().error("TLS read error: " + engine_.lastError());
        conn_->forceClose();
    }
}

void TlsConnection::onTcpClose() {
    if (closed_) return;
    closed_ = true;
    if (close_callback_) {
        close_callback_(shared_from_this());
    }
}

void TlsConnection::send(const std::string& data) {
    if (!handshake_done_ || closed_) {
        log().warning("TlsConnection::send before handshake or after close");
        return;
    }
    size_t offset = 0;
    while (offset < data.size()) {
        size_t consumed = 0;
        TlsEngine::Status s = engine_.writePlaintext(
            data.data() + offset, data.size() - offset, &consumed);
        if (s == TlsEngine::Status::kOk) {
            offset += consumed;
            flushOutbound();
        } else if (s == TlsEngine::Status::kWantIo) {
            flushOutbound();  // push what we have; retry remaining
        } else {
            log().error("TLS write error: " + engine_.lastError());
            conn_->forceClose();
            return;
        }
    }
}

void TlsConnection::shutdown() {
    if (closed_) return;
    engine_.shutdown();  // queue close_notify
    flushOutbound();
    conn_->shutdown();  // half-close the TCP side after flushing
}

void TlsConnection::flushOutbound() {
    std::string enc;
    engine_.takeEncrypted(enc);
    if (!enc.empty()) {
        conn_->send(enc);
    }
}
}  // namespace tls
