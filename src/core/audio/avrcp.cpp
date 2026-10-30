// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/avrcp.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev::avrcp
{

namespace
{
constexpr std::uint16_t kRemoteControlPid = 0x110e;
constexpr std::uint8_t kSubunitPanel = 0x48; // subunit type 9, id 0
constexpr std::uint8_t kVendorDependent = 0x00;
constexpr std::uint8_t kBluetoothSigCompany[3] = {0x00, 0x19, 0x58};
constexpr std::uint8_t kControl = 0x00;
constexpr std::uint8_t kNotify = 0x03;
constexpr std::uint8_t kSetAbsoluteVolume = 0x50;
constexpr std::uint8_t kRegisterNotification = 0x31;
constexpr std::uint8_t kEventVolumeChanged = 0x0d;
constexpr std::uint8_t kResponseBit = 0x02; // AVCTP "this is a response"

// AVCTP header (3 bytes), AV/C header (3), company id (3), PDU id, packet type and length (4)
constexpr int kHeaderLength = 13;
} // namespace

bool parse(const std::uint8_t* frame, int length, Message& out)
{
    // single packet, our profile
    if (length < 6 || (frame[0] & 0x0c) != 0 || getBe16(frame + 1) != kRemoteControlPid)
    {
        return false;
    }
    out = Message{};
    out.label = frame[0] >> 4;
    out.response = (frame[0] & kResponseBit) != 0;
    out.ctype = frame[3] & 0x0f;
    if (frame[5] != kVendorDependent || length < kHeaderLength || std::memcmp(frame + 6, kBluetoothSigCompany, 3) != 0)
    {
        return true; // some other AV/C command, no AVRCP PDU
    }
    const int paramLength = getBe16(frame + 11);

    if (kHeaderLength + paramLength > length)
    {
        return false;
    }
    out.pdu = frame[9];
    out.params = frame + kHeaderLength;
    out.paramLength = paramLength;
    return true;
}

std::size_t buildRegisterVolume(std::uint8_t label, std::uint8_t* out)
{
    out[0] = static_cast<std::uint8_t>((label & 0xf) << 4); // a command, single packet
    putBe16(out + 1, kRemoteControlPid);
    out[3] = kNotify;
    out[4] = kSubunitPanel;
    out[5] = kVendorDependent;
    std::memcpy(out + 6, kBluetoothSigCompany, 3);
    out[9] = kRegisterNotification;
    out[10] = 0; // single packet
    putBe16(out + 11, 5);
    out[13] = kEventVolumeChanged;
    putBe32(out + 14, 0); // playback interval, unused for this event
    return 18;
}

std::size_t buildSetVolume(std::uint8_t label, int volume, std::uint8_t* out)
{
    out[0] = static_cast<std::uint8_t>((label & 0xf) << 4);
    putBe16(out + 1, kRemoteControlPid);
    out[3] = kControl;
    out[4] = kSubunitPanel;
    out[5] = kVendorDependent;
    std::memcpy(out + 6, kBluetoothSigCompany, 3);
    out[9] = kSetAbsoluteVolume;
    out[10] = 0; // single packet
    putBe16(out + 11, 1);
    out[13] = static_cast<std::uint8_t>(volume & 0x7f);
    return 14;
}

std::size_t buildNotImplemented(const std::uint8_t* command, int length, std::uint8_t* out)
{
    std::memcpy(out, command, static_cast<std::size_t>(length));
    out[0] |= kResponseBit;
    out[3] = kNotImplemented;
    return static_cast<std::size_t>(length);
}

int volumeFromNotification(const Message& message)
{
    if (!message.response || message.pdu != kRegisterNotification ||
        (message.ctype != kInterim && message.ctype != kChanged) || message.paramLength < 2 ||
        message.params[0] != kEventVolumeChanged)
    {
        return -1;
    }
    return message.params[1] & 0x7f;
}

int volumeFromSetAnswer(const Message& message)
{
    if (!message.response || message.pdu != kSetAbsoluteVolume || message.ctype != kAccepted || message.paramLength < 1)
    {
        return -1;
    }
    return message.params[0] & 0x7f;
}

} // namespace bluev::avrcp

namespace bluev
{

namespace
{
// the first bytes of a frame as hex, for the log
std::string hexStart(const std::uint8_t* data, int length)
{
    std::string text;
    char byte[4];

    for (int i = 0; i < length && i < 24; i++)
    {
        std::snprintf(byte, sizeof(byte), "%02x ", data[i]);
        text += byte;
    }
    return text;
}
} // namespace

Avrcp::Avrcp(BtStack& bt) : m_bt(bt)
{
}

void Avrcp::listen()
{
    m_bt.bindInbound(avrcp::kAvctpPsm, m_channel,
                     [this](std::uint32_t, const std::uint8_t* frame, int length) { onFrame(frame, length); });
}

void Avrcp::reset()
{
    m_channel.reset();
    m_registered = false;
    m_volume.store(-1);
    m_wanted.store(-1);
}

void Avrcp::start()
{
    const auto up = [this] { return m_channel.up && m_channel.configured; };

    m_bt.wait(up, 1000);
    if (up())
    {
        logLine("avrcp: the headset opened the channel");
    }
    else if (m_bt.openChannel(m_channel, avrcp::kAvctpPsm,
                              [this](std::uint32_t, const std::uint8_t* frame, int length) { onFrame(frame, length); }))
    {
        logLine("avrcp: channel opened by us");
    }
    else
    {
        noVolumeControl("it did not open the channel and ours failed");
        return;
    }
    registerVolume();
}

void Avrcp::stop()
{
    if (m_channel.theirCid != 0 && !m_channel.closed)
    {
        m_bt.closeChannel(m_channel);
    }
}

void Avrcp::service()
{
    const int wanted = m_wanted.exchange(-1);

    if (!m_channel.up || !m_channel.configured)
    {
        return;
    }
    if (!m_registered)
    {
        registerVolume(); // the headset opened the channel after start() gave up waiting
    }
    if (wanted >= 0 && m_volume.load() >= 0)
    {
        std::uint8_t frame[14];
        const std::size_t n = avrcp::buildSetVolume(m_label++, wanted, frame);

        if (!m_bt.send(m_channel.theirCid, frame, static_cast<int>(n)))
        {
            logLine("avrcp: could not send the volume");
        }
    }
}

void Avrcp::registerVolume()
{
    std::uint8_t frame[18];
    const std::size_t n = avrcp::buildRegisterVolume(m_label++, frame);

    m_registered = true;
    if (!m_bt.send(m_channel.theirCid, frame, static_cast<int>(n)))
    {
        logLine("avrcp: could not send the volume registration");
    }
}

void Avrcp::noVolumeControl(const char* why)
{
    m_volume.store(-1);
    logLine("avrcp: no volume control on this link (%s)", why);
}

void Avrcp::onFrame(const std::uint8_t* frame, int length)
{
    avrcp::Message message;

    if (!avrcp::parse(frame, length, message))
    {
        logLine("avrcp: unreadable frame, %d bytes: %s", length, hexStart(frame, length).c_str());
        return;
    }
    if (!message.response)
    {
        // the headset is a target for us; a command from it is not expected
        logLine("avrcp: command from the headset, type %#x pdu %#x: %s", message.ctype, message.pdu,
                hexStart(frame, length).c_str());
        std::uint8_t answer[64];

        if (length <= static_cast<int>(sizeof(answer)))
        {
            const std::size_t n = avrcp::buildNotImplemented(frame, length, answer);

            m_bt.send(m_channel.theirCid, answer, static_cast<int>(n));
        }
        return;
    }
    if (message.pdu == 0x31 && (message.ctype == avrcp::kRejected || message.ctype == avrcp::kNotImplemented))
    {
        noVolumeControl("it refused the volume registration");
        return;
    }
    int volume = avrcp::volumeFromNotification(message);

    if (volume < 0)
    {
        volume = avrcp::volumeFromSetAnswer(message);
    }
    if (volume < 0)
    {
        logLine("avrcp: answer, type %#x pdu %#x: %s", message.ctype, message.pdu, hexStart(frame, length).c_str());
        return;
    }
    if (volume != m_volume.exchange(volume))
    {
        logLine("avrcp: headset volume %d of 127", volume);
    }
    if (message.ctype == avrcp::kChanged)
    {
        registerVolume(); // a registration answers once
    }
}

} // namespace bluev
