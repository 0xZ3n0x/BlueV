// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <arpa/inet.h>

#include "platform/posix/net_address.hpp"

using namespace bluev;

TEST(NetAddress, IsEmptyOrAnIpv4AddressThatIsNotTheLoopback)
{
    const std::string text = localIpv4Address();

    if (text.empty())
    {
        GTEST_SKIP() << "no network interface is up on this machine";
    }
    in_addr address{};
    ASSERT_EQ(1, inet_pton(AF_INET, text.c_str(), &address)) << text;
    EXPECT_NE(htonl(INADDR_LOOPBACK), address.s_addr);
}
