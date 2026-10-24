// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "platform/posix/log_file.hpp"
#include "temp_dir.hpp"

using namespace bluev;
using bluev::testing::TempDir;

namespace
{

std::string contents(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;

    text << in.rdbuf();
    return text.str();
}

int filesIn(const TempDir& dir)
{
    int count = 0;

    for (const auto& entry : std::filesystem::directory_iterator(dir.path()))
    {
        (void)entry;
        count++;
    }
    return count;
}

} // namespace

TEST(LogFile, AppendsToWhatIsThere)
{
    TempDir dir;
    {
        LogFile file;
        ASSERT_TRUE(file.open(dir.path(), dir.file("a.log")));
        file.append("one\n");
    }
    LogFile again;
    ASSERT_TRUE(again.open(dir.path(), dir.file("a.log")));
    again.append("two\n");

    EXPECT_EQ("one\ntwo\n", contents(dir.file("a.log")));
}

TEST(LogFile, CreatesItsDirectory)
{
    TempDir dir;
    const std::string sub = dir.file("logs");
    LogFile file;

    ASSERT_TRUE(file.open(sub, sub + "/a.log"));
    file.append("x\n");
    EXPECT_EQ("x\n", contents(sub + "/a.log"));
}

TEST(LogFile, DoesNothingWhileClosed)
{
    LogFile file;
    file.append("nobody listens\n"); // must not crash
    TempDir dir;
    EXPECT_FALSE(file.open(dir.file("missing/deeper"), dir.file("missing/deeper/a.log")));
    file.append("still nobody\n");
}

TEST(LogFile, TurnsOverAtTheLimitAndKeepsOneBackup)
{
    TempDir dir;
    LogFile file(100);
    ASSERT_TRUE(file.open(dir.path(), dir.file("a.log")));

    file.append(std::string(60, 'a') + "\n"); // 61 bytes: below the limit
    EXPECT_FALSE(dir.exists("a.log.1"));
    file.append(std::string(60, 'b') + "\n"); // 122: reaches it, so it is turned over
    ASSERT_TRUE(dir.exists("a.log.1"));
    EXPECT_EQ(std::string(60, 'a') + "\n" + std::string(60, 'b') + "\n", contents(dir.file("a.log.1")));
    EXPECT_EQ("", contents(dir.file("a.log")));

    file.append("fresh\n");
    EXPECT_EQ("fresh\n", contents(dir.file("a.log")));
}

TEST(LogFile, OnlyTwoFilesEverExistAndTheOldestIsGone)
{
    TempDir dir;
    LogFile file(50);
    ASSERT_TRUE(file.open(dir.path(), dir.file("a.log")));

    file.append(std::string(60, '1') + "\n"); // first turn-over
    file.append(std::string(60, '2') + "\n"); // second: the 1s are replaced
    file.append("tail\n");

    EXPECT_EQ(2, filesIn(dir));
    EXPECT_EQ(std::string(60, '2') + "\n", contents(dir.file("a.log.1")));
    EXPECT_EQ("tail\n", contents(dir.file("a.log")));
}

TEST(LogFile, ABigFileIsTurnedOverWhenItIsOpened)
{
    TempDir dir;
    {
        std::ofstream out(dir.file("a.log"), std::ios::binary);
        out << std::string(200, 'o');
    }
    LogFile file(100);
    ASSERT_TRUE(file.open(dir.path(), dir.file("a.log")));
    file.append("new\n");

    EXPECT_EQ(std::string(200, 'o'), contents(dir.file("a.log.1")));
    EXPECT_EQ("new\n", contents(dir.file("a.log")));
}

TEST(LogFile, ASmallFileIsKeptWhenItIsOpened)
{
    TempDir dir;
    {
        std::ofstream out(dir.file("a.log"), std::ios::binary);
        out << "old\n";
    }
    LogFile file(100);
    ASSERT_TRUE(file.open(dir.path(), dir.file("a.log")));
    file.append("new\n");

    EXPECT_FALSE(dir.exists("a.log.1"));
    EXPECT_EQ("old\nnew\n", contents(dir.file("a.log")));
}

TEST(LogFile, TheSizeIsBoundedWhenTheMoveFails)
{
    TempDir dir;
    // a directory where the backup should go: the rename cannot succeed
    std::filesystem::create_directory(dir.file("a.log.1"));
    LogFile file(50);
    ASSERT_TRUE(file.open(dir.path(), dir.file("a.log")));

    file.append(std::string(60, 'x') + "\n"); // reaches the limit; the move fails, the file is emptied
    file.append("after\n");

    EXPECT_EQ("after\n", contents(dir.file("a.log")));
}

TEST(LogFile, ManyThreadsNeverSplitALine)
{
    TempDir dir;
    LogFile file(4000);
    ASSERT_TRUE(file.open(dir.path(), dir.file("a.log")));
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; t++)
    {
        threads.emplace_back([&file, t] {
            for (int i = 0; i < 200; i++)
            {
                file.append("thread " + std::to_string(t) + " line " + std::to_string(i) + "\n");
            }
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    // every line in what is left is whole
    for (const char* name : {"a.log", "a.log.1"})
    {
        std::istringstream in(contents(dir.file(name)));
        std::string line;

        while (std::getline(in, line))
        {
            EXPECT_EQ(0u, line.find("thread ")) << line;
            EXPECT_NE(std::string::npos, line.find(" line ")) << line;
        }
    }
    EXPECT_LE(contents(dir.file("a.log")).size(), 4000u);
}
