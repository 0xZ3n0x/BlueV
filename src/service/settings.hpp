// SPDX-License-Identifier: GPL-3.0-or-later
// What BlueV remembers between runs, in one small text file of `key=value`
// lines:
//   autostart=1          open the radio when BlueV starts
//   adapter=0a12:0001    the USB adapter to open then (vendor:product, in hex)
//   quality=0            the sound quality: 0 normal, 1 high (the bitpool, see a2dp_params.hpp)
// A missing or damaged file gives the defaults; unknown lines are ignored.
#pragma once

#include <cstdint>
#include <string>

namespace bluev
{

struct Settings
{
    bool autostart = false;
    std::uint16_t vid = 0, pid = 0; // the adapter; 0:0 when none is saved
    int quality = 0;
};

class SettingsFile final
{
  public:
    explicit SettingsFile(std::string path) : m_path(std::move(path))
    {
    }

    Settings load() const;
    // False when the file could not be written.
    bool save(const Settings& settings) const;

  private:
    std::string m_path;
};

} // namespace bluev
