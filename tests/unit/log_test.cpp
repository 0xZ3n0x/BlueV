// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include "capture_sink.hpp"
#include "core/ports/log.hpp"

using namespace bluev;
using bluev::testing::CaptureSink;

TEST(Log, FormatsAndForwardsToTheSink)
{
    CaptureSink sink;
    logLine("hello %s %d", "world", 7);
    notify("note %d", 1);
    notifyShow("shown %d", 2);
    EXPECT_TRUE(sink.has("hello world 7"));
    ASSERT_EQ(2u, sink.notes().size());
    EXPECT_EQ("note 1", sink.notes()[0]);
    EXPECT_TRUE(sink.has("shown 2")); // notifyShow also logs
}

TEST(Log, DropsEverythingWithoutASink)
{
    EXPECT_EQ(nullptr, logSink());
    logLine("nobody listens"); // must not crash
    notify("nor here");
}

TEST(Log, LongLinesAreCut)
{
    CaptureSink sink;
    std::string big(2000, 'x');
    logLine("%s", big.c_str());
    ASSERT_EQ(1u, sink.lines().size());
    EXPECT_LT(sink.lines()[0].size(), 512u);
}
