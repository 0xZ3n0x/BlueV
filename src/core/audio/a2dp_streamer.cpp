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

// One run of the stream, from the first packet to the end of the link. Each
// round of the loop: take what the capture has (or, while it has nothing,
// silence at the pace of the clock) into the fifo, cut the fifo back to 50 ms
// when it has grown beyond that, send as many whole packets as the flow control
// allows, publish the numbers, then let the controller work (poll) for 2 ms, or
// not at all after sending, so that the next packet is not late.
class StreamRun
{
  public:
    StreamRun(BtStack& bt, A2dpSession& session, IAudioSource& audio, IClock& clock, const StreamObserver& observer,
              bool capturing)
        : m_bt(bt), m_session(session), m_audio(audio), m_clock(clock), m_observer(observer), m_sc(session.config()),
          m_media(session.media()), m_rec(kRecordBytes / 4), m_resampler(m_sc.rate), m_capturing(capturing)
    {
    }
    ~StreamRun()
    {
        if (m_encoderReady)
        {
            sbc_finish(&m_enc);
        }
    }
    StreamRun(const StreamRun&) = delete;
    StreamRun& operator=(const StreamRun&) = delete;

    bool run();

  private:
    bool prepareEncoder();
    void feedSilenceAndRestart();
    void drainCapture();
    bool sendPackets(std::uint32_t& pollMs);
    void publishNumbers();
    void logStatus();
    int queueMs() const
    {
        return static_cast<int>(static_cast<long>(m_fifo.size()) * 1000 / static_cast<long>(m_sc.rate));
    }

    BtStack& m_bt;
    A2dpSession& m_session;
    IAudioSource& m_audio;
    IClock& m_clock;
    const StreamObserver& m_observer;
    const A2dpConfig& m_sc;
    L2Channel& m_media;

    sbc_t m_enc{};
    bool m_encoderReady = false;
    std::size_t m_codesize = 0, m_framelen = 0;
    int m_framesPerPacket = 0;
    int m_framesTarget = 0; // PCM frames in one packet
    int m_trimLimit = 0;    // 50 ms

    std::vector<float> m_rec;                     // capture record (float PCM)
    std::uint8_t m_pkt[kMediaHeaderBytes + 1096]; // room for the largest frame BtStack::send accepts
    PcmFifo m_fifo;
    LinearResampler m_resampler;

    bool m_capturing;
    std::uint64_t m_streamStart = 0, m_rateT = 0, m_fillRef = 0, m_waitUntil = 0, m_lastStats = 0, m_lastPublish = 0;
    int m_ratePackets = 0, m_pps = 0;
    std::int64_t m_filled = 0, m_sentSamples = 0, m_trimmed = 0;
    std::uint32_t m_seq = 1;
    int m_packets = 0, m_records = 0, m_restarts = 0;
};

// The encoder, and how many SBC frames go into one packet: what the device's
// MTU and the controller's buffer allow, at most kMaxFramesPerPacket.
bool StreamRun::prepareEncoder()
{
    if (sbc_init(&m_enc, 0) != 0)
    {
        logLine("sbc: init failed");
        return false;
    }
    m_encoderReady = true;
    m_enc.frequency = m_sc.freqCode;
    m_enc.blocks = m_sc.blocks;
    m_enc.subbands = m_sc.subbands8 ? SBC_SB_8 : SBC_SB_4;
    m_enc.mode = m_sc.modeCode;
    m_enc.allocation = m_sc.allocation;
    m_enc.bitpool = m_sc.bitpool;

    m_codesize = sbc_get_codesize(&m_enc);
    m_framelen = sbc_get_frame_length(&m_enc);

    int mtu = static_cast<int>(m_media.theirMtu);

    if (mtu > m_bt.maxFrame() - 4)
    {
        mtu = m_bt.maxFrame() - 4;
    }
    const int inFrames = (mtu - kMediaHeaderBytes) / static_cast<int>(m_framelen);

    m_framesPerPacket = inFrames < kMaxFramesPerPacket ? inFrames : kMaxFramesPerPacket;
    if (inFrames < 1)
    {
        logLine("sbc: a %zu-byte frame does not fit a packet", m_framelen);
        return false;
    }
    m_framesTarget = m_framesPerPacket * static_cast<int>(m_codesize / 4);
    m_trimLimit = static_cast<int>(m_sc.rate / 20);
    return true;
}

// Waiting for audio: keep the fifo aligned with wall clock silence, and ask the
// capture to start again every half second.
void StreamRun::feedSilenceAndRestart()
{
    const std::int64_t target = static_cast<std::int64_t>(static_cast<double>(m_sc.rate) *
                                                          static_cast<double>(m_clock.nowMs() - m_fillRef) / 1000.0);
    const std::int64_t miss = target - m_filled;

    if (miss != 0 && m_filled <= target)
    {
        m_fifo.pushSilence(static_cast<int>(miss));
        m_filled = target;
    }
    if (m_clock.nowMs() >= m_waitUntil)
    {
        if (m_audio.restart())
        {
            m_capturing = true;
            m_restarts++;
        }
        else
        {
            m_waitUntil = m_clock.nowMs() + 500;
        }
    }
}

// Drain the capture into the fifo; a frame is 8 bytes (a left and a right float).
void StreamRun::drainCapture()
{
    int n = m_audio.read(m_rec.data(), kRecordBytes);

    while (n >= 1)
    {
        const int frames = n >> 3;

        if (frames == 0)
        {
            // too short to hold a frame
        }
        else if (m_sc.rate == 48000)
        {
            for (int f = 0; f < frames; f++)
            {
                m_fifo.push(m_rec[f * 2], m_rec[f * 2 + 1]);
            }
        }
        else
        {
            m_resampler.process(m_rec.data(), frames, m_fifo); // linear 48k -> rate
        }
        m_records++;
        n = m_audio.read(m_rec.data(), kRecordBytes);
    }
    if (n != 0)
    { // capture error: start over with silence and try again in 0.5 s
        m_fifo.clear();
        m_fillRef = m_clock.nowMs();
        m_waitUntil = m_clock.nowMs() + 500;
        m_filled = 0;
        m_capturing = false;
    }
}

// Pack and send what the flow control allows. `pollMs` becomes 0 after a send,
// so that the next packet is not late. False when the stream cannot go on.
bool StreamRun::sendPackets(std::uint32_t& pollMs)
{
    if (m_fifo.size() < m_framesTarget || !m_bt.canSend())
    {
        return true;
    }
    do
    {
        int off = kMediaHeaderBytes;

        if (static_cast<long>((m_clock.nowMs() - m_streamStart) + 25) <
                m_sentSamples * 1000 / static_cast<long>(m_sc.rate) &&
            queueMs() < 80)
        {
            break; // more than 25 ms ahead of the clock, and not short of data: wait
        }

        writeMediaHeader(m_pkt, m_seq, static_cast<std::uint32_t>(m_sentSamples),
                         static_cast<std::uint8_t>(m_framesPerPacket));
        m_seq++;

        for (int f = 0; f < m_framesPerPacket; f++)
        {
            ssize_t written = 0;
            const ssize_t r =
                sbc_encode(&m_enc, m_fifo.data() + f * (m_codesize / 2), m_codesize, m_pkt + off, m_framelen, &written);

            if (r < 1 || written < 1)
            {
                logLine("sbc: encode failed (%zd, %zd)", r, written);
                return false;
            }
            off += static_cast<int>(written);
        }
        m_fifo.dropFront(m_framesTarget);
        if (!m_bt.send(m_media.theirCid, m_pkt, off))
        {
            logLine("media: send failed after %d packets", m_packets);
            return false;
        }
        m_packets++;
        pollMs = 0;
        m_sentSamples += m_framesTarget;
        if (m_fifo.size() < m_framesTarget)
        {
            break; // not enough left for another packet
        }
    } while (m_bt.canSend());
    return true;
}

// numbers for the web interface, a few times a second
void StreamRun::publishNumbers()
{
    const std::uint64_t now = m_clock.nowMs();

    if (now - m_lastPublish < 250)
    {
        return;
    }
    if (now - m_rateT >= 2000)
    {
        m_pps = static_cast<int>((m_packets - m_ratePackets) * 1000 / static_cast<std::int64_t>(now - m_rateT));
        m_ratePackets = m_packets;
        m_rateT = now;
    }
    if (m_observer)
    {
        m_observer(static_cast<int>(m_sc.rate), queueMs(), m_pps, m_packets);
    }
    m_lastPublish = now;
}

void StreamRun::logStatus()
{
    logLine("stream: %d packets, %d capture records, queue %d ms, trimmed %ld, overruns %ld, "
            "restarts %d, %s, credits %d, completions assumed %ld, reports missing %ld, "
            "other links %ld",
            m_packets, m_records, queueMs(), static_cast<long>(m_trimmed), m_audio.overruns(), m_restarts,
            m_capturing ? "capturing" : "waiting for audio", m_bt.credits(), m_bt.completionsAssumed(),
            m_bt.reportsMissing(), m_bt.otherTraffic());
}

bool StreamRun::run()
{
    if (!prepareEncoder())
    {
        return false;
    }
    logLine("sbc: %zu-byte frames, %d per packet (%d ms)", m_framelen, m_framesPerPacket,
            m_framesTarget * 1000 / static_cast<int>(m_sc.rate));
    logLine("stream: to %s, %d Hz, bitpool %d", m_bt.peerName().c_str(), m_sc.rate, m_sc.bitpool);
    notify("Streaming to %s", m_bt.peerName().c_str());

    m_streamStart = m_clock.nowMs();
    m_rateT = m_streamStart;
    m_fillRef = m_capturing ? 0 : m_streamStart;
    m_waitUntil = m_capturing ? 0 : m_streamStart;
    m_lastStats = m_streamStart;

    while (!m_bt.linkLost() && !m_media.closed && !m_session.stopRequested())
    {
        if (m_capturing)
        {
            drainCapture();
        }
        else
        {
            feedSilenceAndRestart();
        }

        // trim the fifo if it grew past the target latency
        while (m_framesTarget > 0 && m_fifo.size() > m_trimLimit)
        {
            m_fifo.dropFront(m_framesTarget);
            m_trimmed += m_framesTarget;
        }

        std::uint32_t pollMs = 2;

        if (!sendPackets(pollMs))
        {
            break;
        }
        publishNumbers();
        if (m_clock.nowMs() - m_lastStats >= 10000)
        {
            m_lastStats = m_clock.nowMs();
            logStatus(); // every 10 s
        }
        m_bt.poll(pollMs);
    }

    logLine("stream: ended after %ld s, %d packets, %d capture records, trimmed %ld",
            static_cast<long>((m_clock.nowMs() - m_streamStart) / 1000), m_packets, m_records,
            static_cast<long>(m_trimmed));
    return m_packets > 0;
}

} // namespace

A2dpStreamer::A2dpStreamer(BtStack& bt, A2dpSession& session, IAudioSource& audio, IClock& clock,
                           StreamObserver observer)
    : m_bt(bt), m_session(session), m_audio(audio), m_clock(clock), m_observer(std::move(observer))
{
}

bool A2dpStreamer::run(bool capturing)
{
    StreamRun stream(m_bt, m_session, m_audio, m_clock, m_observer, capturing);

    return stream.run();
}

} // namespace bluev
