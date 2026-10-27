// SPDX-License-Identifier: GPL-3.0-or-later
// Where adapter firmware files are kept. Some USB controllers boot into a
// limited ROM mode and only work after the host uploads a patch (the Realtek
// RTL8761B family, for one). The blobs are not part of BlueV, so they are read
// from a directory at run time.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bluev
{

class IFirmwareStore
{
  public:
    virtual ~IFirmwareStore() = default;

    // Reads the plain file name `name` (no path) into `out`. False when the
    // file does not exist or cannot be read.
    virtual bool read(const std::string& name, std::vector<std::uint8_t>& out) = 0;
};

} // namespace bluev
