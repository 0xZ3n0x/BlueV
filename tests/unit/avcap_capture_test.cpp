// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include "capture_sink.hpp"
#include "core/audio/avcap_capture.hpp"
#include "fake_avcap.hpp"

using namespace bluev;
using namespace bluev::testing;

namespace
{
using Calls = std::vector<std::string>;

constexpr int kNothingNew = static_cast<int>(0x81950002u);
constexpr int kOverrun = static_cast<int>(0x81950004u);
} // namespace

TEST(AvcapCapture, StartsInTheOrderOfTheLibrary)
{
    CaptureSink sink;
    FakeAvcap library;
    AvcapCapture capture(library);

    EXPECT_TRUE(capture.start());
    EXPECT_EQ((Calls{"initialize", "open", "start"}), library.calls);
    EXPECT_TRUE(sink.has("capture: running, 48000 Hz stereo"));
}

TEST(AvcapCapture, ReadingNeedsAStartedCapture)
{
    FakeAvcap library;
    AvcapCapture capture(library);
    float buffer[8];

    EXPECT_EQ(-1, capture.read(buffer, sizeof buffer));
    EXPECT_EQ(0, library.count("initialize") + library.count("start"));
}

// ---- a step that fails undoes the ones before it ----

TEST(AvcapCapture, WhenInitializeFailsNothingIsTornDown)
{
    CaptureSink sink;
    FakeAvcap library;
    AvcapCapture capture(library);

    library.initializeResult = static_cast<int>(0x80010001u);
    EXPECT_FALSE(capture.start());
    EXPECT_EQ((Calls{"initialize"}), library.calls);
    EXPECT_TRUE(sink.has("capture: Initialize -> 0x80010001, not available yet"));
}

TEST(AvcapCapture, WhenOpenFailsTheLibraryIsTerminated)
{
    FakeAvcap library;
    AvcapCapture capture(library);

    library.openResult = -5;
    EXPECT_FALSE(capture.start());
    EXPECT_EQ((Calls{"initialize", "open", "terminate"}), library.calls);
}

TEST(AvcapCapture, AnOpenWithoutAHandleIsAFailureToo)
{
    FakeAvcap library;
    AvcapCapture capture(library);

    library.openGivesNoHandle = true;
    EXPECT_FALSE(capture.start());
    EXPECT_EQ((Calls{"initialize", "open", "terminate"}), library.calls); // nothing to close
}

TEST(AvcapCapture, WhenStartFailsTheHandleIsClosedAndTheLibraryTerminatedButNothingStopped)
{
    FakeAvcap library;
    AvcapCapture capture(library);

    library.startResult = -7;
    EXPECT_FALSE(capture.start());
    EXPECT_EQ((Calls{"initialize", "open", "start", "close", "terminate"}), library.calls);
}

TEST(AvcapCapture, TheSameErrorIsLoggedOnceAndAnotherOneAgain)
{
    CaptureSink sink;
    FakeAvcap library;
    AvcapCapture capture(library);

    library.initializeResult = -3;
    for (int i = 0; i < 4; i++)
    {
        EXPECT_FALSE(capture.restart());
    }
    int lines = 0;
    for (const std::string& line : sink.lines())
    {
        lines += line.find("Initialize -> 0xfffffffd") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(1, lines);

    library.initializeResult = -4;
    EXPECT_FALSE(capture.restart());
    EXPECT_TRUE(sink.has("Initialize -> 0xfffffffc"));
}

TEST(AvcapCapture, AfterASuccessTheSameErrorIsLoggedAgain)
{
    CaptureSink sink;
    FakeAvcap library;
    AvcapCapture capture(library);

    library.initializeResult = -3;
    EXPECT_FALSE(capture.start());
    library.initializeResult = 0;
    EXPECT_TRUE(capture.restart());
    library.initializeResult = -3;
    EXPECT_FALSE(capture.restart());

    int lines = 0;
    for (const std::string& line : sink.lines())
    {
        lines += line.find("Initialize -> 0xfffffffd") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(2, lines);
}

// ---- reading ----

TEST(AvcapCapture, ARecordComesBackWithItsLengthAndTheBufferItWasGiven)
{
    FakeAvcap library;
    AvcapCapture capture(library);
    float buffer[64] = {};

    ASSERT_TRUE(capture.start());
    library.readResult = 512;
    EXPECT_EQ(512, capture.read(buffer, sizeof buffer));
    EXPECT_EQ(buffer, library.lastReadBuffer);
    EXPECT_EQ(sizeof buffer, library.lastReadMax);
    EXPECT_EQ(&library.handleToken, library.lastReadHandle);
    EXPECT_EQ(0.5f, buffer[0]);
}

TEST(AvcapCapture, NothingNewYetIsZero)
{
    FakeAvcap library;
    AvcapCapture capture(library);
    float buffer[8];

    ASSERT_TRUE(capture.start());
    library.readResult = kNothingNew;
    EXPECT_EQ(0, capture.read(buffer, sizeof buffer));
    EXPECT_EQ(0, capture.overruns());
}

TEST(AvcapCapture, ARingOverrunIsZeroCountedAndTheFirstFiveAreLogged)
{
    CaptureSink sink;
    FakeAvcap library;
    AvcapCapture capture(library);
    float buffer[8];

    ASSERT_TRUE(capture.start());
    library.readResult = kOverrun;
    for (int i = 0; i < 8; i++)
    {
        EXPECT_EQ(0, capture.read(buffer, sizeof buffer));
    }
    EXPECT_EQ(8, capture.overruns());
    int lines = 0;
    for (const std::string& line : sink.lines())
    {
        lines += line.find("capture: ring overrun") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(5, lines);
}

TEST(AvcapCapture, AnyOtherErrorIsMinusOneAndLogged)
{
    CaptureSink sink;
    FakeAvcap library;
    AvcapCapture capture(library);
    float buffer[8];

    ASSERT_TRUE(capture.start());
    library.readResult = static_cast<int>(0x81950001u);
    EXPECT_EQ(-1, capture.read(buffer, sizeof buffer));
    EXPECT_TRUE(sink.has("capture: ReadAudio -> 0x81950001"));
    EXPECT_EQ(0, capture.overruns());
}

TEST(AvcapCapture, TheAnswersAreTakenInTurn)
{
    FakeAvcap library;
    AvcapCapture capture(library);
    float buffer[8];

    ASSERT_TRUE(capture.start());
    library.reads = {64, kNothingNew, 128};
    EXPECT_EQ(64, capture.read(buffer, sizeof buffer));
    EXPECT_EQ(0, capture.read(buffer, sizeof buffer));
    EXPECT_EQ(128, capture.read(buffer, sizeof buffer));
}

// ---- restarting and closing ----

TEST(AvcapCapture, RestartTakesTheCaptureDownAndStartsItAgain)
{
    FakeAvcap library;
    AvcapCapture capture(library);

    ASSERT_TRUE(capture.start());
    library.calls.clear();
    EXPECT_TRUE(capture.restart());
    EXPECT_EQ((Calls{"stop", "close", "terminate", "initialize", "open", "start"}), library.calls);
}

TEST(AvcapCapture, ARestartThatFailsLeavesItStoppedAndReadingGivesAnError)
{
    FakeAvcap library;
    AvcapCapture capture(library);
    float buffer[8];

    ASSERT_TRUE(capture.start());
    library.openResult = -9;
    EXPECT_FALSE(capture.restart());
    EXPECT_EQ(-1, capture.read(buffer, sizeof buffer));
    library.openResult = 0;
    EXPECT_TRUE(capture.restart()); // and it can come back
    EXPECT_EQ(16, capture.read(buffer, sizeof buffer));
}

TEST(AvcapCapture, CloseStopsClosesAndTerminatesOnceAndIsSafeToRepeat)
{
    FakeAvcap library;
    AvcapCapture capture(library);

    ASSERT_TRUE(capture.start());
    library.calls.clear();
    capture.close();
    capture.close();
    EXPECT_EQ((Calls{"stop", "close", "terminate"}), library.calls);
}

TEST(AvcapCapture, ClosingACaptureThatNeverStartedDoesNothing)
{
    FakeAvcap library;
    AvcapCapture capture(library);

    capture.close();
    EXPECT_TRUE(library.calls.empty());
}

TEST(AvcapCapture, ADestroyedCaptureIsTakenDown)
{
    FakeAvcap library;
    {
        AvcapCapture capture(library);

        ASSERT_TRUE(capture.start());
    }
    EXPECT_EQ((Calls{"initialize", "open", "start", "stop", "close", "terminate"}), library.calls);
}
