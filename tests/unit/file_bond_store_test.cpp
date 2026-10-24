// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>

#include "platform/posix/file_bond_store.hpp"
#include "temp_dir.hpp"

using namespace bluev;
using namespace bluev::testing;

namespace
{

const BdAddr kLocal{{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
const BdAddr kOther{{0x01, 0x02, 0x03, 0x04, 0x05, 0x06}};
const BdAddr kSpeaker{{0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00}};
const BdAddr kHeadset{{0xFF, 0xEE, 0xDD, 0x22, 0x11, 0x00}};

BondKey keyFor(const BdAddr& peer, std::uint8_t fill)
{
    BondKey k;
    k.have = true;
    k.peer = peer;
    k.key.fill(fill);
    k.type = 5;
    return k;
}

} // namespace

TEST(FileBondStore, SavesAndLoadsAKeyInTheOldFileFormat)
{
    TempDir dir;
    FileBondStore store(dir.path());

    EXPECT_FALSE(store.hasKey(kLocal, kSpeaker));
    ASSERT_TRUE(store.saveKey(kLocal, keyFor(kSpeaker, 0x7e)));
    EXPECT_TRUE(store.hasKey(kLocal, kSpeaker));
    EXPECT_FALSE(store.hasKey(kOther, kSpeaker)); // belongs to the pair

    // bond-<ours>-<theirs>.key, 23 bytes: address, key, type
    ASSERT_TRUE(dir.exists("bond-665544332211-001122AABBCC.key"));
    std::ifstream in(dir.file("bond-665544332211-001122AABBCC.key"), std::ios::binary);
    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_EQ(23u, blob.size());
    EXPECT_EQ(0xCC, static_cast<unsigned char>(blob[0]));
    EXPECT_EQ(0x7e, static_cast<unsigned char>(blob[6]));
    EXPECT_EQ(5, blob[22]);

    BondKey loaded;
    ASSERT_TRUE(store.loadKey(kLocal, kSpeaker, loaded));
    EXPECT_TRUE(loaded.have);
    EXPECT_EQ(kSpeaker, loaded.peer);
    EXPECT_EQ(0x7e, loaded.key[15]);
    EXPECT_EQ(5, loaded.type);
}

TEST(FileBondStore, ALoadOfNothingOrOfAShortFileFails)
{
    TempDir dir;
    FileBondStore store(dir.path());
    BondKey k = keyFor(kSpeaker, 1);

    EXPECT_FALSE(store.loadKey(kLocal, kSpeaker, k));
    EXPECT_FALSE(k.have);
    std::ofstream(dir.file("bond-665544332211-001122AABBCC.key")) << "short";
    EXPECT_FALSE(store.loadKey(kLocal, kSpeaker, k));
    EXPECT_FALSE(k.have);
}

TEST(FileBondStore, RemoveDeletesOnlyThatPair)
{
    TempDir dir;
    FileBondStore store(dir.path());
    store.saveKey(kLocal, keyFor(kSpeaker, 1));
    store.saveKey(kOther, keyFor(kSpeaker, 2));

    store.removeKey(kLocal, kSpeaker);
    EXPECT_FALSE(store.hasKey(kLocal, kSpeaker));
    EXPECT_TRUE(store.hasKey(kOther, kSpeaker));
}

TEST(FileBondStore, NamesBelongToTheDevice)
{
    TempDir dir;
    FileBondStore store(dir.path());

    EXPECT_EQ("", store.loadName(kSpeaker));
    store.saveName(kSpeaker, "Creative Pebble");
    EXPECT_EQ("Creative Pebble", store.loadName(kSpeaker));
    EXPECT_TRUE(dir.exists("bond-001122AABBCC.name"));
}

TEST(FileBondStore, ListsThePairingsOfOneAdapterOnly)
{
    TempDir dir;
    FileBondStore store(dir.path());
    store.saveKey(kLocal, keyFor(kSpeaker, 1));
    store.saveKey(kLocal, keyFor(kHeadset, 2));
    store.saveKey(kOther, keyFor(kSpeaker, 3));
    store.saveName(kSpeaker, "Creative");
    std::ofstream(dir.file("bond-notes.txt")) << "x"; // stray files are ignored

    auto entries = store.list(kLocal);
    ASSERT_EQ(2u, entries.size());
    std::sort(entries.begin(), entries.end(), [](const BondEntry& a, const BondEntry& b) { return a.peer < b.peer; });
    EXPECT_EQ(kSpeaker, entries[0].peer.size() ? entries[0].peer : kSpeaker);
    bool sawSpeaker = false;
    for (const BondEntry& e : entries)
    {
        if (e.peer == kSpeaker)
        {
            sawSpeaker = true;
            EXPECT_EQ("Creative", e.name);
        }
    }
    EXPECT_TRUE(sawSpeaker);
    EXPECT_TRUE(store.list(BdAddr{{9, 9, 9, 9, 9, 9}}).empty());
}

TEST(FileBondStore, ForgetRemovesTheDeviceForEveryAdapter)
{
    TempDir dir;
    FileBondStore store(dir.path());
    store.saveKey(kLocal, keyFor(kSpeaker, 1));
    store.saveKey(kOther, keyFor(kSpeaker, 2));
    store.saveKey(kLocal, keyFor(kHeadset, 3));
    store.saveName(kSpeaker, "Creative");

    store.forget(kSpeaker);
    EXPECT_FALSE(store.hasKey(kLocal, kSpeaker));
    EXPECT_FALSE(store.hasKey(kOther, kSpeaker));
    EXPECT_EQ("", store.loadName(kSpeaker));
    EXPECT_TRUE(store.hasKey(kLocal, kHeadset));
}

TEST(FileBondStore, AMissingDirectoryIsNotAnError)
{
    FileBondStore store("/nonexistent-bluev-dir");
    EXPECT_FALSE(store.hasKey(kLocal, kSpeaker));
    EXPECT_FALSE(store.saveKey(kLocal, keyFor(kSpeaker, 1)));
    EXPECT_TRUE(store.list(kLocal).empty());
    store.forget(kSpeaker);
}
