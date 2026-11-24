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

constexpr int kRecordBytes = 0x2000; // the most one capture read returns
constexpr int kMaxFramesPerPacket =
    10; // SBC frames in one media packet (fewer, larger packets: the controller handles about 45 a second at best)
constexpr std::uint64_t kStartFlushMs =
    3000;                                  // for this long the fifo is kept short: what the capture had piled up is old
constexpr int kStartKeepMs = 60;           // ... down to this much
constexpr int kBacklogMs = 150;            // the longest the fifo may get after that (a stall of the radio fills it)
constexpr std::uint64_t kLightAfterMs = 0; // the stream is this old before the reads are cut back
constexpr std::uint64_t kStuckMs = 150;    // the controller has not taken the last packet for this long
constexpr int kFirstPauseMs = 300;
constexpr int kLongestPauseMs = 2000;
constexpr int kBitpoolFloor = 28; // lowest bitpool the rate adaptation goes to (about 207 kbit/s at 48 kHz)

// One run of the stream, from the first packet to the end of the link. Each
// round of the loop: take what the capture has (or, while it has nothing,
// silence at the pace of the clock) into the fifo, cut the fifo back to 300 ms
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
    bool backedOff();
    void publishNumbers();
    void adaptBitrate();
    void applyReadBudget();
    void setBitpool(int bitpool);
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
    int m_trimLimit = 0;    // 150 ms
    int m_startLimit = 0;   // 60 ms
    std::int64_t m_startDropped = 0;
    int m_bitpool = 0, m_bitpoolMin = 0, m_bitpoolMax = 0; // the one in use, and the range it moves in
    std::uint64_t m_lastAdapt = 0, m_lastChange = 0, m_lastStress = 0;
    std::int64_t m_trimmedAtAdapt = 0;
    std::uint64_t m_lastBudget = 0;
    int m_appliedEvents = -2, m_appliedAcl = -2; // what was last handed to the transport
    bool m_budgetApplied = false;

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

    // timing, per 10 s window: the longest send, the longest round of the loop and
    // how many rounds took over 40 ms (the capture ring holds about 2 x that)
    std::uint64_t m_sendMaxMs = 0, m_sendTotalMs = 0, m_loopMaxMs = 0;
    int m_sendCount = 0, m_slowLoops = 0;
    int m_slowSectionsLogged = 0;

    // When the controller stops taking packets (its buffers full, the speaker not answering) it
    // keeps trying to send what it has, and with a second link on the same radio (the gamepads)
    // that is time they do not get. So we stop adding to it for a while, longer each time.
    std::uint64_t m_busySince = 0, m_pauseUntil = 0, m_lastPause = 0;
    int m_pauseMs = kFirstPauseMs, m_pauses = 0;

    // Logs a part of the loop that took long: 10 s freezes were seen right after the reads
    // were cut back, and the log should say which part it was.
    void noteSection(const char* name, std::uint64_t begunMs)
    {
        const std::uint64_t took = m_clock.nowMs() - begunMs;

        if (took >= 250 && m_slowSectionsLogged < 20)
        {
            m_slowSectionsLogged++;
            logLine("stream: %s took %llu ms", name, static_cast<unsigned long long>(took));
        }
    }
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
    m_trimLimit = static_cast<int>(m_sc.rate) * kBacklogMs / 1000; // a short stall of the radio is waited out, not cut
    m_startLimit = static_cast<int>(m_sc.rate) * kStartKeepMs / 1000;
    m_bitpoolMax = static_cast<int>(m_sc.bitpool);
    m_bitpoolMin = m_bitpoolMax < kBitpoolFloor ? m_bitpoolMax : kBitpoolFloor;
    if (m_bitpoolMin < m_sc.poolMin)
    {
        m_bitpoolMin = m_sc.poolMin;
    }
    m_bitpool = m_bitpoolMax;
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

// True while sending is paused because the controller stopped taking packets.
bool StreamRun::backedOff()
{
    const std::uint64_t now = m_clock.nowMs();

    if (now < m_pauseUntil)
    {
        return true;
    }
    if (m_bt.aclOutReady())
    {
        m_busySince = 0;
        if (m_lastPause != 0 && now - m_lastPause > 4000)
        {
            m_pauseMs = kFirstPauseMs; // it has been well for a while
            m_lastPause = 0;
        }
        return false;
    }
    if (m_busySince == 0)
    {
        m_busySince = now;
        return false;
    }
    if (now - m_busySince < kStuckMs)
    {
        return false;
    }
    m_busySince = 0;
    m_pauseUntil = now + static_cast<std::uint64_t>(m_pauseMs);
    m_lastPause = m_pauseUntil;
    if (m_pauses++ < 20)
    {
        logLine("stream: the controller stopped taking packets, pausing %d ms", m_pauseMs);
    }
    m_pauseMs = m_pauseMs * 2 > kLongestPauseMs ? kLongestPauseMs : m_pauseMs * 2;
    return true;
}

// Pack and send what the flow control allows. `pollMs` becomes 0 after a send,
// so that the next packet is not late. False when the stream cannot go on.
bool StreamRun::sendPackets(std::uint32_t& pollMs)
{
    if (m_fifo.size() < m_framesTarget || backedOff() || !m_bt.canSend())
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
        const std::uint64_t sendStart = m_clock.nowMs();
        const bool sent = m_bt.send(m_media.theirCid, m_pkt, off);
        const std::uint64_t sendMs = m_clock.nowMs() - sendStart;

        m_sendTotalMs += sendMs;
        m_sendCount++;
        if (sendMs > m_sendMaxMs)
        {
            m_sendMaxMs = sendMs;
        }
        if (!sent)
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

void StreamRun::setBitpool(int bitpool)
{
    m_bitpool = bitpool;
    m_enc.bitpool = static_cast<std::uint8_t>(bitpool);
    m_framelen = sbc_get_frame_length(&m_enc);
    m_lastChange = m_clock.nowMs();
    logLine("sbc: bitpool %d (%zu-byte frames)", bitpool, m_framelen);
}

// The controller takes what the radio can carry, and that changes with the
// room and with what the console's own Bluetooth does. When the fifo backs up
// (or had to be cut) the rate goes down quickly; after ten calm seconds it goes
// up again, a little at a time, to the bitpool the sink negotiated. libsbc
// reads the bitpool from the encoder for every frame, and each frame carries
// it in its header, so a change needs no new A2DP configuration.
void StreamRun::adaptBitrate()
{
    const std::uint64_t now = m_clock.nowMs();

    if (now - m_lastAdapt < 250 || now - m_streamStart < 1000) // the first second is a burst of backlog
    {
        return;
    }
    m_lastAdapt = now;

    const bool stressed = m_trimmed > m_trimmedAtAdapt || queueMs() > 150;

    m_trimmedAtAdapt = m_trimmed;
    if (stressed)
    {
        m_lastStress = now;
        if (m_bitpool > m_bitpoolMin && now - m_lastChange >= 500)
        {
            setBitpool(m_bitpool - 4 < m_bitpoolMin ? m_bitpoolMin : m_bitpool - 4);
        }
    }
    else if (m_bitpool < m_bitpoolMax && now - m_lastStress >= 10000 && now - m_lastChange >= 2000)
    {
        setBitpool(m_bitpool + 2 > m_bitpoolMax ? m_bitpoolMax : m_bitpool + 2);
    }
}

// The console's own Bluetooth stack shares the chip, and every read we keep waiting
// takes packets from it, replies to its commands and a gamepad's connection requests
// among them. Setup needs many reads; a stream that only writes does not, so after
// the first seconds the reads are cut back to what the service says (none,
// unless told otherwise).
void StreamRun::applyReadBudget()
{
    const std::uint64_t now = m_clock.nowMs();

    if (now - m_streamStart < kLightAfterMs || now - m_lastBudget < 500)
    {
        return;
    }
    m_lastBudget = now;

    const int events = m_session.streamEvents();
    const int acl = m_session.streamAcl();

    if (events == m_appliedEvents && acl == m_appliedAcl)
    {
        return;
    }
    m_appliedEvents = events;
    m_appliedAcl = acl;
    m_budgetApplied = true;
    m_bt.setReadBudget(events < 0 ? 1000 : events, acl < 0 ? 1000 : acl);
    logLine("stream: reads kept while streaming: %d event, %d ACL (-1 = all)", events, acl);
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
    logLine(
        "timing: send avg %llu max %llu ms, loop max %llu ms, slow loops %d, bitpool %d (%d-%d), start drop %lld ms",
        static_cast<unsigned long long>(m_sendCount > 0 ? m_sendTotalMs / m_sendCount : 0),
        static_cast<unsigned long long>(m_sendMaxMs), static_cast<unsigned long long>(m_loopMaxMs), m_slowLoops,
        m_bitpool, m_bitpoolMin, m_bitpoolMax, static_cast<long long>(m_startDropped * 1000 / m_sc.rate));
    m_bt.logEventCounts();
    if (m_bt.eventReadsOn())
    {
        m_bt.probeLink(); // the answers come back to us only while the events are read
    }
    m_sendMaxMs = m_sendTotalMs = m_loopMaxMs = 0;
    m_sendCount = m_slowLoops = 0;
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

    std::uint64_t loopStart = m_clock.nowMs();

    while (!m_bt.linkLost() && !m_media.closed && !m_session.stopRequested())
    {
        const std::uint64_t roundStart = m_clock.nowMs();

        if (roundStart - loopStart > m_loopMaxMs && roundStart != loopStart)
        {
            m_loopMaxMs = roundStart - loopStart;
        }
        if (roundStart - loopStart > 40)
        {
            m_slowLoops++;
        }
        loopStart = roundStart;
        if (m_capturing)
        {
            drainCapture();
        }
        else
        {
            feedSilenceAndRestart();
        }
        noteSection("capture", roundStart);

        // trim the fifo if it grew past the target latency; in the first seconds what the
        // capture had kept for us is old, and playing it would only delay the sound
        if (roundStart - m_streamStart < kStartFlushMs)
        {
            while (m_framesTarget > 0 && m_fifo.size() > m_startLimit)
            {
                m_fifo.dropFront(m_framesTarget);
                m_startDropped += m_framesTarget;
            }
        }
        while (m_framesTarget > 0 && m_fifo.size() > m_trimLimit)
        {
            m_fifo.dropFront(m_framesTarget);
            m_trimmed += m_framesTarget;
        }

        const std::uint64_t adaptStart = m_clock.nowMs();

        adaptBitrate();
        applyReadBudget();
        noteSection("rate and read budget", adaptStart);

        std::uint32_t pollMs = 2;
        const std::uint64_t sendStart = m_clock.nowMs();

        if (!sendPackets(pollMs))
        {
            break;
        }
        noteSection("sending", sendStart);
        publishNumbers();
        if (m_clock.nowMs() - m_lastStats >= 10000)
        {
            m_lastStats = m_clock.nowMs();
            logStatus(); // every 10 s
        }
        const std::uint64_t pollStart = m_clock.nowMs();

        m_bt.poll(pollMs);
        noteSection("bluetooth poll", pollStart);
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
