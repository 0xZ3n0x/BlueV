// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal SDP server: answers service searches for the A2DP audio source
// record so that a device recognises us as a media source.
#pragma once

#include <cstdint>

namespace bluev
{

class SdpServer
{
  public:
    // Handles one SDP request; writes the response into `out` (at most
    // `outcap` bytes) and returns its length, or 0 when there is nothing to
    // answer. The server keeps no state.
    static int handle(const std::uint8_t* request, std::uint32_t requestLength, std::uint8_t* out, int outcap);
};

} // namespace bluev
