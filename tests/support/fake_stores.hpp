// SPDX-License-Identifier: GPL-3.0-or-later
// In-memory versions of the bond store and the USB directory.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/ports/bond_store.hpp"
#include "core/ports/usb_directory.hpp"

namespace bluev::testing
{

class FakeBondStore final : public IBondStore
{
  public:
    bool hasKey(const BdAddr& local, const BdAddr& peer) override
    {
        return m_keys.count({local, peer}) != 0;
    }
    bool loadKey(const BdAddr& local, const BdAddr& peer, BondKey& out) override
    {
        auto it = m_keys.find({local, peer});
        if (it == m_keys.end())
        {
            return false;
        }
        out = it->second;
        return true;
    }
    bool saveKey(const BdAddr& local, const BondKey& key) override
    {
        if (failWrites)
        {
            return false;
        }
        m_keys[{local, key.peer}] = key;
        return true;
    }
    void removeKey(const BdAddr& local, const BdAddr& peer) override
    {
        m_keys.erase({local, peer});
    }

    std::string loadName(const BdAddr& peer) override
    {
        auto it = m_names.find(peer);
        return it == m_names.end() ? std::string() : it->second;
    }
    void saveName(const BdAddr& peer, const std::string& name) override
    {
        m_names[peer] = name;
    }

    std::vector<BondEntry> list(const BdAddr& local) override
    {
        std::vector<BondEntry> out;
        for (const auto& [k, v] : m_keys)
        {
            if (k.first == local)
            {
                out.push_back({k.second, loadName(k.second)});
            }
        }
        return out;
    }
    void forget(const BdAddr& peer) override
    {
        for (auto it = m_keys.begin(); it != m_keys.end();)
        {
            it = it->first.second == peer ? m_keys.erase(it) : std::next(it);
        }
        m_names.erase(peer);
    }

    // for tests
    void put(const BdAddr& local, const BdAddr& peer, std::uint8_t fill = 0x5a, const char* name = "")
    {
        BondKey k;
        k.have = true;
        k.peer = peer;
        k.key.fill(fill);
        k.type = 4;
        m_keys[{local, peer}] = k;
        if (*name)
        {
            m_names[peer] = name;
        }
    }
    std::size_t keyCount() const
    {
        return m_keys.size();
    }
    bool failWrites = false;

  private:
    std::map<std::pair<BdAddr, BdAddr>, BondKey> m_keys;
    std::map<BdAddr, std::string> m_names;
};

class FakeUsbDirectory final : public IUsbDirectory
{
  public:
    const char* internalNode() const override
    {
        return "ugen0.2";
    }
    std::vector<UsbDevice> adapters() override
    {
        return devices;
    }
    bool nodeInfo(const char* node, UsbDevice& out) override
    {
        for (const UsbDevice& d : devices)
        {
            if (d.node == node)
            {
                out = d;
                return true;
            }
        }
        return false;
    }
    bool discoverLayout(const char* node, HciLayout& out) override
    {
        auto it = layouts.find(node);
        if (it == layouts.end())
        {
            return false;
        }
        out = it->second;
        return true;
    }

    std::vector<UsbDevice> devices;
    std::map<std::string, HciLayout> layouts;
};

} // namespace bluev::testing
