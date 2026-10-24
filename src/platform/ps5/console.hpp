// SPDX-License-Identifier: GPL-3.0-or-later
// Facts about the console's hardware that more than one adapter needs.
#pragma once

namespace bluev
{

// The ugen node of the console's own Bluetooth chip, which the system's stack uses.
constexpr const char* kConsoleBluetoothNode = "ugen0.2";

} // namespace bluev
