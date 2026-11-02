// SPDX-License-Identifier: GPL-3.0-or-later
#include "service_harness.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>

namespace bluev::testing
{

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

int ServiceHarness::nextPortBase()
{
    static std::atomic<int> next{21000 + (static_cast<int>(::getpid()) % 50) * 20};
    return next.fetch_add(1);
}

ServiceHarness::ServiceHarness()
{
    m_portBase = nextPortBase();
    usb.devices.clear();
    setUpDefaultRadio();
}

ServiceHarness::~ServiceHarness()
{
    stopService();
}

void ServiceHarness::setUpDefaultRadio()
{
    ON_CALL(radio, open(_)).WillByDefault(Return(true));
    ON_CALL(radio, volume()).WillByDefault(Invoke([this] { return headsetVolume.load(); }));
    ON_CALL(radio, openError()).WillByDefault(Return(std::string()));
    ON_CALL(radio, setTick(_)).WillByDefault(Invoke([this](std::function<void()> tick) {
        std::lock_guard<std::mutex> l(m_tickMutex);
        m_tick = std::move(tick);
    }));
    ON_CALL(radio, setStreamObserver(_)).WillByDefault(Invoke([this](StreamObserver o) { m_observer = std::move(o); }));
    // like the real one: waits, and runs the once-a-second tick (faster here)
    ON_CALL(radio, poll(_)).WillByDefault(Invoke([this](std::uint32_t ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms < 20 ? 20 : ms / 4));
        std::function<void()> tick;
        {
            std::lock_guard<std::mutex> l(m_tickMutex);
            tick = m_tick;
        }
        if (tick)
        {
            tick();
        }
    }));
    ON_CALL(radio, scan(_)).WillByDefault(Invoke([this](std::uint32_t) {
        for (int i = 0; i < 5; i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            std::function<void()> tick;
            {
                std::lock_guard<std::mutex> l(m_tickMutex);
                tick = m_tick;
            }
            if (tick)
            {
                tick();
            }
        }
        return 2;
    }));
    ON_CALL(radio, devices()).WillByDefault(Invoke([] {
        std::vector<BtDevice> out;
        for (int i = 0; i < 2; i++)
        {
            BtDevice d;
            d.address = BdAddr{{static_cast<std::uint8_t>(i + 1), 0, 0, 0, 0, 0}};
            d.rssi = -50 - i;
            // the second one is called a TV, which the service marks as probably not audio
            d.name = (i == 1 ? "TV \"" : "Device \"") + std::to_string(i) + "\"\n";
            out.push_back(d);
        }
        return out;
    }));
    ON_CALL(radio, bonds()).WillByDefault(Return(std::vector<BondEntry>{}));
    ON_CALL(radio, hasBond(_)).WillByDefault(Return(false));
    ON_CALL(radio, foreignRequests()).WillByDefault(Return(0));
    ON_CALL(radio, linkUp()).WillByDefault(Invoke([this] { return m_incoming.exchange(false); }));
    ON_CALL(radio, connectTo(_)).WillByDefault(Invoke([this](const BdAddr&) { return connectWorks.load(); }));
    ON_CALL(radio, adoptIncoming()).WillByDefault(Return(true));
    ON_CALL(radio, peerName()).WillByDefault(Return(std::string("Fake Speaker")));
    ON_CALL(radio, peerAddress()).WillByDefault(Return(std::string("00:11:22:33:44:55")));
    ON_CALL(radio, startAudio(_)).WillByDefault(Return(true));
    ON_CALL(radio, resetAudio()).WillByDefault(Invoke([this] { m_stopFlag.store(false); }));
    ON_CALL(radio, requestStop()).WillByDefault(Invoke([this] { m_stopFlag.store(true); }));
    ON_CALL(radio, stream(_)).WillByDefault(Invoke([this](bool capturing) {
        streamCalls++;
        lastStreamCapturing.store(capturing);
        const auto start = std::chrono::steady_clock::now();
        long packets = 0;
        while (!m_stopFlag.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (m_observer)
            {
                m_observer(48000, 20, 49, ++packets);
            }
            std::function<void()> tick;
            {
                std::lock_guard<std::mutex> l(m_tickMutex);
                tick = m_tick;
            }
            if (tick && packets % 10 == 0)
            {
                tick();
            }
            const int limit = streamingForMs.load();
            if (limit > 0 && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(limit))
            {
                break;
            }
        }
        return packets > 0;
    }));
}

bool ServiceHarness::startService(bool capturing)
{
    m_service =
        std::make_unique<Service>(radio, usb, shortcut, settings, log, page, "CFI-2002 B01", address, m_portBase);
    m_thread = std::thread([this, capturing] { m_service->run(capturing); });
    for (int i = 0; i < 100 && port == 0; i++)
    {
        if (m_service->port() != 0)
        {
            port = m_service->port();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return port != 0;
}

void ServiceHarness::stopService()
{
    if (m_thread.joinable())
    {
        std::string body;

        if (port != 0)
        {
            request("POST", "/api/quit", body);
        }
        m_thread.join();
    }
    m_service.reset();
}

// ---- HTTP ----
int ServiceHarness::rawRequest(const std::string& text, std::string& body)
{
    const std::string answer = fullAnswer(text);
    int code = 0;

    body.clear();
    if (answer.compare(0, 9, "HTTP/1.0 ") == 0)
    {
        code = std::atoi(answer.c_str() + 9);
    }
    const std::size_t split = answer.find("\r\n\r\n");
    if (split != std::string::npos)
    {
        body = answer.substr(split + 4);
    }
    return code;
}

std::string ServiceHarness::fullAnswer(const std::string& text)
{
    timeval timeout{3, 0};
    sockaddr_in address{};
    std::string answer;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0)
    {
        char chunk[1024];
        ssize_t got;

        send(fd, text.data(), text.size(), MSG_NOSIGNAL);
        shutdown(fd, SHUT_WR);
        while ((got = recv(fd, chunk, sizeof chunk, 0)) > 0)
        {
            answer.append(chunk, static_cast<std::size_t>(got));
        }
    }
    close(fd);
    return answer;
}

int ServiceHarness::request(const std::string& method, const std::string& target, std::string& body)
{
    return rawRequest(method + " " + target + " HTTP/1.0\r\n\r\n", body);
}

std::string ServiceHarness::status()
{
    std::string body;
    request("GET", "/api/status", body);
    return body;
}

std::string ServiceHarness::stateName()
{
    const std::string body = status();
    const std::string key = "\"state\":\"";
    const std::size_t at = body.find(key);

    if (at == std::string::npos)
    {
        return "";
    }
    const std::size_t from = at + key.size();
    return body.substr(from, body.find('"', from) - from);
}

bool ServiceHarness::waitFor(const std::function<bool()>& condition, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 25)
    {
        if (condition())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return condition();
}

bool ServiceHarness::waitForState(const char* state, int timeoutMs)
{
    return waitFor([&] { return stateName() == state; }, timeoutMs);
}

// Balanced brackets, strings closed, no raw control characters in strings.
bool ServiceHarness::looksLikeJson(const std::string& text)
{
    int depth = 0;
    bool inString = false;

    if (text.empty() || (text[0] != '{' && text[0] != '['))
    {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); i++)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);

        if (inString)
        {
            if (c == '\\')
            {
                i++;
            }
            else if (c == '"')
            {
                inString = false;
            }
            else if (c < 0x20)
            {
                return false;
            }
        }
        else if (c == '"')
        {
            inString = true;
        }
        else if (c == '{' || c == '[')
        {
            depth++;
        }
        else if (c == '}' || c == ']')
        {
            if (--depth < 0)
            {
                return false;
            }
        }
    }
    return depth == 0 && !inString;
}

} // namespace bluev::testing
