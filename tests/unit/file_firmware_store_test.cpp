// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <fstream>
#include <vector>

#include "platform/posix/file_firmware_store.hpp"
#include "temp_dir.hpp"

using namespace bluev;
using namespace bluev::testing;

TEST(FileFirmwareStore, ReadsAFileFromTheDirectory)
{
    TempDir dir;
    const std::vector<std::uint8_t> bytes = {'R', 'e', 'a', 'l', 't', 'e', 'c', 'h', 1, 2, 3};
    std::ofstream(dir.file("rtl8761bu_fw.bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    FileFirmwareStore store(dir.path());
    std::vector<std::uint8_t> out;

    ASSERT_TRUE(store.read("rtl8761bu_fw.bin", out));
    EXPECT_EQ(bytes, out);
}

TEST(FileFirmwareStore, RefusesMissingEmptyAndPathLikeNames)
{
    TempDir dir;
    {
        std::ofstream(dir.file("empty.bin"), std::ios::binary);
    }
    FileFirmwareStore store(dir.path());
    std::vector<std::uint8_t> out = {1, 2, 3};

    EXPECT_FALSE(store.read("missing.bin", out));
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(store.read("empty.bin", out));
    EXPECT_FALSE(store.read("../rtl8761bu_fw.bin", out));
    EXPECT_FALSE(store.read("", out));
}
