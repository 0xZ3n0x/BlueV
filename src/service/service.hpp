// SPDX-License-Identifier: GPL-3.0-or-later
// The service: a state machine that is driven by commands from the web
// interface and by paired devices dialling in.
//
// The radio is only ever touched by the thread that runs run(). Other
// threads use post() and the *Json() snapshots, which are guarded by
// m_mutex; the two flags that must be seen at once from the radio thread are
// atomic.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/ports/log_reader.hpp"
#include "core/ports/shortcut.hpp"
#include "core/ports/stream_observer.hpp"
#include "core/ports/system_info.hpp"
#include "core/ports/usb_directory.hpp"
#include "core/radio.hpp"
#include "service/sync.hpp"
#include "service/web.hpp"

namespace bluev
{

class Service final : public IStreamObserver
{
  public:
    enum class State
    {
        Idle,
        Scanning,
        Connecting,
        Streaming
    };
    // What a command got: Ok = taken; Busy = another command is waiting to be
    // run; BadAddress / BadAdapter = the argument is wrong; NotAllowed = not in
    // this state (or an unknown command); RadioOff = it needs the radio on.
    enum class PostResult
    {
        Ok,
        Busy,
        BadAddress,
        NotAllowed,
        RadioOff,
        BadAdapter
    };

    // What the service works with; the caller owns all of it and keeps it alive.
    struct Ports
    {
        IRadio& radio;
        IUsbDirectory& usb;
        ISystemInfo& system;
        IShortcut& shortcut;
        ILogReader& log;
        std::string_view page; // the control page
        std::string
            address{}; // where the console can be reached on the network, for the "ready" notification; may be empty
    };

    // `server`: the range of ports the web interface may listen on.
    explicit Service(const Ports& ports, ServerPorts server = {});

    // ---- any thread -------------------------------------------------
    // A command from the web interface: scan, connect, forget (the last two with
    // an address), disconnect, uninstall-shortcut or quit. Scan, connect and
    // forget only run while idle and are done later by the radio thread, one at
    // a time; disconnect and quit are always accepted.
    PostResult post(std::string_view command, std::string_view address) BLUEV_EXCLUDES(m_mutex);
    // Switch the radio on with the chosen controller ("internal", or
    // "external" with the name of its node under /dev) or off again. Until
    // it is on, nothing touches the USB side.
    PostResult startRadio(std::string_view adapter, std::string_view node) BLUEV_EXCLUDES(m_mutex);
    PostResult stopRadio() BLUEV_EXCLUDES(m_mutex);
    // The state, the peer, the stream numbers and the radio, as JSON for /api/status.
    std::string statusJson() BLUEV_EXCLUDES(m_mutex);
    // The last scan plus the paired devices, as JSON for /api/devices.
    std::string devicesJson() BLUEV_EXCLUDES(m_mutex);
    // The live numbers from the streaming loop; the same as onStream().
    void publishStream(int rate, int queueMs, int packetsPerSecond, long packets) BLUEV_EXCLUDES(m_mutex);
    void onStream(int rate, int queueMs, int packetsPerSecond, long packets) override
    {
        publishStream(rate, queueMs, packetsPerSecond, packets);
    }

    // ---- radio thread only ------------------------------------------
    // Starts the web server and serves commands until "quit" is posted, then
    // stops the server and the radio. Returns true. `capturing`: the audio
    // capture is already running.
    bool run(bool capturing);
    // Called about once a second from inside the radio's poll: flushes the log,
    // refreshes the peer and the device list, and ends a session that was asked to stop.
    void tick() BLUEV_EXCLUDES(m_mutex);

    // the port the web interface listens on (0 until it is up)
    int port() const
    {
        return m_server.port();
    }

  private:
    enum class Command
    {
        None,
        Scan,
        Connect,
        Forget,
        StartRadio,
        StopRadio
    };

    // The one command waiting for the radio thread.
    struct Pending
    {
        Command command = Command::None;
        BdAddr address{};
        std::string node; // StartRadio: empty = the console's chip
    };

    // A device as the page shows it (the text is already final).
    struct DeviceInfo
    {
        std::string address;
        std::string name;
        int rssi = -128;
        unsigned cod = 0;
        bool bonded = false;
        bool connected = false;
        bool other = false; // probably not an audio device
    };

    struct StreamInfo
    {
        int rate = 0;
        int queueMs = 0;
        int packetsPerSecond = 0;
        long packets = 0;
    };

    // Changes the state and the message the page shows.
    void setState(State state, const char* fmt, ...) BLUEV_EXCLUDES(m_mutex) __attribute__((format(printf, 3, 4)));
    Pending takePending() BLUEV_EXCLUDES(m_mutex);
    void publishDevices() BLUEV_EXCLUDES(m_mutex);
    // One connection from start to end; `address` null = a device dialled in.
    void runSession(bool capturing, const BdAddr* address);
    // Opens the controller (empty node = the console's chip); false, with the
    // reason shown, when it does not start.
    bool openRadio(const std::string& node) BLUEV_EXCLUDES(m_mutex);
    void closeRadio() BLUEV_EXCLUDES(m_mutex);

    static const char* stateName(State state);

    IRadio& m_radio;
    IUsbDirectory& m_usb;
    ISystemInfo& m_system;
    IShortcut& m_shortcut;
    std::string m_address; // see Ports::address

    Mutex m_mutex;
    State m_state BLUEV_GUARDED_BY(m_mutex) = State::Idle;
    std::string m_message BLUEV_GUARDED_BY(m_mutex);
    std::string m_peerName BLUEV_GUARDED_BY(m_mutex);
    std::string m_peerAddress BLUEV_GUARDED_BY(m_mutex);
    std::vector<DeviceInfo> m_devices BLUEV_GUARDED_BY(m_mutex);
    StreamInfo m_stream BLUEV_GUARDED_BY(m_mutex);
    Pending m_pending BLUEV_GUARDED_BY(m_mutex);
    bool m_radioOn BLUEV_GUARDED_BY(m_mutex) = false;
    std::string m_adapterNode BLUEV_GUARDED_BY(m_mutex); // empty = internal

    std::atomic<bool> m_quit{false}; // "quit" was posted
    // set by the web threads, honoured by the radio thread
    std::atomic<bool> m_disconnectRequested{false};

    // radio thread only
    State m_radioState = State::Idle; // m_state without the lock
    BdAddr m_currentAddress{};        // the device of the session in progress
    bool m_hasCurrent = false;
    bool m_radioOpen = false; // m_radioOn without the lock
    bool m_serverUp = false;  // the HTTP server is listening

    HttpServer m_server;
};

} // namespace bluev
