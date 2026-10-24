// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bluetooth/address.hpp"

#include <cstdio>
#include <cstring>

namespace bluev
{

AddrText addressText(const BdAddr& a)
{
    AddrText out;

    std::snprintf(out.text, sizeof out.text, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    return out;
}

std::string addressString(const BdAddr& address)
{
    return addressText(address).c_str();
}

bool parseAddress(std::string_view text, BdAddr& out)
{
    char copy[32];
    unsigned v[6];
    int consumed = 0;

    if (text.size() >= sizeof copy)
    {
        return false;
    }
    std::memcpy(copy, text.data(), text.size());
    copy[text.size()] = 0;
    if (std::sscanf(copy, "%2x:%2x:%2x:%2x:%2x:%2x%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &consumed) != 6)
    {
        return false;
    }
    if (static_cast<std::size_t>(consumed) != text.size()) // nothing may follow the address
    {
        return false;
    }
    for (int i = 0; i < 6; i++)
    {
        out[5 - i] = static_cast<std::uint8_t>(v[i]);
    }
    return true;
}

BdAddr addressFrom(const std::uint8_t* six)
{
    BdAddr a;

    std::memcpy(a.data(), six, 6);
    return a;
}

} // namespace bluev
