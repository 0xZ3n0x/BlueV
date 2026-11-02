// SPDX-License-Identifier: GPL-3.0-or-later
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "mock_radio.hpp"
#include "service/app_log.hpp"
#include "service/home_screen_shortcut.hpp"
#include "temp_dir.hpp"

using namespace bluev;
using namespace bluev::testing;
using ::testing::_;
using ::testing::Return;

// ---- LogBuffer ----
TEST(LogBuffer, SinceReturnsLinesFromANumberOn)
{
    LogBuffer b;
    b.add("one");
    b.add("two");
    b.add("three");

    unsigned next = 0;
    EXPECT_EQ("one\ntwo\nthree\n", b.since(0, next, 1000));
    EXPECT_EQ(3u, next);
    EXPECT_EQ("three\n", b.since(2, next, 1000));
    EXPECT_EQ("", b.since(3, next, 1000));
    EXPECT_EQ(3u, next);
}

TEST(LogBuffer, ANumberAheadOfTheLogStartsOver)
{
    // the page asked with a number from an earlier run of the program
    LogBuffer b;
    b.add("one");
    b.add("two");

    unsigned next = 0;
    EXPECT_EQ("one\ntwo\n", b.since(500, next, 1000));
    EXPECT_EQ(2u, next);
}

TEST(LogBuffer, ByteLimitStopsBetweenLines)
{
    LogBuffer b;
    for (int i = 0; i < 10; i++)
    {
        b.add("0123456789");
    }
    unsigned next = 0;
    const std::string part = b.since(0, next, 25);
    EXPECT_EQ(22u, part.size()); // two lines of 11
    EXPECT_EQ(2u, next);
    EXPECT_EQ(22u, b.since(next, next, 25).size());
    EXPECT_EQ(4u, next);
}

TEST(LogBuffer, TheRingKeepsTheLastThreeHundredLines)
{
    LogBuffer b;
    for (int i = 0; i < 500; i++)
    {
        b.add("line " + std::to_string(i));
    }
    unsigned next = 0;
    const std::string all = b.since(0, next, 100000);
    EXPECT_EQ(500u, next);
    EXPECT_EQ(0u, all.find("line 200\n")); // the oldest still there
    EXPECT_NE(std::string::npos, all.find("line 499\n"));
}

TEST(LogBuffer, LongLinesAreCutForTheRingButKeptForTheFile)
{
    LogBuffer b;
    b.add(std::string(300, 'x'));
    unsigned next = 0;
    EXPECT_EQ(LogBuffer::kLineLength, b.since(0, next, 1000).size()); // 199 characters and the newline
    EXPECT_EQ(301u, b.takePending().size());
}

TEST(LogBuffer, PendingIsTakenOnce)
{
    LogBuffer b;
    b.add("a");
    b.add("b");
    EXPECT_EQ("a\nb\n", b.takePending());
    EXPECT_EQ("", b.takePending());
}

TEST(LogBuffer, PendingStopsGrowingAtTheLimit)
{
    LogBuffer b;
    const std::string line(1000, 'y');
    for (int i = 0; i < 200; i++)
    {
        b.add(line);
    }
    EXPECT_LT(b.takePending().size(), LogBuffer::kPendingLimit + 2000);
}

TEST(LogBuffer, ManyThreadsAtOnce)
{
    LogBuffer b;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
    {
        threads.emplace_back([&] {
            for (int i = 0; i < 500; i++)
            {
                b.add("x");
                unsigned next;
                b.since(0, next, 100);
            }
        });
    }
    for (auto& t : threads)
    {
        t.join();
    }
    unsigned next = 0;
    EXPECT_EQ("x\n", b.since(1999, next, 10)); // 2000 lines in all
    EXPECT_EQ(2000u, next);
}

// ---- AppLog ----
namespace
{
struct Collected
{
    std::mutex mutex;
    std::vector<std::string> texts;
    void add(const std::string& t)
    {
        std::lock_guard<std::mutex> l(mutex);
        texts.push_back(t);
    }
    std::vector<std::string> get()
    {
        std::lock_guard<std::mutex> l(mutex);
        return texts;
    }
};
} // namespace

TEST(AppLog, StampsAndFansOut)
{
    LogBuffer buffer;
    Collected notes, file;
    AppLog log(
        buffer, [&](const std::string& t) { file.add(t); }, [&](const std::string& t) { notes.add(t); },
        [] { return static_cast<std::time_t>(3600 * 5 + 60 * 7 + 9); });

    log.line("hello");
    log.notify("on screen");
    log.flush();
    log.flush(); // nothing new: no second write

    EXPECT_EQ((std::vector<std::string>{"on screen"}), notes.get());
    EXPECT_EQ((std::vector<std::string>{"05:07:09 hello\n"}), file.get());
    unsigned next = 0;
    EXPECT_EQ("05:07:09 hello\n", buffer.since(0, next, 1000));
}

TEST(AppLog, WorksWithoutAFile)
{
    LogBuffer buffer;
    AppLog log(buffer, nullptr, nullptr);
    log.line("x");
    log.flush();
    unsigned next = 0;
    EXPECT_NE(std::string::npos, buffer.since(0, next, 100).find("x\n"));
}

TEST(AppLog, ThroughTheGlobalFunctions)
{
    LogBuffer buffer;
    Collected notes;
    AppLog log(buffer, nullptr, [&](const std::string& t) { notes.add(t); });
    setLogSink(&log);
    logLine("value %d", 42);
    notify("note %s", "n");
    flushLog();
    setLogSink(nullptr);
    unsigned next = 0;
    EXPECT_NE(std::string::npos, buffer.since(0, next, 100).find("value 42\n"));
    EXPECT_EQ((std::vector<std::string>{"note n"}), notes.get());
}

// ---- HomeScreenShortcut ----
namespace
{
const std::string kIcon = "\x89PNG-fake-icon-bytes";
}

TEST(HomeScreenShortcut, ParamJsonPointsAtOurPort)
{
    const std::string json = HomeScreenShortcut::paramJson(7733);
    EXPECT_NE(std::string::npos, json.find("\"titleId\": \"BLUV00001\""));
    EXPECT_NE(std::string::npos, json.find("\"deeplinkUri\": \"http://127.0.0.1:7733/\""));
    EXPECT_NE(std::string::npos, json.find("\"titleName\": \"BlueV\""));
}

TEST(HomeScreenShortcut, InstallWritesTheFilesAndAsksTheInstallerOnce)
{
    TempDir dir;
    MockInstaller installer;
    HomeScreenShortcut shortcut(installer, kIcon, dir.path());

    EXPECT_CALL(installer, registerTitle("BLUV00001", dir.path() + "/")).Times(1).WillOnce(Return(true));
    EXPECT_TRUE(shortcut.install(7733));
    EXPECT_TRUE(dir.exists("BLUV00001/sce_sys/param.json"));
    EXPECT_TRUE(dir.exists("BLUV00001/sce_sys/icon0.png"));
    EXPECT_TRUE(dir.exists("BLUV00001/.registered"));
    EXPECT_TRUE(shortcut.install(7733)); // as it should be: nothing happens
}

TEST(HomeScreenShortcut, AChangedPortRegistersAgain)
{
    TempDir dir;
    MockInstaller installer;
    HomeScreenShortcut shortcut(installer, kIcon, dir.path());

    EXPECT_CALL(installer, registerTitle(_, _)).Times(2).WillRepeatedly(Return(true));
    EXPECT_TRUE(shortcut.install(7733));
    EXPECT_TRUE(shortcut.install(7734));
}

TEST(HomeScreenShortcut, AFailedInstallerIsTriedAgainNextTime)
{
    TempDir dir;
    MockInstaller installer;
    HomeScreenShortcut shortcut(installer, kIcon, dir.path());

    EXPECT_CALL(installer, registerTitle(_, _)).WillOnce(Return(false)).WillOnce(Return(true));
    EXPECT_FALSE(shortcut.install(7733));
    EXPECT_FALSE(dir.exists("BLUV00001/.registered"));
    EXPECT_TRUE(shortcut.install(7733));
    EXPECT_TRUE(dir.exists("BLUV00001/.registered"));
}

TEST(HomeScreenShortcut, ARemovedMarkerMeansRegisterAgain)
{
    TempDir dir;
    MockInstaller installer;
    HomeScreenShortcut shortcut(installer, kIcon, dir.path());

    EXPECT_CALL(installer, registerTitle(_, _)).Times(2).WillRepeatedly(Return(true));
    shortcut.install(7733);
    std::remove(dir.file("BLUV00001/.registered").c_str());
    shortcut.install(7733);
}

TEST(HomeScreenShortcut, RemoveUnregistersAndDeletesTheFiles)
{
    TempDir dir;
    MockInstaller installer;
    HomeScreenShortcut shortcut(installer, kIcon, dir.path());

    EXPECT_CALL(installer, registerTitle(_, _)).WillOnce(Return(true));
    shortcut.install(7733);
    EXPECT_CALL(installer, unregisterTitle("BLUV00001")).WillOnce(Return(true));
    EXPECT_TRUE(shortcut.remove());
    EXPECT_FALSE(dir.exists("BLUV00001"));
}

TEST(HomeScreenShortcut, RemoveReportsAFailureButStillCleansUp)
{
    TempDir dir;
    MockInstaller installer;
    HomeScreenShortcut shortcut(installer, kIcon, dir.path());

    EXPECT_CALL(installer, registerTitle(_, _)).WillOnce(Return(true));
    shortcut.install(7733);
    EXPECT_CALL(installer, unregisterTitle(_)).WillOnce(Return(false));
    EXPECT_FALSE(shortcut.remove());
    EXPECT_FALSE(dir.exists("BLUV00001"));
}

TEST(HomeScreenShortcut, AnUnwritableRootFailsWithoutAskingTheInstaller)
{
    MockInstaller installer;
    HomeScreenShortcut shortcut(installer, kIcon, "/proc/nonexistent-root");
    EXPECT_CALL(installer, registerTitle(_, _)).Times(0);
    EXPECT_FALSE(shortcut.install(7733));
}
