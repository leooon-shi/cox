#pragma once

#include "tls_context.h"
#include "tls_engine.h"

#include <functional
#include <memory>
#include <string>

namespace buff
{
class Buffer;
}
namespace net
{
class TcpConnection;
}

namespace tls
{
class TlsConnection;
using TlsConnectionPtr = std::shared_ptr<TlsConnection>;

// TlsConnection layers TLS over an established net::TcpConnection using a
// TlsEngine. It intercepts the TCP connection's byte stream: inbound encrypted
// bytes are fed to the engine and surfaced as decrypted plaintext to the
// application; outbound plaintext is encrypted and written to the TCP
// connection.
//
// Lifecycle: create a TlsConnection wrapping a freshly established
// TcpConnection (e.g. in TcpServer's connection callback). set callbacks, and
// call start(). The TLS handshake runs first; the onHandshakeDone callback
// fires when the secure channel is ready, after which send/onMessage carry
// application plaintext.
class TlsConnection : public std::enable_shared_from_this<TlsConnection>
{
public:
    using HandshakeDoneCallback = std::function<void(const TlsConnectionPtr&)>;
    using MessageCallback =
        std::function<void(const TlsConnectionPtr&, std::string& plaintext)>;
    using CloseCallback = std::function<void(const TlsConnectionPtr&)>;

    TlsConnection(std::shared_ptr<net::TcpConnection> conn,
                  const TlsContextPtr& ctx, Role role);
    // Client role `server_hostname` is sent as SNI (so a multi-domain server
    // selects the right certificate) and used for certificate hostname
    // verification. Ignored for the server role.
    TlsConnection(std::shared_ptr<net::TcpConnection> conn,
                  const TlsContextPtr& ctx, Role role,
                  const std::string& server_hostname);
    ~TlsConnection();

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    // Begin the TLS handshake. Must be called once after construction.
    void start();

    // Send application plaintext (encrypted before hitting the wire).
    void send(const std::string& data);

    // Initiate a clean TLS + TCP shutdown.
    void shutdown();

    bool handshakeDone() const { return handshake_done_; }
    std::string protocolVersion() const { return engine_.protocolVersion(); }
    std::string cipherName() const { return engine_.cipherName(); }
    // Server role: hostname the client requested via SNI ("" if none).
    std::string requestedServerName() const
    {
        return engine_.requestedServerName();
    }

    void setHandshakeDoneCallback(HandshakeDoneCallback cb) {
        handshake_done_callback_ = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb)
    {
        message_callback_ = std::move(cb);
    }
    void setCloseCallback(CloseCallback cb) { close_callback_ = std::move(cb); }

    const std::shared_ptr<net::TcpConnection>& tcpConnection() const
    { 
        return conn_; 
    }

private:
    // Fed inbound encrypted bytes from the underlying TcpConnection.
    void onTcpMessage(buffer::Buffer* buf);
    void onTcpClose();

    // Push any pending outbound encrypted bytes from the engine to the wire.
    void flushOutbound();
    // Drive the handshake forward; fires the done callback on completion.
    void driveHandshake();

    std::shared_ptr<net::TcpConnection> conn_;
    TlsEngine engine_;
    // NOTE: the role only affects how the engine is constructed (and is queried
    // via the engine when needed), so it is deliberately NOT stored here.
    bool handshake_done_{false};
    bool closed_{false};

    HandshakeDoneCallback handshake_done_callback_;
    MessageCallback message_callback_;
    CloseCallback close_callback_;
};
} // namespace tls
