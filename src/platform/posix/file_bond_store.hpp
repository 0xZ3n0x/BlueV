// SPDX-License-Identifier: GPL-3.0-or-later
// Pairings as plain files in one directory:
//   bond-<ours>-<theirs>.key   the link key of the pair (our adapter, the device)
//   bond-<theirs>.name         the device's name
// The same device can so be paired with the console's chip and with a USB
// adapter at the same time. In the file names addresses are written most
// significant byte first; inside a key file they keep the order HCI uses.
// Every call reads or writes the directory; nothing is cached.
#pragma once

#include <string>

#include "core/ports/bond_store.hpp"

namespace bluev
{

class FileBondStore final : public IBondStore
{
  public:
    explicit FileBondStore(std::string directory) : m_dir(std::move(directory))
    {
    }

    bool hasKey(const BdAddr& local, const BdAddr& peer) override;
    bool loadKey(const BdAddr& local, const BdAddr& peer, BondKey& out) override;
    bool saveKey(const BdAddr& local, const BondKey& key) override;
    void removeKey(const BdAddr& local, const BdAddr& peer) override;
    std::string loadName(const BdAddr& peer) override;
    void saveName(const BdAddr& peer, const std::string& name) override;
    std::vector<BondEntry> list(const BdAddr& local) override;
    void forget(const BdAddr& peer) override;

  private:
    std::string keyPath(const BdAddr& local, const BdAddr& peer) const;
    std::string namePath(const BdAddr& peer) const;

    std::string m_dir;
};

} // namespace bluev
