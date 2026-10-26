#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstdint>
#include <string>

namespace net
{
// A wrapper around a socket address. Despite the historical name, it can hold
// an IPv4 (sockaddr_in), IPv6 (sockaddr_in6), or Unix-domain (sockaddr_un)
// address. The family is discriminated by the sa_family field.
//
// This unification (one address type across families, a la folly::SocketAddress)
// lets Acceptor/Connector/TcpConnection work with IP and Unix sockets without
// duplicating the connection machinery: a connected Unix stream socket has the
// same read/write/accept/connect semantics as a TCP socket.
class InetAddress {
public:
    // ---- IP constructors ----
    
    // Construct from a port. Binds to the wildcard address (0.0.0.0 for IPv4,
    // :: for IPv6) by default, or to loopback (127.0.0.1 / ::1) when
    // loopback only is true. Set ipv6=true to create an IPv6 address.
    explicit InetAddress(uint16_t port=0, bool loopback_only = false,
                        bool ipv6 = false);
    // Construct from a numeric IP string and port (IPv6 when ipv6=true).
    InetAddress(const std::string& ip, uint16_t port, bool ipv6 = false);

    // Construct from a raw sockaddr_in (IPv4).
    explicit InetAddress(const struct sockaddr_in& addr) : addr_(addr) {}
    // Construct from a raw sockaddr_in6 (IPv6).
    explicit InetAddress(const struct sockaddr_in6& addr) : addr6_(addr) {}
    // Construct from a raw sockaddr_un (Unix domain).
    explicit InetAddress(const struct sockaddr_un& addr) : addr_un_(addr) {}

    // ---- Unix-domain factory ----

    // Build a Unix-domain address from a filesystem path. Paths longer than the
    // platform limit (sizeof(sun_path)-1) throw std::invalid_argument.
    static InetAddress unixDomain(const std::string& path);

    sa_family_t family() const { return storage_family_; }
    bool isIpv6() const { return family() == AF_INET6; }
    bool isUnix() const { return family() == AF_UNIX; }

    // For Unix addresses, the filesystem path (empty ofr IP addresses).
    std::string unixPath() const;

    // Numeric IP without port (empty for Unix addresses).
    std::string toIp() const;
    // Human-readable address:
    //     IPv4  -> "127.0.0.1:8080"
    //     IPv6  -> "[::1]:8080"
    //     Unix  -> "unix:/path/to/socket"
    std::string toIpPort() const;
    // Host-byte-order port (0 for Unix addresses).
    uint16_t port() const;

    const struct sockaddr* getSockAddr() const {
        return reinterpret_cast<const struct sockaddr*>(&addr6_);
    }

    // Size of the underlying sockaddr for the current family.
    socklen_t getSockLen() const;

    void setSockAddrInet6(const struct sockaddr_in6& addr6) { addr6_ = addr6; }
    // Copy a raw address of the given length into this object (used by accept / 
    // getpeername / getsockname which fill a sockaddr_storage).
    void setSockAddrStorage(const struct sockaddr_storage& ss, socklen_t len);
private:
    union {
        sa_family_t storage_family_;    // aliases the family field of all members
        struct sockaddr_in addr_;
        struct sockaddr_in6 addr6_;
        struct sockaddr_un addr_un_;
        struct sockaddr_storage storage_;
    };
};
} // namespace net
