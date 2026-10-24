// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/posix/net_address.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace bluev
{

std::string localIpv4Address()
{
    ifaddrs* list = nullptr;
    std::string found;

    if (getifaddrs(&list) != 0)
    {
        return found;
    }
    for (const ifaddrs* entry = list; entry != nullptr && found.empty(); entry = entry->ifa_next)
    {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET || (entry->ifa_flags & IFF_UP) == 0 ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0)
        {
            continue;
        }
        char text[INET_ADDRSTRLEN];
        const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);

        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof text) != nullptr)
        {
            found = text;
        }
    }
    freeifaddrs(list);
    return found;
}

} // namespace bluev
