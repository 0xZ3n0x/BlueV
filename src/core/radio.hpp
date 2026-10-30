// SPDX-License-Identifier: GPL-3.0-or-later
// What the service needs from the radio, and the radio that wires the
// Bluetooth stack to the A2DP session and the streaming loop.
//
// Everything here is used by one thread only (the service's radio thread),
// except foreignRequests(), which may be asked from any thread.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/audio/a2dp_session.hpp"
#include "core/audio/a2dp_streamer.hpp"
#include "core/audio/avrcp.hpp"
#include "core/bluetooth/bt_stack.hpp"
#include "core/ports/audio_source.hpp"
#include "core/ports/bond_store.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/firmware_store.hpp"
#include "core/ports/hci_transport.hpp"
#include "core/ports/usb_directory.hpp"

namespace bluev
{

class IRadio
{
  public:
    virtual ~IRadio() = default;

    // ---- the controller ----
    // Starts the console's own chip (empty node) or the adapter on `node`
    // (a name under /dev). On failure openError() tells why.
    virtual bool open(const std::string& node) = 0;
    // Why the last open() failed, in words for the user; empty when there is no particular reason.
    virtual std::string openError() = 0;
    // Releases the controller (the console's own chip is left in a state its stack can use).
    virtual void close() = 0;
    // `tick` is called about once a second, from inside poll().
    virtual void setTick(std::function<void()> tick) = 0;
    // Where the stream's live numbers go; empty for nobody.
    virtual void setStreamObserver(StreamObserver observer) = 0;
    // Lets the controller work for up to `timeoutMs` and handles what arrived.
    virtual void poll(std::uint32_t timeoutMs) = 0;

    // ---- devices ----
    // Inquiry; returns how many devices were found. devices() is the result.
    virtual int scan(std::uint32_t timeoutMs) = 0;
    virtual std::vector<BtDevice> devices() = 0;
    // Devices paired with the adapter in use, whether or not they were seen.
    virtual std::vector<BondEntry> bonds() = 0;
    virtual bool hasBond(const BdAddr& peer) = 0;
    virtual void forget(const BdAddr& peer) = 0;
    // Connection attempts by unpaired devices that were refused so far.
    virtual long foreignRequests() = 0;

    // ---- a session ----
    // A session goes: link (connectTo, or adoptIncoming when a paired device
    // dialed in), audio setup (startAudio), stream, then stopAudio and
    // disconnect. resetAudio and listenAudio prepare for a link that may come.
    virtual bool linkUp() = 0;
    // Pages the device, pairs it when there is no bond, and encrypts the link.
    virtual bool connectTo(const BdAddr& peer) = 0;
    // Takes over a link a paired device opened by itself; false when there is none.
    virtual bool adoptIncoming() = 0;
    virtual void disconnect() = 0;
    // "the device" and "--" until a peer is known.
    virtual std::string peerName() = 0;
    virtual std::string peerAddress() = 0;
    // Forgets the previous audio session and any stop request.
    virtual void resetAudio() = 0;
    // Accepts the signaling channel when the device opens it.
    virtual void listenAudio() = 0;
    // Sets up AVDTP and the media channel. `initiatedByUs`: we made the
    // connection, so the device is not given long to start the procedure.
    virtual bool startAudio(bool initiatedByUs) = 0;
    virtual void stopAudio() = 0;
    // Streams until the link is lost, the channel closes or requestStop() is
    // called; true when at least one packet went out. `capturing`: the audio
    // source is already running.
    virtual bool stream(bool capturing) = 0;
    // From any thread: ends stream() and the setup waits.
    virtual void requestStop() = 0;
    // The headset's own volume, 0..127; -1 when it has none or has not said.
    // From any thread.
    virtual int volume() = 0;
    // Asks the headset to take this volume, 0..127. From any thread.
    virtual void setVolume(int volume) = 0;
};

class Radio final : public IRadio
{
  public:
    Radio(IHciTransport& transport, IBondStore& bonds, IUsbDirectory& usb, IFirmwareStore& firmware,
          IAudioSource& audio, IClock& clock);

    bool open(const std::string& node) override;
    std::string openError() override;
    void close() override;
    void setTick(std::function<void()> tick) override
    {
        m_bt.setTick([this, tick = std::move(tick)] {
            m_avrcp.service();
            tick();
        });
    }
    void setStreamObserver(StreamObserver observer) override
    {
        m_streamer.setObserver(std::move(observer));
    }
    void poll(std::uint32_t timeoutMs) override
    {
        m_bt.poll(timeoutMs);
    }

    int scan(std::uint32_t timeoutMs) override
    {
        return m_bt.scan(timeoutMs);
    }
    std::vector<BtDevice> devices() override
    {
        return m_bt.devices();
    }
    std::vector<BondEntry> bonds() override
    {
        return m_bt.bonds();
    }
    bool hasBond(const BdAddr& peer) override
    {
        return m_bt.hasBond(peer);
    }
    void forget(const BdAddr& peer) override
    {
        m_bt.forget(peer);
    }
    long foreignRequests() override
    {
        return m_bt.foreignRequests();
    }

    bool linkUp() override
    {
        return m_bt.linkUp();
    }
    bool connectTo(const BdAddr& peer) override
    {
        return m_bt.connectTo(peer);
    }
    bool adoptIncoming() override
    {
        return m_bt.adoptIncoming();
    }
    void disconnect() override
    {
        m_bt.disconnect();
    }
    std::string peerName() override
    {
        return m_bt.peerName();
    }
    std::string peerAddress() override
    {
        return m_bt.peerAddress();
    }
    void resetAudio() override
    {
        m_session.reset();
        m_avrcp.reset();
    }
    void listenAudio() override
    {
        m_session.listen();
        m_avrcp.listen();
    }
    bool startAudio(bool initiatedByUs) override
    {
        if (!m_session.start(initiatedByUs))
        {
            return false;
        }
        m_avrcp.start();
        return true;
    }
    void stopAudio() override
    {
        m_avrcp.stop();
        m_session.stop();
    }
    bool stream(bool capturing) override
    {
        return m_streamer.run(capturing);
    }
    void requestStop() override
    {
        m_session.requestStop();
    }
    int volume() override
    {
        return m_avrcp.volume();
    }
    void setVolume(int volume) override
    {
        m_avrcp.setVolume(volume);
    }

  private:
    BtStack m_bt;
    A2dpSession m_session;
    Avrcp m_avrcp;
    A2dpStreamer m_streamer;
};

} // namespace bluev
