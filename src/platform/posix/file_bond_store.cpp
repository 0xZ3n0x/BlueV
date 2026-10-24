// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/posix/file_bond_store.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

namespace bluev
{

namespace
{

std::string hexAddress(const BdAddr& a)
{
    char text[13];

    std::snprintf(text, sizeof text, "%02X%02X%02X%02X%02X%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    return text;
}

bool parseHex(const char* text, BdAddr& out)
{
    unsigned v[6];

    if (std::strlen(text) != 12 ||
        std::sscanf(text, "%2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    {
        return false;
    }
    for (int i = 0; i < 6; i++)
    {
        out[5 - i] = static_cast<std::uint8_t>(v[i]);
    }
    return true;
}

// "bond-<first>-<second>.key"
bool parseKeyName(const char* name, char* first, char* second)
{
    char ext[8];

    return std::sscanf(name, "bond-%12[0-9A-F]-%12[0-9A-F].%7s", first, second, ext) == 3 &&
           std::strcmp(ext, "key") == 0 && std::strlen(first) == 12 && std::strlen(second) == 12;
}

std::string readSmall(const std::string& path, std::size_t max)
{
    std::string out;
    std::FILE* f = std::fopen(path.c_str(), "rb");

    if (f == nullptr)
    {
        return out;
    }
    char buf[256];
    const std::size_t n = std::fread(buf, 1, max < sizeof buf ? max : sizeof buf, f);

    out.assign(buf, n);
    std::fclose(f);
    return out;
}

} // namespace

std::string FileBondStore::keyPath(const BdAddr& local, const BdAddr& peer) const
{
    return m_dir + "/bond-" + hexAddress(local) + "-" + hexAddress(peer) + ".key";
}

std::string FileBondStore::namePath(const BdAddr& peer) const
{
    return m_dir + "/bond-" + hexAddress(peer) + ".name";
}

bool FileBondStore::hasKey(const BdAddr& local, const BdAddr& peer)
{
    return access(keyPath(local, peer).c_str(), F_OK) == 0;
}

bool FileBondStore::loadKey(const BdAddr& local, const BdAddr& peer, BondKey& out)
{
    const std::string blob = readSmall(keyPath(local, peer), 23);

    out = BondKey{};
    if (blob.size() != 23)
    {
        return false;
    }
    std::memcpy(out.peer.data(), blob.data(), 6);
    std::memcpy(out.key.data(), blob.data() + 6, 16);
    out.type = static_cast<std::uint8_t>(blob[22]);
    out.have = true;
    return true;
}

bool FileBondStore::saveKey(const BdAddr& local, const BondKey& key)
{
    std::FILE* f = std::fopen(keyPath(local, key.peer).c_str(), "wb");
    std::uint8_t blob[23];

    if (f == nullptr)
    {
        return false;
    }
    std::memcpy(blob, key.peer.data(), 6);
    std::memcpy(blob + 6, key.key.data(), 16);
    blob[22] = key.type;
    const bool ok = std::fwrite(blob, 1, 23, f) == 23;
    return std::fclose(f) == 0 && ok;
}

void FileBondStore::removeKey(const BdAddr& local, const BdAddr& peer)
{
    std::remove(keyPath(local, peer).c_str());
}

std::string FileBondStore::loadName(const BdAddr& peer)
{
    return readSmall(namePath(peer), 63);
}

void FileBondStore::saveName(const BdAddr& peer, const std::string& name)
{
    std::FILE* f = std::fopen(namePath(peer).c_str(), "wb");

    if (f == nullptr)
    {
        return;
    }
    std::fwrite(name.data(), 1, name.size(), f);
    std::fclose(f);
}

std::vector<BondEntry> FileBondStore::list(const BdAddr& local)
{
    std::vector<BondEntry> out;
    DIR* d = opendir(m_dir.c_str());
    const std::string ours = hexAddress(local);

    if (d == nullptr)
    {
        return out;
    }
    while (dirent* de = readdir(d))
    {
        char first[16], second[16];
        BondEntry entry;

        if (!parseKeyName(de->d_name, first, second) || ours != first || !parseHex(second, entry.peer))
        {
            continue;
        }
        entry.name = loadName(entry.peer);
        out.push_back(entry);
        if (out.size() >= 16)
        {
            break;
        }
    }
    closedir(d);
    return out;
}

// the device for every adapter
void FileBondStore::forget(const BdAddr& peer)
{
    const std::string theirs = hexAddress(peer);
    DIR* d = opendir(m_dir.c_str());

    if (d != nullptr)
    {
        while (dirent* de = readdir(d))
        {
            char first[16], second[16];

            if (parseKeyName(de->d_name, first, second) && theirs == second)
            {
                std::remove((m_dir + "/" + de->d_name).c_str());
            }
        }
        closedir(d);
    }
    std::remove(namePath(peer).c_str());
}

} // namespace bluev
