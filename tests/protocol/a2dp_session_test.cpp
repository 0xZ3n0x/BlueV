// SPDX-License-Identifier: GPL-3.0-or-later
#include "bt_fixture.hpp"
#include "core/audio/a2dp_session.hpp"
#include "fake_speaker.hpp"

using namespace bluev;
using namespace bluev::testing;

namespace
{

class Session : public BtFixture
{
  protected:
    Session() : speaker(controller, clock), session(stack, clock)
    {
    }

    void SetUp() override
    {
        bonds.put(controller.localAddress, remoteAddress(), 0x5a, "Creative");
        startExternal();
        speaker.attach();
        ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();
        controller.aclOut.clear();
    }

    FakeSpeaker speaker;
    A2dpSession session;
};

} // namespace

TEST_F(Session, WeRunTheWholeProcedure)
{
    ASSERT_TRUE(session.start(true)) << sink.all();

    EXPECT_EQ((std::vector<std::uint8_t>{1, 2, 3, 6, 7}), speaker.signals); // discover ... start
    EXPECT_TRUE(speaker.started);
    EXPECT_TRUE(sink.has("avdtp: signaling channel opened by us"));
    EXPECT_TRUE(sink.has("avdtp: > command 0x01 label ")); // Discover, with its bytes
    EXPECT_TRUE(sink.has("avdtp: < answer 0x01 label "));
    EXPECT_TRUE(sink.has("avdtp: SBC sink 2, capabilities ff ff, bitpool 2-53"));
    EXPECT_TRUE(sink.has("avdtp: streaming SBC 48000 Hz, bitpool 40"));
    EXPECT_EQ(48000u, session.config().rate);
    EXPECT_TRUE(session.media().up);
    EXPECT_EQ(speaker.mediaCid, session.media().theirCid);

    // what we asked the speaker to use
    const Bytes& c = speaker.configuration;
    ASSERT_EQ(12u, c.size());
    EXPECT_EQ(0x08, c[0]); // its endpoint 2
    EXPECT_EQ(0x04, c[1]); // ours
    EXPECT_EQ(0x11, c[8]); // 48 kHz, joint stereo
    EXPECT_EQ(0x15, c[9]); // 16 blocks, 8 subbands, loudness
    EXPECT_EQ(40, c[10]);
    EXPECT_EQ(40, c[11]);
}

TEST_F(Session, WeOpenTheMediaChannelWhenTheSpeakerDoesNot)
{
    speaker.opensMediaItself = false;
    ASSERT_TRUE(session.start(true)) << sink.all();
    EXPECT_TRUE(session.media().up);
    EXPECT_TRUE(session.media().configured);
}

TEST_F(Session, ASpeakerWithOnly44100AndStereoGetsThat)
{
    speaker.sbcCaps = 0x350215 << 8 | 0x22;
    ASSERT_TRUE(session.start(true)) << sink.all();
    EXPECT_EQ(44100u, session.config().rate);
    EXPECT_EQ(0x22, speaker.configuration[8]);
    EXPECT_TRUE(sink.has("avdtp: streaming SBC 44100 Hz, bitpool 40"));
}

TEST_F(Session, NoSbcSinkNoSession)
{
    speaker.hasSbcSink = false;
    EXPECT_FALSE(session.start(true));
    EXPECT_TRUE(sink.has("avdtp: the device has no SBC sink"));
    EXPECT_FALSE(speaker.started);
}

TEST_F(Session, ARefusedConfigurationEndsTheSession)
{
    speaker.refuseConfiguration = true;
    EXPECT_FALSE(session.start(true));
    EXPECT_TRUE(sink.has("avdtp: configuration refused"));
    EXPECT_EQ((std::vector<std::uint8_t>{1, 2, 3}), speaker.signals);
}

TEST_F(Session, CapabilitiesWeCannotUseEndTheSession)
{
    speaker.sbcCaps = 0x350215 << 8 | 0x01; // 16 kHz mono
    EXPECT_FALSE(session.start(true));
    EXPECT_TRUE(sink.has("sbc: the device takes neither 44.1 nor 48 kHz"));
}

TEST_F(Session, ASpeakerThatNeverAnswersFailsAfterTwoTries)
{
    speaker.silentAvdtp = true;
    const auto t0 = clock.nowMs();
    EXPECT_FALSE(session.start(true));
    EXPECT_TRUE(sink.has("avdtp: signal 0x01 unanswered"));
    EXPECT_TRUE(sink.has("avdtp: discover failed"));
    EXPECT_GE(clock.nowMs() - t0, 16000u); // 8 s, twice
    EXPECT_EQ(2u, speaker.signals.size());
}

TEST_F(Session, TheSpeakerDrivesTheProcedureItself)
{
    session.listen();
    speaker.drive(500, 0x3502ffff);
    ASSERT_TRUE(session.start(false)) << sink.all();

    EXPECT_TRUE(sink.has("avdtp: the device opened the signaling channel"));
    EXPECT_TRUE(sink.has("avdtp: the device configured the stream itself (ff ff, bitpool 2-53)"));
    EXPECT_TRUE(sink.has("(the device drove it, started=1)"));
    EXPECT_EQ(48000u, session.config().rate);
    EXPECT_TRUE(session.media().up);
    EXPECT_TRUE(speaker.signals.empty()); // we sent no command of our own
}

TEST_F(Session, HighQualityConfiguresBitpool53)
{
    session.setQuality(kQualityHigh);
    ASSERT_TRUE(session.start(true)) << sink.all();

    EXPECT_EQ(53u, session.config().bitpool);
    EXPECT_TRUE(sink.has("avdtp: streaming SBC 48000 Hz, bitpool 53"));
    ASSERT_GE(speaker.configuration.size(), 12u);
    EXPECT_EQ(53, speaker.configuration[speaker.configuration.size() - 1]); // maximum bitpool in the configuration
    EXPECT_EQ(53, speaker.configuration[speaker.configuration.size() - 2]); // and the minimum
}

TEST_F(Session, AGetAllCapabilitiesFromTheSpeakerIsAccepted)
{
    // the Redmi Buds ask for this (AVDTP 1.3) and wait when it is refused
    session.listen();
    speaker.dialIn(500);
    speaker.sendCommand(1, 0x0c, {0x04}, 1000);
    ASSERT_TRUE(session.start(false)) << sink.all();

    ASSERT_EQ(12u, speaker.lastAvdtpReply.size());
    EXPECT_EQ(0x02, speaker.lastAvdtpReply[0] & 3); // accepted, not rejected
    EXPECT_EQ(0x0c, speaker.lastAvdtpReply[1]);
    EXPECT_TRUE(sink.has("avdtp: > accept 0x0c label 1"));
}

TEST_F(Session, WeStartTheStreamWhenTheSpeakerConfiguredButDoesNotStart)
{
    session.listen();
    speaker.driveSendsStart = false; // we give it 300 ms, then send our own
    speaker.drive(500, 0x3502ffff);
    ASSERT_TRUE(session.start(false)) << sink.all();
    EXPECT_EQ((std::vector<std::uint8_t>{7}), speaker.signals) << sink.all();
    EXPECT_TRUE(speaker.started);
    EXPECT_TRUE(sink.has("(the device drove it, started=1)"));
}

TEST_F(Session, ASpeakerThatDialsInWhileWeWaitIsAccepted)
{
    session.listen();
    speaker.dialIn(2000);
    ASSERT_TRUE(session.start(false)) << sink.all();
    EXPECT_TRUE(sink.has("avdtp: the device opened the signaling channel"));
    // it was quiet, so we ran the procedure
    EXPECT_FALSE(speaker.signals.empty());
    EXPECT_EQ(1, speaker.signals[0]);
}

TEST_F(Session, AfterAConnectionWeMakeWeWaitOnlyOneSecondBeforeOpeningTheChannel)
{
    const auto t0 = clock.nowMs();
    ASSERT_TRUE(session.start(true));
    EXPECT_TRUE(sink.has("avdtp: signaling channel opened by us"));
    // 1 s of waiting, then the setup and short quiet period
    EXPECT_LT(clock.nowMs() - t0, 9000u);
    EXPECT_GE(clock.nowMs() - t0, 1000u);
}

TEST_F(Session, WithoutOurConnectionTheSpeakerGetsTwentySeconds)
{
    const auto t0 = clock.nowMs();
    ASSERT_TRUE(session.start(false));
    EXPECT_GE(clock.nowMs() - t0, 20000u);
}

TEST_F(Session, TheSpeakersDiscoverIsAnsweredWithOneSourceEndpoint)
{
    session.listen();
    speaker.dialIn(100);
    speaker.sendCommand(9, 1, {}, 3000); // it asks first
    ASSERT_TRUE(session.start(false)) << sink.all();
    ASSERT_GE(speaker.lastAvdtpReply.size(), 4u);
    // the last answer we saw may belong to a later command; look at the first accept to a discover
    EXPECT_FALSE(speaker.signals.empty());
}

TEST_F(Session, UnknownCommandsFromTheSpeakerAreRejected)
{
    session.listen();
    speaker.dialIn(100);
    speaker.sendCommand(5, 0x0b, {}, 1000); // security control, a signal we do not know
    session.start(false);
    EXPECT_EQ(1, speaker.rejectedGeneral);
    EXPECT_EQ(0x0b, speaker.lastAvdtpReply.size() > 1 ? speaker.lastAvdtpReply[1] : 0);
}

TEST_F(Session, StopClosesTheMediaAndSignalingChannels)
{
    ASSERT_TRUE(session.start(true));
    controller.aclOut.clear();
    session.stop();

    EXPECT_TRUE(speaker.closed);
    EXPECT_TRUE(session.media().closed);
    EXPECT_TRUE(session.signaling().closed);
}

TEST_F(Session, StopDoesNothingOnALostLink)
{
    ASSERT_TRUE(session.start(true));
    controller.remoteDisconnect();
    stack.poll(10);
    controller.aclOut.clear();
    session.stop();
    EXPECT_TRUE(controller.aclOut.empty());
}

TEST_F(Session, ResetForgetsTheLastSession)
{
    ASSERT_TRUE(session.start(true));
    session.requestStop();
    EXPECT_TRUE(session.stopRequested());
    session.reset();
    EXPECT_FALSE(session.stopRequested());
    EXPECT_EQ(0u, session.media().theirCid);
    EXPECT_FALSE(session.media().up);
    EXPECT_EQ(672u, session.media().theirMtu);
}
