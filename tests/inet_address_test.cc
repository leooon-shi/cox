#include "inet_address.h"

#include <gtest/gtest.h>

#include <stdexcept>

using net::InetAddress;

TEST(InetAddressTest, Ipv4FromString)
{
    InetAddress addr("127.0.0.1", 8080);
    EXPECT_FALSE(addr.isIpv6());
    EXPECT_EQ(addr.family(), AF_INET);
    EXPECT_EQ(addr.toIp(), "127.0.0.1");
    EXPECT_EQ(addr.port(), 8080);
    EXPECT_EQ(addr.toIpPort(), "127.0.0.1:8080");
    EXPECT_EQ(addr.getSockLen(), sizeof(struct sockaddr_in));
}

TEST(InetAddressTest, Ipv4Wildcard)
{
    InetAddress addr(9000);   // any address, IPv4
    EXPECT_FALSE(addr.isIpv6());
    EXPECT_EQ(addr.toIp(), "0.0.0.0");
    EXPECT_EQ(addr.port(), 9000);
}

TEST(InetAddressTest, Ipv4Loopback)
{
    InetAddress addr(1234, /*loopback_only=*/true);
    EXPECT_EQ(addr.toIp(), "127.0.0.1");
    EXPECT_EQ(addr.port(), 1234);
}

TEST(InetAddressTest, Ipv6FromString)
{
    InetAddress addr("::1", 8080, /*ipv6=*/true);
    EXPECT_TRUE(addr.isIpv6());
    EXPECT_EQ(addr.family(), AF_INET6);
    EXPECT_EQ(addr.toIp(), "::1");
    EXPECT_EQ(addr.port(), 8080);
    EXPECT_EQ(addr.toIpPort(), "[::1]:8080");
    EXPECT_EQ(addr.getSockLen(), sizeof(struct sockaddr_in6));
}

TEST(InetAddressTest, Ipv6Wildcard)
{
    InetAddress addr(9000, /*loopback_only=*/false, /*ipv6=*/true);
    EXPECT_TRUE(addr.isIpv6());
    EXPECT_EQ(addr.toIp(), "::");
    EXPECT_EQ(addr.port(), 9000);
}

TEST(InetAddressTest, Ipv6Lookback)
{
    InetAddress addr(1234, /*loopback_only=*/true, /*ipv6=*/true);
    EXPECT_TRUE(addr.isIpv6());
    EXPECT_EQ(addr.toIp(), "::1");
}

TEST(InetAddressTest, Ipv6FullAddress)
{
    InetAddress addr("fe80::1", 443, /*ipv6=*/true);
    EXPECT_EQ(addr.toIp(), "fe80::1");
    EXPECT_EQ(addr.toIpPort(), "[fe80::1]:443");
}

TEST(InetAddressTest, InvalidIpv4Throws)
{
    EXPECT_THROW(InetAddress("not.an.ip", 80), std::invalid_argument);
}

TEST(InetAddressTest, InvalidIpv6Throws)
{
    EXPECT_THROW(InetAddress("::zz::", 80, /*ipv6=*/true), std::invalid_argument);
}

TEST(InetAddressTest, RoundTripFromSockAddr)
{
    InetAddress orig("192.168.1.50", 5555);
    const struct sockaddr_in* sa =
        reinterpret_cast<const struct sockaddr_in*>(orig.getSockAddr());
    InetAddress copy(*sa);
    EXPECT_EQ(copy.toIpPort(), "192.168.1.50:5555");
}

TEST(InetAddressTest, UnixDomainBasic)
{
    InetAddress addr = InetAddress::unixDomain("/tmp/cox_test.sock");
    EXPECT_TRUE(addr.isUnix());
    EXPECT_FALSE(addr.isIpv6());
    EXPECT_EQ(addr.family(), AF_UNIX);
    EXPECT_EQ(addr.unixPath(), "/tmp/cox_test.sock");
    EXPECT_EQ(addr.toIpPort(), "unix:/tmp/cox_test.sock");
    EXPECT_EQ(addr.port(), 0);
    EXPECT_EQ(addr.toIp(), "");
}

TEST(InetAddressTest, UnixDomainPathTooLongThrows)
{
    std::string long_path(200, 'a');  // exceeds sun_path (108 on Linux)
    EXPECT_THROW(InetAddress::unixDomain(long_path), std::invalid_argument);
}

TEST(InetAddressTest, unixDomainSockLen)
{
    InetAddress addr = InetAddress::unixDomain("/tmp/x.sock");
    // Length covers the family field + used path + NUL.
    EXPECT_GT(addr.getSockLen(), 0u);
    EXPECT_LT(addr.getSockLen(), sizeof(struct sockaddr_un) + 1);
}
