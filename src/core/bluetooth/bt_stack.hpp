// SPDX-License-Identifier: GPL-3.0-or-later
// The Bluetooth Classic host stack on top of a raw HCI transport: bring-up of
// the controller, inquiry, paging, pairing, encryption, L2CAP and the ACL
// flow control. It does not sleep or touch the system: time comes from the
// IClock, packets from the IHciTransport, pairings from the IBondStore.
//
// Everything runs on the caller's thread. Time only passes inside poll() (and
// the waits built on it), which is how the stack lets the transport work.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/bluetooth/address.hpp"
#include "core/bluetooth/l2cap_reassembler.hpp"
#include "core/ports/bond_store.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/hci_transport.hpp"
#include "core/ports/usb_directory.hpp"

namespace bluev
{

// A device seen during inquiry. The paging fields are what the next page needs.
struct BtDevice
{
    BdAddr address{};
    std::uint8_t pageScanRepMode = 0;
    std::uint16_t clockOffset = 0; // with bit 15 set, as Create Connection wants it
    std::uint32_t classOfDevice = 0;
    std::int32_t rssi = -128; // dBm; -128 when the controller gave none
    std::string name;
};

// One L2CAP channel. The flags are set by the stack as the signaling goes.
struct L2Channel
{
    L2Channel(const char* name_, std::uint32_t ourCid_) : name(name_), ourCid(ourCid_)
    {
    }

    const char* name;             // for log messages
    std::uint32_t ourCid;         // the channel id we chose
    std::uint32_t theirCid = 0;   // the one the device chose
    bool connDone = false;        // the connection response has come
    std::uint32_t result = 0;     // its result code, 0 = success
    bool up = false;              // open and usable
    bool configured = false;      // the configuration exchange is finished
    bool closed = false;          // the device closed it, or the link went down
    std::uint32_t theirMtu = 672; // 672 is the L2CAP default until they say otherwise

    // forget a previous session's state
    void reset()
    {
        theirCid = 0;
        connDone = false;
        result = 0;
        up = false;
        configured = false;
        closed = false;
        theirMtu = 672;
    }
};

// Receives each complete L2CAP frame of a channel: its channel id and payload.
// Called from poll(), so it must not wait for anything.
using FrameHandler = std::function<void(std::uint32_t cid, const std::uint8_t* data, int length)>;

class BtStack
{
  public:
    // The four ports are used, not owned; they must outlive the stack.
    BtStack(IHciTransport& transport, IBondStore& bonds, IUsbDirectory& usb, IClock& clock);

    // ---- the controller (bt_stack.cpp) ----
    // Choose the controller before start(): the console's own chip (empty) or
    // an adapter in a USB port (a name under /dev).
    void useAdapter(const std::string& node);
    // Opens the transport and brings the controller up. The console's own chip
    // is never reset, because the console's stack is using it.
    bool start();
    // why start() returned false, in words for the user (empty = no particular reason)
    const std::string& startError() const
    {
        return m_startError;
    }
    // Turns the stack's presence down: drops our link, leaves page scan on so
    // paired devices can reconnect to the console, and closes the transport.
    void stop();

    // Let the transport work for at most `timeoutMs` and handle what arrived.
    void poll(std::uint32_t timeoutMs);
    // Poll until `done` is true. False on timeout or when the link is lost
    // (unless `stopOnLinkLoss` is off).
    bool wait(const std::function<bool()>& done, std::uint32_t timeoutMs, bool stopOnLinkLoss = true);
    // called about once a second from poll()
    void setTick(std::function<void()> tick)
    {
        m_tick = std::move(tick);
    }
    // Service mode (a long-running program, not a one-off tool): a device that
    // was paired before may dial in and is accepted, any other is counted in
    // foreignRequests() and refused, and inquiry results are not announced.
    void setServiceMode(bool on)
    {
        m_service = on;
    }

    // a command whose answer is waited for; true when it succeeded
    bool hciSync(std::uint16_t opcode, const std::uint8_t* params = nullptr, std::uint8_t length = 0);

    // ---- inquiry and the list of devices (bt_stack_inquiry.cpp) ----
    // Inquiry for up to `timeoutMs`; replaces devices() and returns how many
    // were found.
    int scan(std::uint32_t timeoutMs);
    const std::vector<BtDevice>& devices() const
    {
        return m_devices;
    }
    void setScan(int mode); // 0 off, 1 inquiry, 2 page, 3 both

    // ---- bonds ----
    bool hasBond(const BdAddr& peer);
    std::vector<BondEntry> bonds();  // of the adapter in use
    void forget(const BdAddr& peer); // deletes the pairing (the keys and the saved name)

    // ---- the link (bt_stack_link.cpp) ----
    bool connectTo(const BdAddr& peer); // page, pair when needed, encrypt
    bool adoptIncoming();               // a paired device dialed in
    // True from the moment the connection completes until it is lost.
    bool linkUp() const
    {
        return m_connDone && m_connStatus == 0 && !m_disconnected;
    }
    // The device hung up, or the controller reported the link gone.
    bool linkLost() const
    {
        return m_disconnected;
    }
    void disconnect();
    void linkReset(); // forget the link state and the target
    // HCI connection handle of the link, 0 when there is none.
    std::uint16_t handle() const
    {
        return m_handle;
    }
    // How many links have come up since start(); a change means a new link.
    unsigned connectionCount() const
    {
        return m_connGen;
    }
    std::string peerName() const
    {
        return m_peerName.empty() ? "the device" : m_peerName;
    }
    std::string peerAddress() const
    {
        return m_peerMac.empty() ? "--" : m_peerMac;
    }
    // Connection requests from unpaired devices that were refused (service mode).
    long foreignRequests() const
    {
        return m_foreignRequests.load();
    }

    // ---- L2CAP (bt_stack_l2cap.cpp) ----
    bool openChannel(L2Channel& channel, std::uint16_t psm, FrameHandler handler);
    void closeChannel(L2Channel& channel);
    // accept an inbound connection to `psm` on `channel`
    void bindInbound(std::uint16_t psm, L2Channel& channel, FrameHandler handler);

    // ---- sending and flow control ----
    // One L2CAP frame on channel `cid`, unfragmented; false when it does not fit
    // the controller's ACL buffer or the transport refused it. Takes a credit.
    bool send(std::uint32_t cid, const std::uint8_t* data, int length);
    // True when a credit is free and not too much is in flight (see send()).
    bool canSend();
    // The largest `length` send() takes: the controller's ACL size minus the headers.
    int maxFrame() const
    {
        return m_aclMtu ? static_cast<int>(m_aclMtu) - 4 : 0x444;
    }
    // The numbers below are for the stream statistics in the log.
    int credits() const
    {
        return m_credits;
    }
    // Packets sent that the controller never reported as completed.
    long reportsMissing() const
    {
        return m_sent - m_reported;
    }
    // Packets counted as completed by the timeout, not by a report.
    long completionsAssumed() const
    {
        return m_assumed;
    }
    // Completions and ACL data that belong to links that are not ours (the console's own).
    long otherTraffic() const
    {
        return m_otherCompleted + m_otherAcl;
    }

  private:
    // commands
    // Sends a command and notes it, so its reply can be told from the console's.
    bool cmd(std::uint16_t opcode, const std::uint8_t* params = nullptr, std::uint8_t length = 0);
    // Whether a command reply answers a command we sent recently (the console's
    // stack shares the chip and its replies arrive here too).
    bool replyIsOurs(std::uint16_t opcode);
    // Turns scanning off and disconnects the link we hold, if any.
    void dropStaleLinks();

    // events (bt_stack_events.cpp)
    void handleEvent(const std::uint8_t* evt, int n);
    void onConnectionComplete(const std::uint8_t* e);
    void onConnectionRequest(const std::uint8_t* e);
    void onDisconnectionComplete(const std::uint8_t* e);
    void onAuthenticationComplete(const std::uint8_t* e);
    void onEncryptionChange(const std::uint8_t* e);
    void onCommandComplete(const std::uint8_t* evt, int n);
    void onCommandStatus(const std::uint8_t* e);
    void onCompletedPackets(const std::uint8_t* e, int n);
    void onLinkKeyNotification(const std::uint8_t* e);
    void handleAcl(const std::uint8_t* acl, int n);
    bool isOurLink(const std::uint8_t* e) const;

    // inquiry
    void onInquiryRecord(const std::uint8_t* rec, std::int32_t rssi, const std::uint8_t* eir, int eirLength);
    void runInquiry(std::uint32_t timeoutMs);

    // link
    bool waitConn(std::uint32_t timeoutMs);
    bool pageDevice(const BdAddr& peer, std::uint8_t psrm, std::uint16_t clock);
    bool encryptLink();
    void resetLinkState(); // keeps the target
    void setBond(const BdAddr& peer);
    bool bondExists(const BdAddr& peer);

    // L2CAP
    void signaling(const std::uint8_t* cmd, int length);
    void dispatch(std::uint32_t cid, const std::uint8_t* data, int length);
    void addChannel(L2Channel& channel, FrameHandler handler);
    L2Channel* channelByTheirs(std::uint16_t cid);

    IHciTransport& m_transport;
    IBondStore& m_bondStore;
    IUsbDirectory& m_usb;
    IClock& m_clock;

    // the controller
    std::string m_node; // empty = the console's chip
    HciLayout m_layout;
    bool m_external = false;
    bool m_layoutKnown = false;      // the endpoints are known, no detection needed
    bool m_internalStandard = false; // detected once on the console's chip
    std::string m_startError;
    BdAddr m_local{};
    bool m_localKnown = false;
    bool m_service = false;
    std::function<void()> m_tick;
    std::uint64_t m_lastTick = 0;

    // command bookkeeping: the recent commands we sent, the last Command Complete
    // and how many replies and vendor events were logged (each log is capped)
    struct SentCommand
    {
        std::uint16_t op = 0;
        std::uint64_t at = 0;
    };
    static constexpr int kSentSlots = 16;
    SentCommand m_sentCommands[kSentSlots];
    std::uint16_t m_ccOp = 0;
    std::uint32_t m_ccLen = 0;
    std::uint8_t m_cc[0x44c] = {};
    long m_systemReplies = 0; // replies that were not ours, logged up to 50
    int m_vendorDumps = 0;    // vendor events dumped raw, up to 5
    int m_otherAclLogged = 0; // data of other links logged before ours is up, up to 20

    // the link
    BdAddr m_target{};
    std::uint8_t m_targetPsrm = 0;
    std::uint16_t m_targetClock = 0;
    BondKey m_key;
    std::string m_peerName;
    std::string m_peerMac;
    std::uint16_t m_handle = 0;
    bool m_disconnected = false;
    bool m_connDone = false;
    int m_connStatus = 0;
    bool m_authDone = false;
    int m_authStatus = 0;
    bool m_encOn = false;
    int m_encStatus = 0;
    unsigned m_connGen = 0; // counts successful connections
    std::atomic<long> m_foreignRequests{0};

    // inquiry
    std::vector<BtDevice> m_devices;
    bool m_inqDone = false;
    int m_skippedLogged = 0;

    // flow control: `m_credits` ACL buffers free of `m_creditsMax`; `m_inflight`
    // is a ring of send times of packets not yet reported completed
    std::uint32_t m_aclMtu = 0;
    int m_credits = 0;
    int m_creditsMax = 0;
    long m_sent = 0, m_reported = 0, m_assumed = 0;
    long m_otherCompleted = 0, m_otherAcl = 0;
    std::uint64_t m_inflight[64] = {};
    int m_ifHead = 0, m_ifCount = 0;

    // L2CAP
    static constexpr int kMaxChannels = 4;
    L2Channel m_sdp{"sdp", 0x50};
    L2Channel* m_channels[kMaxChannels] = {};
    FrameHandler m_handlers[kMaxChannels];
    int m_channelCount = 0;
    std::uint8_t m_sigId = 1;
    std::uint16_t m_inPsm = 0;
    L2Channel* m_inChannel = nullptr;
    FrameHandler m_inHandler;
    unsigned m_rejectedGen = 0;
    int m_rejectedCount = 0; // refused inbound connections logged for this link
    L2capReassembler m_reassembler;
};

} // namespace bluev
