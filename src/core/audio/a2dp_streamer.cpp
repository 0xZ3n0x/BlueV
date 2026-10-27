// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/a2dp_streamer.hpp"

#include <vector>

#include "core/audio/pcm_fifo.hpp"
#include "core/audio/resampler.hpp"
#include "core/audio/rtp.hpp"
#include "core/ports/log.hpp"

extern "C"
{
#include "sbc.h"
}

namespace bluev
{

namespace
{

constexpr int kRecordBytes = 0x2000;   // the most one capture read returns
constexpr int kMaxFramesPerPacket = 8; // SBC frames in one media packet

} // namespace

A2dpStreamer::A2dpStreamer(BtStack& bt, A2dpSession& session, IAudioSource& audio, IClock& clock,
                           StreamObserver observer)
    : m_bt(bt), m_session(session), m_audio(audio), m_clock(clock), m_observer(std::move(observer))
{
}

// Each round of the loop: take what the capture has (or, while it has nothing,
// silence at the pace of the clock) into the fifo, cut the fifo back to 50 ms
// when it has grown beyond that, send as many whole packets as the flow control
// allows, publish the numbers, then let the controller work (poll) for 2 ms, or
// not at all after sending, so that the next packet is not late.
bool A2dpStreamer::run(bool capturing)
{
    const A2dpConfig& sc = m_session.config();
    L2Channel& media = m_session.media();
    sbc_t enc;
    std::vector<float> rec(kRecordBytes / 4);   // capture record (float PCM)
    std::uint8_t pkt[kMediaHeaderBytes + 1096]; // room for the largest frame BtStack::send accepts
    PcmFifo fifo;
    LinearResampler resampler(sc.rate);
    std::uint64_t lastPublish = 0;
    int ratePackets = 0, pps = 0;
    std::int64_t filled = 0, sentSamples = 0, trimmed = 0;
    std::uint32_t seq = 1;
    int packets = 0, records = 0, restarts = 0;

    if (sbc_init(&enc, 0) != 0)
    {
        logLine("sbc: init failed");
        return false;
    }
    enc.frequency = sc.freqCode;
    enc.blocks = sc.blocks;
    enc.subbands = sc.subbands8 ? SBC_SB_8 : SBC_SB_4;
    enc.mode = sc.modeCode;
    enc.allocation = sc.allocation;
    enc.bitpool = sc.bitpool;

    const std::size_t codesize = sbc_get_codesize(&enc);
    const std::size_t framelen = sbc_get_frame_length(&enc);

    // How many SBC frames go into one packet: what the device's MTU and the
    // controller's buffer allow, at most kMaxFramesPerPacket.
    int framesPerPacket;
    {
        int mtu = static_cast<int>(media.theirMtu);

        if (mtu > m_bt.maxFrame() - 4)
        {
            mtu = m_bt.maxFrame() - 4;
        }
        const int inFrames = (mtu - kMediaHeaderBytes) / static_cast<int>(framelen);
        framesPerPacket = inFrames < kMaxFramesPerPacket ? inFrames : kMaxFramesPerPacket;
        if (inFrames < 1)
        {
            logLine("sbc: a %zu-byte frame does not fit a packet", framelen);
            sbc_finish(&enc);
            return false;
        }
    }
    const int framesTarget = framesPerPacket * static_cast<int>(codesize / 4); // frames per packet
    const int trimLimit = static_cast<int>(sc.rate / 20);                      // 50 ms

    logLine("sbc: %zu-byte frames, %d per packet (%d ms)", framelen, framesPerPacket,
            framesTarget * 1000 / static_cast<int>(sc.rate));
    logLine("stream: to %s, %d Hz, bitpool %d", m_bt.peerName().c_str(), sc.rate, sc.bitpool);
    notify("Streaming to %s", m_bt.peerName().c_str());

    const std::uint64_t streamStart = m_clock.nowMs();
    std::uint64_t rateT = streamStart;
    std::uint64_t fillRef = capturing ? 0 : streamStart;
    std::uint64_t waitUntil = capturing ? 0 : streamStart;
    std::uint64_t lastStats = streamStart;

    while (!m_bt.linkLost() && !media.closed && !m_session.stopRequested())
    {
        if (!capturing)
        {
            // keep the fifo aligned with wall clock silence
            const std::int64_t target = static_cast<std::int64_t>(
                static_cast<double>(sc.rate) * static_cast<double>(m_clock.nowMs() - fillRef) / 1000.0);
            const std::int64_t miss = target - filled;

            if (miss != 0 && filled <= target)
            {
                fifo.pushSilence(static_cast<int>(miss));
                filled = target;
            }
            if (m_clock.nowMs() >= waitUntil)
            {
                if (m_audio.restart())
                {
                    capturing = true;
                    restarts++;
                }
                else
                {
                    waitUntil = m_clock.nowMs() + 500;
                }
            }
        }
        else
        {
            // drain the capture; a frame is 8 bytes (a left and a right float)
            int n = m_audio.read(rec.data(), kRecordBytes);

            while (n >= 1)
            {
                const int frames = n >> 3;

                if (n < 8)
                {
                    // too short to hold a frame
                }
                else if (sc.rate == 48000)
                {
                    for (int f = 0; f < frames; f++)
                    {
                        fifo.push(rec[f * 2], rec[f * 2 + 1]);
                    }
                }
                else
                {
                    resampler.process(rec.data(), frames, fifo); // linear 48k -> rate
                }
                records++;
                n = m_audio.read(rec.data(), kRecordBytes);
            }
            if (n != 0)
            { // capture error: start over with silence and try again in 0.5 s
                fifo.clear();
                fillRef = m_clock.nowMs();
                waitUntil = m_clock.nowMs() + 500;
                filled = 0;
                capturing = false;
            }
        }

        // trim the fifo if it grew past the target latency
        while (framesTarget > 0 && fifo.size() > trimLimit)
        {
            fifo.dropFront(framesTarget);
            trimmed += framesTarget;
        }

        std::uint32_t pollMs = 2;

        // pack and send what we can
        if (fifo.size() >= framesTarget && m_bt.canSend())
        {
            do
            {
                int off = kMediaHeaderBytes;

                if (static_cast<long>((m_clock.nowMs() - streamStart) + 25) <
                        sentSamples * 1000 / static_cast<long>(sc.rate) &&
                    static_cast<long>(fifo.size()) * 1000 / static_cast<long>(sc.rate) < 80)
                {
                    break; // more than 25 ms ahead of the clock, and not short of data: wait
                }

                writeMediaHeader(pkt, seq, static_cast<std::uint32_t>(sentSamples),
                                 static_cast<std::uint8_t>(framesPerPacket));
                seq++;

                for (int f = 0; f < framesPerPacket; f++)
                {
                    ssize_t written = 0;
                    const ssize_t r =
                        sbc_encode(&enc, fifo.data() + f * (codesize / 2), codesize, pkt + off, framelen, &written);

                    if (r < 1 || written < 1)
                    {
                        logLine("sbc: encode failed (%zd, %zd)", r, written);
                        goto out; // leave the loop and report the end
                    }
                    off += static_cast<int>(written);
                }
                fifo.dropFront(framesTarget);
                if (!m_bt.send(media.theirCid, pkt, off))
                {
                    logLine("media: send failed after %d packets", packets);
                    goto out;
                }
                packets++;
                pollMs = 0;
                sentSamples += framesTarget;
                if (fifo.size() < framesTarget)
                {
                    break; // not enough left for another packet
                }
            } while (m_bt.canSend());
        }

        // numbers for the web interface, a few times a second
        {
            const std::uint64_t tn = m_clock.nowMs();

            if (tn - lastPublish >= 250)
            {
                if (tn - rateT >= 2000)
                {
                    pps = static_cast<int>((packets - ratePackets) * 1000 / static_cast<std::int64_t>(tn - rateT));
                    ratePackets = packets;
                    rateT = tn;
                }
                if (m_observer)
                {
                    m_observer(static_cast<int>(sc.rate),
                               static_cast<int>(static_cast<long>(fifo.size()) * 1000 / static_cast<long>(sc.rate)),
                               pps, packets);
                }
                lastPublish = tn;
            }
        }

        // status line every 10 s
        if (m_clock.nowMs() - lastStats >= 10000)
        {
            lastStats = m_clock.nowMs();
            logLine(
                "stream: %d packets, %d capture records, queue %d ms, trimmed %ld, overruns %ld, "
                "restarts %d, %s, credits %d, completions assumed %ld, reports missing %ld, "
                "other links %ld",
                packets, records, static_cast<int>(static_cast<long>(fifo.size()) * 1000 / static_cast<long>(sc.rate)),
                static_cast<long>(trimmed), m_audio.overruns(), restarts, capturing ? "capturing" : "waiting for audio",
                m_bt.credits(), m_bt.completionsAssumed(), m_bt.reportsMissing(), m_bt.otherTraffic());
        }

        m_bt.poll(pollMs);
    }

out:
    logLine("stream: ended after %ld s, %d packets, %d capture records, trimmed %ld",
            static_cast<long>((m_clock.nowMs() - streamStart) / 1000), packets, records, static_cast<long>(trimmed));
    sbc_finish(&enc);
    return packets > 0;
}

} // namespace bluev
