// SPDX-License-Identifier: GPL-3.0-or-later
// AVRCP, only as far as the headset's volume goes: we are the controller, the
// headset is the target. The channel (AVCTP, psm 0x17) is separate from A2DP;
// whatever goes wrong here only means "no volume control", never a lost stream.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/bluetooth/bt_stack.hpp"

namespace bluev::avrcp
{

constexpr std::uint16_t kAvctpPsm = 0x17;

enum Response : std::uint8_t
{
    kNotImplemented = 0x08,
    kAccepted = 0x09,
    kRejected = 0x0a,
    kInterim = 0x0f,
    kChanged = 0x0d,
};

// What we read from a frame that carries an AV/C vendor command or response.
struct Message
{
    std::uint8_t label = 0; // transaction label, 0..15
    bool response = false;  // false: a command from the headset
    std::uint8_t ctype = 0; // command type, or the Response
    std::uint8_t pdu = 0;   // AVRCP PDU id, 0 when the frame is not an AVRCP PDU
    const std::uint8_t* params = nullptr;
    int paramLength = 0;
};

// False when the frame is not a complete single-packet AVRCP message.
bool parse(const std::uint8_t* frame, int length, Message& out);

// Asks the headset to tell us when its volume changes (REGISTER_NOTIFICATION,
// event VOLUME_CHANGED). It answers at once with the current volume (interim)
// and again at each change; every answer has to be followed by a new request.
std::size_t buildRegisterVolume(std::uint8_t label, std::uint8_t* out);
// Sets the headset's volume (SET_ABSOLUTE_VOLUME), 0..127.
std::size_t buildSetVolume(std::uint8_t label, int volume, std::uint8_t* out);
// The answer to a command we do not handle.
std::size_t buildNotImplemented(const std::uint8_t* command, int length, std::uint8_t* out);

// The volume (0..127) in a VOLUME_CHANGED answer; -1 when it is not one.
int volumeFromNotification(const Message& message);
// The volume the headset set after SET_ABSOLUTE_VOLUME; -1 when the answer is
// not an acceptance.
int volumeFromSetAnswer(const Message& message);

} // namespace bluev::avrcp

namespace bluev
{

// The AVCTP channel of a link. Once the headset has opened it, or we have, it
// asks for the volume and keeps track of it. volume() and setVolume() may be
// called from any thread; the rest belongs to the radio thread.
class Avrcp
{
  public:
    explicit Avrcp(BtStack& bt);

    // Accept the channel when the headset opens it.
    void listen();
    // Forget the previous link.
    void reset();
    // Waits briefly for the headset to open the channel, opens it otherwise,
    // then registers for volume changes. Every link asks the headset again.
    void start();
    // Closes the channel.
    void stop();
    // Called about once a second: registers when the headset opened the
    // channel late, and sends a volume that setVolume() asked for.
    void service();

    // The headset's volume, 0..127; -1 until it has told us (or when it has
    // no volume control).
    int volume() const
    {
        return m_volume.load();
    }
    // Asks for a new volume, 0..127; ignored while the headset has none.
    void setVolume(int volume)
    {
        m_wanted.store(volume);
    }

  private:
    void onFrame(const std::uint8_t* frame, int length);
    void registerVolume();
    void noVolumeControl(const char* why);

    BtStack& m_bt;
    L2Channel m_channel{"avctp", 0x44};
    std::uint8_t m_label = 0;
    bool m_registered = false;
    std::atomic<int> m_volume{-1};
    std::atomic<int> m_wanted{-1};
};

} // namespace bluev
