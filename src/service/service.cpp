// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/service.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "core/bluetooth/address.hpp"
#include "core/ports/log.hpp"
#include "platform/posix/quit_signal.hpp"
#include "service/json.hpp"

namespace bluev
{

namespace
{

constexpr int kScanMilliseconds = 30000;
constexpr std::size_t kMaxDevices = 32;

bool validNode(std::string_view node)
{
    // "ugen" <bus> "." <address>
    if (node.size() < 7 || node.size() > 20 || node.substr(0, 4) != "ugen")
    {
        return false;
    }
    bool dot = false;
    for (std::size_t i = 4; i < node.size(); i++)
    {
        if (node[i] == '.' && !dot && i > 4 && i + 1 < node.size())
        {
            dot = true;
        }
        else if (node[i] < '0' || node[i] > '9')
        {
            return false;
        }
    }
    return dot;
}

} // namespace

Service::Service(IRadio& radio, IUsbDirectory& usb, HomeScreenShortcut& shortcut, LogBuffer& log, std::string_view page,
                 std::string model, std::string address, int port)
    : m_radio(radio), m_usb(usb), m_shortcut(shortcut), m_model(std::move(model)), m_address(std::move(address)),
      m_server(*this, usb, log, page, port)
{
    m_radio.setStreamObserver(
        [this](int rate, int queueMs, int pps, long packets) { publishStream(rate, queueMs, pps, packets); });
}

const char* Service::stateName(State state)
{
    switch (state)
    {
    case State::Idle:
        return "idle";
    case State::Scanning:
        return "scanning";
    case State::Connecting:
        return "connecting";
    case State::Streaming:
        return "streaming";
    }
    return "idle";
}

// ---------------------------------------------------------------------
// any thread
// ---------------------------------------------------------------------
Service::PostResult Service::post(std::string_view command, std::string_view address)
{
    Command wanted = Command::None;
    BdAddr parsed{};

    if (command == "scan")
    {
        wanted = Command::Scan;
    }
    else if (command == "connect")
    {
        wanted = Command::Connect;
    }
    else if (command == "forget")
    {
        wanted = Command::Forget;
    }
    else if (command == "disconnect")
    {
        m_disconnectRequested.store(true);
        return PostResult::Ok;
    }
    else if (command == "uninstall-shortcut")
    {
        // the console's home screen entry; the system's installer does the
        // work, nothing here touches the radio
        return m_shortcut.remove() ? PostResult::Ok : PostResult::NotAllowed;
    }
    else if (command == "quit")
    {
        m_quit.store(true);
        return PostResult::Ok;
    }
    else
    {
        return PostResult::NotAllowed;
    }

    if (wanted != Command::Scan)
    {
        if (address.empty() || !parseAddress(address, parsed))
        {
            return PostResult::BadAddress;
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.command != Command::None)
    {
        return PostResult::Busy;
    }
    if (m_state != State::Idle)
    {
        return PostResult::NotAllowed;
    }
    if (wanted != Command::Forget && !m_radioOn)
    {
        return PostResult::RadioOff;
    }
    m_pending.command = wanted;
    m_pending.address = parsed;
    return PostResult::Ok;
}

Service::PostResult Service::startRadio(std::string_view adapter, std::string_view node)
{
    std::string chosen;

    if (adapter == "external")
    {
        if (!validNode(node) || node == m_usb.internalNode())
        {
            return PostResult::BadAdapter;
        }
        chosen = std::string(node);
    }
    else if (adapter != "internal")
    {
        return PostResult::BadAdapter;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.command != Command::None)
    {
        return PostResult::Busy;
    }
    if (m_radioOn || m_state != State::Idle)
    {
        return PostResult::NotAllowed;
    }
    m_pending.command = Command::StartRadio;
    m_pending.node = chosen;
    return PostResult::Ok;
}

Service::PostResult Service::stopRadio()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.command != Command::None)
    {
        return PostResult::Busy;
    }
    if (!m_radioOn || m_state != State::Idle)
    {
        return PostResult::NotAllowed;
    }
    m_pending.command = Command::StopRadio;
    return PostResult::Ok;
}

std::string Service::statusJson()
{
    Json json;

    std::lock_guard<std::mutex> lock(m_mutex);
    const bool idle = m_state == State::Idle;

    json.beginObject()
        .key("state")
        .value(stateName(m_state))
        .key("message")
        .value(m_message)
        .key("peer")
        .beginObject()
        .key("name")
        .value(idle ? std::string_view() : std::string_view(m_peerName))
        .key("addr")
        .value(idle ? std::string_view() : std::string_view(m_peerAddress))
        .endObject()
        .key("stream")
        .beginObject()
        .key("rate")
        .value(m_stream.rate)
        .key("pps")
        .value(m_stream.packetsPerSecond)
        .key("queue_ms")
        .value(m_stream.queueMs)
        .key("packets")
        .value(m_stream.packets)
        .endObject()
        .key("radio")
        .beginObject()
        .key("on")
        .value(m_radioOn)
        .key("adapter")
        .value(m_adapterNode.empty() ? "internal" : "external")
        .key("device")
        .value(m_adapterNode)
        .key("foreign_requests")
        .value(m_radio.foreignRequests())
        .endObject()
        .endObject();
    return json.take();
}

std::string Service::devicesJson()
{
    Json json;

    std::lock_guard<std::mutex> lock(m_mutex);
    json.beginArray();
    for (const DeviceInfo& device : m_devices)
    {
        json.beginObject()
            .key("addr")
            .value(device.address)
            .key("name")
            .value(device.name)
            .key("rssi")
            .value(device.rssi)
            .key("cod")
            .value(device.cod)
            .key("bonded")
            .value(device.bonded)
            .key("connected")
            .value(device.connected)
            .key("other")
            .value(device.other)
            .endObject();
    }
    json.endArray();
    return json.take();
}

void Service::publishStream(int rate, int queueMs, int packetsPerSecond, long packets)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stream = StreamInfo{rate, queueMs, packetsPerSecond, packets};
}

// ---------------------------------------------------------------------
// radio thread
// ---------------------------------------------------------------------
void Service::setState(State state, const char* fmt, ...)
{
    char text[128];
    va_list args;

    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = state;
    m_message = text;
}

Service::Pending Service::takePending()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Pending taken = m_pending;
    m_pending = Pending{};
    return taken;
}

// Rebuilds the device list: scan results first, then paired devices that
// were not seen. A scan result without a name keeps the name saved with the
// bond.
void Service::publishDevices()
{
    std::vector<DeviceInfo> devices;

    for (const RadioDevice& found : m_radio.devices())
    {
        if (devices.size() >= kMaxDevices)
        {
            break;
        }
        DeviceInfo info;
        info.address = addressString(found.address);
        info.name = found.name;
        info.rssi = found.rssi;
        info.cod = found.classOfDevice;
        info.other = found.other;
        info.bonded = m_radio.hasBond(found.address);
        info.connected = m_current == found.address;
        devices.push_back(std::move(info));
    }

    const std::vector<BondEntry> bonds = m_radio.bonds();
    for (DeviceInfo& info : devices)
    {
        if (!info.name.empty())
        {
            continue;
        }
        for (const BondEntry& bond : bonds)
        {
            if (info.address == addressString(bond.peer))
            {
                info.name = bond.name;
            }
        }
    }
    for (const BondEntry& bond : bonds)
    {
        if (devices.size() >= kMaxDevices)
        {
            break;
        }
        const std::string address = addressString(bond.peer);
        bool seen = false;

        for (const DeviceInfo& info : devices)
        {
            seen = seen || info.address == address;
        }
        if (seen)
        {
            continue;
        }
        DeviceInfo info;
        info.address = address;
        info.name = bond.name;
        info.bonded = true;
        info.connected = m_current == bond.peer;
        devices.push_back(std::move(info));
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_devices = std::move(devices);
}

// A newer copy found us by name and asked us (SIGTERM) to step aside; take
// exactly the same, peaceful path "quit" from the web interface does: finish
// what is running, then stop. Checked in tick() and in run()'s loop: a session
// blocks the loop for as long as it runs, but tick() keeps being called from
// inside BtStack::poll(), which a stream calls too; and tick() is never
// called while idle with the radio off, which the loop iterates through.
bool Service::checkQuitSignal()
{
    if (!takeQuitRequestedFlag())
    {
        return false;
    }
    logLine("service: asked by a newer copy to quit");
    m_quit.store(true);
    return true;
}

// called by the radio's poll about once a second
void Service::tick()
{
    checkQuitSignal();
    flushLog();
    {
        const std::string name = m_radio.peerName();
        const std::string address = m_radio.peerAddress();
        std::lock_guard<std::mutex> lock(m_mutex);

        m_peerName = name;
        m_peerAddress = address;
    }
    const State state = currentState();

    if (state == State::Scanning || state == State::Idle)
    {
        publishDevices();
    }
    // a disconnect or quit during a session ends it; when no session is
    // running the request has nothing to stop and must not linger
    if (state != State::Idle && state != State::Scanning && (m_disconnectRequested.load() || m_quit.load()))
    {
        m_radio.requestStop();
    }
}

// Connects out (address) or takes over a link a paired device opened
// (address == nullptr), then streams until the device leaves.
void Service::runSession(bool capturing, const BdAddr* address)
{
    const char* failure = nullptr;

    m_disconnectRequested.store(false); // an old request does not end this session
    if (address != nullptr)
    {
        m_radio.resetAudio();
        m_current = *address;
        setState(State::Connecting, "connecting to %s", addressText(*address).c_str());
        if (!m_radio.connectTo(*address))
        {
            failure = "could not connect";
        }
    }
    else
    {
        setState(State::Connecting, "a paired device is connecting");
        if (!m_radio.adoptIncoming())
        {
            failure = "the link could not be secured";
        }
    }
    if (failure == nullptr)
    {
        setState(State::Connecting, "setting up audio");
        if (!m_radio.startAudio(address != nullptr))
        {
            failure = "audio setup failed";
            m_radio.stopAudio();
        }
    }
    if (failure == nullptr)
    {
        tick();
        setState(State::Streaming, "streaming to %s", m_radio.peerName().c_str());
        m_radio.stream(capturing);
        m_radio.stopAudio();
    }
    m_radio.disconnect();
    m_radio.resetAudio();
    m_radio.listenAudio();
    // a device we failed to pair with leaves no name file behind
    if (failure != nullptr && address != nullptr && !m_radio.hasBond(*address))
    {
        m_radio.forget(*address);
    }
    m_current.reset();
    publishStream(0, 0, 0, 0);
    setState(State::Idle, "%s", failure != nullptr ? failure : "stream ended");
    publishDevices();
}

Service::State Service::currentState()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

bool Service::radioIsOn()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_radioOn;
}

bool Service::openRadio(const std::string& node)
{
    UsbDevice usb;
    const char* which = node.empty() ? m_usb.internalNode() : node.c_str();
    const std::string& model = m_model;

    // what this is running on, for the log: a console of another model may
    // have another chip
    if (m_usb.nodeInfo(which, usb))
    {
        logLine("radio: console %s, %s adapter %s %04x:%04x (USB class %02x/%02x/%02x)",
                !model.empty() ? model.c_str() : "(model unknown)", node.empty() ? "internal" : "external", which,
                usb.vid, usb.pid, usb.deviceClass, usb.subClass, usb.protocol);
    }
    else
    {
        logLine("radio: console %s, %s adapter %s (no device information)",
                !model.empty() ? model.c_str() : "(model unknown)", node.empty() ? "internal" : "external", which);
    }

    setState(State::Connecting, "starting the %s Bluetooth", node.empty() ? "internal" : "external");
    if (!m_radio.open(node))
    {
        std::string why = m_radio.openError();

        if (why.empty())
        {
            why = node.empty() ? "the controller is busy" : "the adapter did not answer";
        }
        notify("%s", why.c_str());
        setState(State::Idle, "radio off: %s", why.c_str());
        return false;
    }
    m_radio.setTick([this] { tick(); });
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_radioOn = true;
        m_adapterNode = node;
    }
    setState(State::Idle, "ready");
    return true;
}

void Service::closeRadio()
{
    if (!radioIsOn())
    {
        return;
    }
    m_radio.close();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_radioOn = false;
    }
    setState(State::Idle, "radio off");
}

bool Service::run(bool capturing)
{
    m_serverUp = m_server.start();
    if (!m_serverUp)
    {
        logLine("web: could not start the HTTP server, will keep trying");
    }
    publishDevices();
    setState(State::Idle, "radio off");
    if (!m_address.empty() && m_server.port() > 0)
    {
        notify("BlueV ready: open http://%s:%d", m_address.c_str(), m_server.port());
    }
    else
    {
        notify("BlueV ready: open its entry on the home screen");
    }

    auto lastServerTry = std::chrono::steady_clock::now();

    while (!m_quit.load())
    {
        if (checkQuitSignal())
        {
            continue;
        }
        // started at boot, the network may not be up yet
        if (!m_serverUp && std::chrono::steady_clock::now() - lastServerTry > std::chrono::seconds(2))
        {
            lastServerTry = std::chrono::steady_clock::now();
            m_serverUp = m_server.start();
        }
        const Pending pending = takePending();

        switch (pending.command)
        {
        case Command::StartRadio:
            openRadio(pending.node);
            publishDevices();
            break;
        case Command::StopRadio:
            closeRadio();
            publishDevices();
            break;
        case Command::Scan:
            setState(State::Scanning, "scanning");
            m_radio.scan(kScanMilliseconds);
            publishDevices();
            setState(State::Idle, "scan finished");
            break;
        case Command::Connect:
            runSession(capturing, &pending.address);
            break;
        case Command::Forget:
            m_radio.forget(pending.address);
            publishDevices();
            break;
        case Command::None:
            if (!radioIsOn())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                break;
            }
            m_radio.poll(200);
            if (m_radio.linkUp())
            {
                runSession(capturing, nullptr);
            }
            break;
        }
    }
    logLine("service: stopping");
    if (m_serverUp)
    {
        m_server.stop();
    }
    closeRadio();
    return true;
}

} // namespace bluev
