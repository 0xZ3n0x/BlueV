// SPDX-License-Identifier: GPL-3.0-or-later
// The address other devices on the network can reach this machine at.
#pragma once

#include <string>

namespace bluev
{

// The first IPv4 address of an interface that is up and is not the loopback,
// as text ("192.168.1.20"); empty when there is none.
std::string localIpv4Address();

} // namespace bluev
