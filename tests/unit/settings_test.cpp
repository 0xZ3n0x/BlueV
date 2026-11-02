// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <fstream>

#include "service/settings.hpp"
#include "temp_dir.hpp"

using namespace bluev;
using namespace bluev::testing;

TEST(SettingsFile, ASavedFileComesBack)
{
    TempDir dir;
    const SettingsFile file(dir.file("settings"));
    Settings saved;

    saved.autostart = true;
    saved.vid = 0x0a12;
    saved.pid = 0x0001;
    saved.quality = 1;
    ASSERT_TRUE(file.save(saved));

    const Settings loaded = file.load();
    EXPECT_TRUE(loaded.autostart);
    EXPECT_EQ(0x0a12, loaded.vid);
    EXPECT_EQ(0x0001, loaded.pid);
    EXPECT_EQ(1, loaded.quality);
    EXPECT_FALSE(dir.exists("settings.tmp")); // renamed over the file
}

TEST(SettingsFile, AMissingFileGivesTheDefaults)
{
    TempDir dir;
    const Settings loaded = SettingsFile(dir.file("nothing")).load();

    EXPECT_FALSE(loaded.autostart);
    EXPECT_EQ(0, loaded.vid);
    EXPECT_EQ(0, loaded.pid);
    EXPECT_EQ(0, loaded.quality);
}

TEST(SettingsFile, TheNetworkChoiceComesBack)
{
    TempDir dir;
    const SettingsFile file(dir.file("settings"));
    Settings saved;

    EXPECT_FALSE(file.load().localOnly); // the default: anyone on the network
    saved.localOnly = true;
    ASSERT_TRUE(file.save(saved));
    EXPECT_TRUE(file.load().localOnly);
    saved.localOnly = false;
    ASSERT_TRUE(file.save(saved));
    EXPECT_FALSE(file.load().localOnly);
}

TEST(SettingsFile, AnUnknownQualityIsNormal)
{
    TempDir dir;
    {
        std::ofstream out(dir.file("settings"));
        out << "quality=9\n";
    }
    EXPECT_EQ(0, SettingsFile(dir.file("settings")).load().quality);
}

TEST(SettingsFile, UnknownAndDamagedLinesAreIgnored)
{
    TempDir dir;
    {
        std::ofstream out(dir.file("settings"));
        out << "something=else\nautostart=1\nadapter=garbage\n\x01\x02 not a line\nadapter=2357:0604\n";
    }
    const Settings loaded = SettingsFile(dir.file("settings")).load();

    EXPECT_TRUE(loaded.autostart);
    EXPECT_EQ(0x2357, loaded.vid);
    EXPECT_EQ(0x0604, loaded.pid);
}

TEST(SettingsFile, SavingIntoAMissingDirectoryFails)
{
    TempDir dir;

    EXPECT_FALSE(SettingsFile(dir.file("no/such/dir/settings")).save(Settings{}));
}
