#include "inet_address.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/un.h>

#include <cstddef>  // offsetof
#include <cstring>
#include <stdexcept>
#include <string>

namespace net {
namespace
{
// Compile-time sanity: the union must be laid out so the family field aliases
// across sockaddr_in / sockaddr_in6 / sockaddr_un. This holds on POSIX; assert
// it to document the assumption.
static_assert(offsetof(struct sockaddr_in, sin_family) == 
                  offsetof(struct sockaddr_in6, sin6_family),
            "sin_family and sin6_family must alias");
static_assert(offsetof(struct sockaddr_in, sin_family) ==
                  offsetof(struct sockaddr_un, sun_family),
            "sin_family and sun_family must alias");
} // namespace

InetAddress InetAddress::unixDomain(const std::string& path)
{
    struct sockaddr_un un;
    std::memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    if (path.size() >= sizeof(un.sun_path))
    {
        throw std::invalid_argument(
            "InetAddress::unixDomain: path too long (max " +
            std::to_string(sizeof(un.sun_path) - 1) + "): " + path);
    }
    std::memcpy(un.sun_path, path.c_str(), path.size());
    return InetAddress(un);
}

std::string InetAddress::unixPath() const {
    if (!isUnix())
    {
        return std::string();
    }
    // sun_path is null-terminated for pathname sockets we create.
    return std::string(addr_un_.sun_path);
}

void InetAddress::setSockAddrStorage(const struct sockaddr_storage & ss, 
                                     socklen_t len)
{
    std::memset(&storage_, 0, sizeof(storage_));
    std::memcpy(&storage_, &ss, len > static_cast<socklen_t>(sizeof(storage_))
                                    ? sizeof(storage_) 
                                    : static_cast<size_t>(len));
}

socklen_t InetAddress::getSockLen() const {
    switch (family())
    {
        case AF_INET6:
            return static_cast<socklen_t>(sizeof(addr6_));
        case AF_UNIX:
            // Length up to and including the used portion of sun_path.
            return static_cast<socklen_t>(
                offsetof(struct sockaddr_un, sun_path) +
                std::strlen(addr_un_.sun_path) + 1);
        case AF_INET:
        default:
            return static_cast<socklen_t>(sizeof(addr_));
    }
}

InetAddress::InetAddress(uint16_t port, bool loopback_only, bool ipv6)
{
    if (ipv6) {
        std::memset(&addr6_, 0, sizeof(addr6_));
        addr6_.sin6_family = AF_INET6;
        addr6_.sin6_addr = loopback_only ? in6addr_loopback : in6addr_any;
        addr6_.sin6_port = htons(port);
    }
    else
    {
        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_addr.s_addr =
            htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
        addr_.sin_port = htons(port);
    }
}

InetAddress::InetAddress(const std::string& ip, uint16_t port, bool ipv6)
{
    if (ipv6)
    {
        std::memset(&addr6_, 0, sizeof(addr_));
        addr6_.sin6_family = AF_INET6;
        addr6_.sin6_port = htons(port);
        if (::inet_pton(AF_INET6, ip.c_str(), &addr6_.sin6_addr) <= 0)
        {
            throw std::invalid_argument("InetAddress: invalid IPv6 address: " + ip);
        }
    }
    else
    {
        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0)
        {
            throw std::invalid_argument("InetAddress: invalid IPv4 address: " + ip);
        }
    }
}

uint16_t InetAddress::port() const {
    if (isUnix())
    {
        return 0;
    }
    return isIpv6() ? ntohs(addr6_.sin6_port) : ntohs(addr_.sin_port);
}

std::string InetAddress::toIp() const
{
    if (isUnix())
    {
        return std::string();
    }
    char buf[INET6_ADDRSTRLEN] = {};
    if (isIpv6())
    {
        ::inet_ntop(AF_INET6, &addr6_.sin6_addr, buf, sizeof(buf));
    }
    else
    {
        ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

std::string InetAddress::toIpPort() const
{
    if (isUnix())
    {
        return "unix:" + unixPath();
    }
    const std::string ip = toIp();
    const std::string p = std::to_string(port());
    // Bracket IPv6 literals so the port delimiter is unambiguous.
    if (isIpv6())
    {
        return "[" + ip + "]:" + p;
    }
    return ip + ":" + p;
}

} // namespace net