// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <vector>

#include "core/audio/pcm_fifo.hpp"
#include "core/audio/resampler.hpp"
#include "core/audio/rtp.hpp"

using namespace bluev;

TEST(PcmFifo, PushClampsAndConverts)
{
    PcmFifo f;
    f.push(1.0f, -1.0f);
    f.push(2.0f, -3.0f); // out of range: clamped
    f.push(0.5f, 0.0f);
    ASSERT_EQ(3, f.size());
    EXPECT_EQ(32767, f.data()[0]);
    EXPECT_EQ(-32767, f.data()[1]);
    EXPECT_EQ(32767, f.data()[2]);
    EXPECT_EQ(-32767, f.data()[3]);
    EXPECT_EQ(16383, f.data()[4]);
    EXPECT_EQ(0, f.data()[5]);
}

TEST(PcmFifo, DropFrontKeepsTheNewest)
{
    PcmFifo f;
    for (int i = 0; i < 10; i++)
    {
        f.push(i / 100.0f, 0.0f);
    }
    f.dropFront(4);
    ASSERT_EQ(6, f.size());
    EXPECT_EQ(static_cast<std::int16_t>(0.04f * 32767.0f), f.data()[0]);
    f.dropFront(100); // more than there is
    EXPECT_EQ(0, f.size());
    f.dropFront(-1);
    EXPECT_EQ(0, f.size());
}

TEST(PcmFifo, SilenceAndTheCapacity)
{
    PcmFifo f;
    f.pushSilence(PcmFifo::kCapacityFrames + 500);
    EXPECT_EQ(PcmFifo::kCapacityFrames, f.size()); // as far as there is room
    f.push(0.5f, 0.5f);                            // full: dropped
    EXPECT_EQ(PcmFifo::kCapacityFrames, f.size());
    EXPECT_EQ(0, f.data()[0]);
    f.clear();
    EXPECT_EQ(0, f.size());
}

TEST(Resampler, FortyEightToFortyFourOneProducesAboutTheRightCount)
{
    LinearResampler r(44100);
    PcmFifo out;
    std::vector<float> in(20000 * 2, 0.25f);
    r.process(in.data(), 20000, out);
    // 20000 frames in give 18375 out, give or take the first one
    EXPECT_NEAR(18375, out.size(), 2);
}

TEST(Resampler, ADcInputStaysDc)
{
    LinearResampler r(44100);
    PcmFifo out;
    std::vector<float> in(2000 * 2, 0.5f);
    r.process(in.data(), 2000, out);
    for (int i = 4; i < out.size() - 1; i++)
    {
        ASSERT_NEAR(16383, out.data()[i * 2], 2) << i;
    }
}

TEST(Resampler, StatePersistsAcrossCallsAndResets)
{
    std::vector<float> in(1000 * 2);
    for (std::size_t i = 0; i < in.size(); i++)
    {
        in[i] = static_cast<float>(i % 7) / 10.0f;
    }
    LinearResampler whole(44100), parts(44100);
    PcmFifo a, b;
    whole.process(in.data(), 1000, a);
    parts.process(in.data(), 500, b);
    parts.process(in.data() + 1000, 500, b); // the second half in a second call
    ASSERT_EQ(a.size(), b.size());
    for (int i = 0; i < a.size() * 2; i++)
    {
        ASSERT_EQ(a.data()[i], b.data()[i]) << i;
    }

    whole.reset();
    PcmFifo c;
    whole.process(in.data(), 1000, c);
    EXPECT_EQ(a.size(), c.size());
}

TEST(Rtp, MediaHeader)
{
    std::uint8_t pkt[13] = {};
    writeMediaHeader(pkt, 0x1234, 0xdeadbeef, 7);
    EXPECT_EQ(0x80, pkt[0]);
    EXPECT_EQ(0x60, pkt[1]);
    EXPECT_EQ(0x12, pkt[2]);
    EXPECT_EQ(0x34, pkt[3]);
    EXPECT_EQ(0xde, pkt[4]);
    EXPECT_EQ(0xad, pkt[5]);
    EXPECT_EQ(0xbe, pkt[6]);
    EXPECT_EQ(0xef, pkt[7]);
    EXPECT_EQ(1, pkt[11]);                    // ssrc 1
    EXPECT_EQ(7, pkt[12]);                    // seven SBC frames
    writeMediaHeader(pkt, 0x10000 + 5, 0, 1); // the sequence wraps at 16 bits
    EXPECT_EQ(0x00, pkt[2]);
    EXPECT_EQ(0x05, pkt[3]);
}
