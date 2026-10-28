// SPDX-License-Identifier: GPL-3.0-or-later
// Firmware blobs as plain files in one directory, for example
// /data/bluev/firmware/rtl8761bu_fw.bin.
#pragma once

#include <string>
#include <utility>

#include "core/ports/firmware_store.hpp"

namespace bluev
{

class FileFirmwareStore final : public IFirmwareStore
{
  public:
    explicit FileFirmwareStore(std::string directory) : m_dir(std::move(directory))
    {
    }

    bool read(const std::string& name, std::vector<std::uint8_t>& out) override;
    std::string location() const override
    {
        return m_dir;
    }

  private:
    std::string m_dir;
};

} // namespace bluev
