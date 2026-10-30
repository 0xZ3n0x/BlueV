// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>

#include "bt_fixture.hpp"
#include "core/audio/a2dp_session.hpp"
#include "core/audio/a2dp_streamer.hpp"
#include "fake_audio.hpp"
#include "fake_speaker.hpp"

using namespace bluev;
using namespace bluev::testing;

namespace
{

class Stream : public BtFixture
{
  protected:
    Stream()
        : speaker(controller, clock), session(stack, clock), audio(clock),
          streamer(stack, session, audio, clock, observer.hook())
    {
    }

    // connected and negotiated, ready to stream
    void ready(std::uint32_t caps = 0x3502ffff)
    {
        bonds.put(controller.localAddress, remoteAddress(), 0x5a, "Creative");
        startExternal();
        speaker.attach();
        speaker.sbcCaps = caps;
        ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();
        ASSERT_TRUE(session.start(true)) << sink.all();
        controller.aclOut.clear();
        speaker.media.clear();
    }
    // the speaker hangs up after `ms`
    void endAfter(std::uint32_t ms)
    {
        controller.remoteDisconnect(0x13, ms);
    }

    FakeSpeaker speaker;
    A2dpSession session;
    FakeAudio audio;
    RecordingObserver observer;
    A2dpStreamer streamer;
};

} // namespace

TEST_F(Stream, SendsPacedSbcPacketsAt48kHz)
{
    ready();
    endAfter(4000);
    const auto t0 = clock.nowMs();
    ASSERT_TRUE(streamer.run(true)) << sink.all();

    ASSERT_GT(speaker.media.size(), 100u);
    // seven frames of 128 samples per packet: 53.6 packets/s for 4 s
    EXPECT_NEAR(215, static_cast<int>(speaker.media.size()), 12);
    EXPECT_EQ(7, speaker.media[0].sbcFrames);
    EXPECT_EQ(0u, speaker.media[0].timestamp);
    EXPECT_EQ(1, speaker.media[0].sequence);
    for (std::size_t i = 1; i < speaker.media.size(); i++)
    {
        ASSERT_EQ(speaker.media[i - 1].sequence + 1, speaker.media[i].sequence) << i;
        ASSERT_EQ(speaker.media[i - 1].timestamp + 896, speaker.media[i].timestamp) << i;
    }
    // each frame is 93 bytes (bitpool 40, joint stereo, 16 blocks, 8 subbands)
    EXPECT_EQ(7 * 93, speaker.media[0].payloadBytes);
    EXPECT_GE(clock.nowMs() - t0, 4000u);
    EXPECT_TRUE(sink.has("sbc: 93-byte frames, 7 per packet (18 ms)"));
    EXPECT_TRUE(sink.has("stream: ended after 4 s,"));
}

TEST_F(Stream, OneNotificationNamesTheDeviceAndTheLogHasTheFormat)
{
    ready();
    endAfter(500);
    streamer.run(true);
    EXPECT_EQ((std::vector<std::string>{"Streaming to Creative"}), sink.notes());
    EXPECT_TRUE(sink.has("stream: to Creative, 48000 Hz, bitpool 40"));
}

TEST_F(Stream, ResamplesFor44100Speakers)
{
    ready(0x350215 << 8 | 0x22);
    endAfter(4000);
    ASSERT_TRUE(streamer.run(true)) << sink.all();

    // 44100 Hz: 896 frames per packet -> about 49 packets/s
    EXPECT_NEAR(197, static_cast<int>(speaker.media.size()), 12);
    EXPECT_EQ(44100, observer.rate);
    EXPECT_TRUE(sink.has("stream: to Creative, 44100 Hz, bitpool 40"));
}

TEST_F(Stream, WithoutAudioItStreamsSilenceAndKeepsTryingToStartTheCapture)
{
    ready();
    audio.silent = true;
    audio.restartWorks = false;
    endAfter(3000);
    ASSERT_TRUE(streamer.run(false)) << sink.all();

    EXPECT_NEAR(161, static_cast<int>(speaker.media.size()), 12); // silence keeps the packets coming
    EXPECT_GE(audio.restarts, 5);                                 // every 500 ms
    EXPECT_LE(audio.restarts, 7);
    EXPECT_EQ(0, audio.reads); // never reads before it works
}

TEST_F(Stream, CaptureStartsWhenTheAudioAppears)
{
    ready();
    audio.silent = true;
    endAfter(3000);
    audio.restartWorks = true;
    ASSERT_TRUE(streamer.run(false)) << sink.all();
    EXPECT_EQ(1, audio.restarts);
    EXPECT_GT(audio.reads, 100);
    EXPECT_GT(audio.framesDelivered, 100000);
}

TEST_F(Stream, ACaptureErrorMakesItWaitForTheAudioAgain)
{
    ready();
    int reads = 0;
    audio.onRead = [&] {
        if (++reads == 200)
        {
            audio.failNext = true;
        }
    };
    endAfter(3000);
    ASSERT_TRUE(streamer.run(true)) << sink.all();
    EXPECT_GE(audio.restarts, 1);          // started again after the error
    EXPECT_GT(speaker.media.size(), 150u); // and the packets never stopped
}

TEST_F(Stream, AStopRequestEndsTheStream)
{
    ready();
    audio.onRead = [&] {
        if (clock.nowMs() > 100000 + 2000)
        {
            session.requestStop();
        }
    };
    clock.set(100000);
    ASSERT_TRUE(streamer.run(true));
    EXPECT_TRUE(sink.has("stream: ended after 2 s") || sink.has("stream: ended after 3 s"));
    EXPECT_FALSE(stack.linkLost());
}

TEST_F(Stream, TheSpeakerClosingTheMediaChannelEndsTheStream)
{
    ready();
    speaker.closeMedia(1500);
    ASSERT_TRUE(streamer.run(true));
    EXPECT_TRUE(session.media().closed);
    EXPECT_TRUE(sink.has("stream: ended after 1 s"));
}

TEST_F(Stream, ALostLinkEndsTheStream)
{
    ready();
    endAfter(1000);
    streamer.run(true);
    EXPECT_TRUE(stack.linkLost());
    EXPECT_TRUE(sink.has("stream: ended after 1 s"));
}

TEST_F(Stream, ASendFailureEndsItAndIsReported)
{
    ready();
    audio.onRead = [&] {
        if (clock.nowMs() > 100000 + 1000)
        {
            controller.aclSendOk = false;
        }
    };
    clock.set(100000);
    EXPECT_TRUE(streamer.run(true)); // packets went out before it failed
    EXPECT_TRUE(sink.has("media: send failed after"));
    EXPECT_TRUE(sink.has("stream: ended after"));
}

TEST_F(Stream, TheObserverGetsNumbersAFewTimesASecond)
{
    ready();
    endAfter(5000);
    streamer.run(true);
    EXPECT_NEAR(20, observer.count, 4); // every 250 ms
    EXPECT_NEAR(54, observer.pps, 8);
    EXPECT_LT(observer.queueMs, 100);
    EXPECT_GT(observer.packets, 250);
}

TEST_F(Stream, WorksWithoutAnObserver)
{
    ready();
    A2dpStreamer quiet(stack, session, audio, clock, nullptr);
    endAfter(1000);
    EXPECT_TRUE(quiet.run(true));
}

TEST_F(Stream, AStatusLineEveryTenSeconds)
{
    ready();
    endAfter(25000);
    streamer.run(true);
    EXPECT_EQ(2, sink.count("capture records, queue"));
    EXPECT_TRUE(sink.has("overruns 7"));
    EXPECT_TRUE(sink.has("capturing"));
    EXPECT_TRUE(sink.has("credits "));
}

TEST_F(Stream, NoCompletionReportsAreHandledByAssumingThem)
{
    ready();
    speaker.reportsCompletions = false;
    endAfter(4000);
    ASSERT_TRUE(streamer.run(true)) << sink.all();
    EXPECT_GT(stack.completionsAssumed(), 100);
    EXPECT_NEAR(215, static_cast<int>(speaker.media.size()), 25);
}

TEST_F(Stream, TheQueueIsTrimmedWhenTheAudioArrivesInBursts)
{
    ready();
    audio.minIntervalMs = 200; // 9600 frames at once
    endAfter(3000);
    streamer.run(true);
    EXPECT_FALSE(sink.has("trimmed 0"));
    EXPECT_TRUE(sink.has("stream: ended after 3 s"));
}

TEST_F(Stream, AFrameThatDoesNotFitIsRefused)
{
    ready();
    session.media().theirMtu = 60; // 60 - 13 < 93
    EXPECT_FALSE(streamer.run(true));
    EXPECT_TRUE(sink.has("sbc: a 93-byte frame does not fit a packet"));
    EXPECT_TRUE(speaker.media.empty());
}

TEST_F(Stream, HighQualityStreamsBiggerFramesThatStillFitThePackets)
{
    session.setQuality(kQualityHigh);
    ready();
    endAfter(1000);
    ASSERT_TRUE(streamer.run(true)) << sink.all();

    EXPECT_TRUE(sink.has("stream: to Creative, 48000 Hz, bitpool 53"));
    EXPECT_TRUE(sink.has("sbc: 119-byte frames"));
    ASSERT_FALSE(speaker.media.empty());
    for (const auto& packet : speaker.media)
    {
        EXPECT_LE(packet.payloadBytes, static_cast<int>(session.media().theirMtu) - 13);
    }
}

TEST_F(Stream, ThePacketSizeFollowsTheMtu)
{
    ready();
    session.media().theirMtu = 300; // (300-13)/93 = 3 frames
    endAfter(1000);
    streamer.run(true);
    ASSERT_FALSE(speaker.media.empty());
    EXPECT_EQ(3, speaker.media[0].sbcFrames);
}
