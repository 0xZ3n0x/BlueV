// SPDX-License-Identifier: GPL-3.0-or-later
// Where pairings are kept. A link key belongs to the pair (our adapter, the
// device); the name belongs to the device.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/bluetooth/address.hpp"

namespace bluev
{

// A link key. `have` is false for "no key".
struct BondKey
{
    bool have = false;
    BdAddr peer{};
    std::array<std::uint8_t, 16> key{};
    std::uint8_t type = 0; // key type from the Link Key Notification event
};

// A paired device as the list shows it.
struct BondEntry
{
    BdAddr peer{};
    std::string name;
};

class IBondStore
{
  public:
    virtual ~IBondStore() = default;

    // `local` is the address of our adapter, `peer` the device.
    virtual bool hasKey(const BdAddr& local, const BdAddr& peer) = 0;
    virtual bool loadKey(const BdAddr& local, const BdAddr& peer, BondKey& out) = 0;
    // Stores the key of key.peer; false when it could not be written.
    virtual bool saveKey(const BdAddr& local, const BondKey& key) = 0;
    // Removes the key of one adapter and the device; the name stays.
    virtual void removeKey(const BdAddr& local, const BdAddr& peer) = 0;

    // Empty when no name was saved.
    virtual std::string loadName(const BdAddr& peer) = 0;
    virtual void saveName(const BdAddr& peer, const std::string& name) = 0;

    // the devices paired with this adapter, with their names
    virtual std::vector<BondEntry> list(const BdAddr& local) = 0;
    // every key of the device, for every adapter, and its name
    virtual void forget(const BdAddr& peer) = 0;
};

} // namespace bluev
