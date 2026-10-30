// SPDX-License-Identifier: GPL-3.0-or-later
#include "web.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/ports/log.hpp"
#include "service/json.hpp"
#include "service/service.hpp"

namespace bluev
{

namespace
{

// Connections the system holds for accept() while all kMaxConnections are busy
// or the accept thread is asleep; a browser opens several at once.
constexpr int kBacklog = 16;
constexpr std::size_t kMaxRequest = 2048;  // bytes of a request that are read; more is cut
constexpr std::size_t kMaxLogBytes = 8192; // most log text one /api/log answer carries

// Sends everything, or stops quietly when the client has gone.
void sendAll(int fd, const char* data, std::size_t size)
{
    while (size > 0)
    {
        ssize_t sent = send(fd, data, size, 0);

        if (sent <= 0)
        {
            return;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
}

// The status text for the codes this server uses.
const char* reason(int code)
{
    switch (code)
    {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 503:
        return "Service Unavailable";
    }
    return "Error";
}

// One whole HTTP/1.0 answer; the connection is closed after it.
void reply(int fd, int code, const char* contentType, std::string_view body)
{
    char head[512];
    const int length = std::snprintf(head, sizeof head,
                                     "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                     code, reason(code), contentType, body.size());

    sendAll(fd, head, static_cast<std::size_t>(length));
    sendAll(fd, body.data(), body.size());
}

// The body of every plain answer: {"ok":..., "message":...}
std::string message(bool ok, const char* text)
{
    Json json;

    json.beginObject().key("ok").value(ok).key("message").value(text).endObject();
    return json.take();
}

// Value of a query parameter, percent-decoded (a % that is not followed by two
// hex digits is kept as it is). Empty if absent.
std::string queryValue(std::string_view query, std::string_view key)
{
    while (!query.empty())
    {
        std::string_view pair = query.substr(0, query.find('&'));

        if (pair.size() > key.size() && pair.substr(0, key.size()) == key && pair[key.size()] == '=')
        {
            std::string_view raw = pair.substr(key.size() + 1);
            std::string value;

            for (std::size_t i = 0; i < raw.size(); i++)
            {
                if (raw[i] == '%' && i + 2 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(raw[i + 2])))
                {
                    char hex[3] = {raw[i + 1], raw[i + 2], 0};

                    value += static_cast<char>(std::strtol(hex, nullptr, 16));
                    i += 2;
                }
                else
                {
                    value += raw[i];
                }
            }
            return value;
        }
        const std::size_t next = query.find('&');

        if (next == std::string_view::npos)
        {
            break;
        }
        query.remove_prefix(next + 1);
    }
    return {};
}

// The answer to a command posted to the service.
HttpResponse postResponse(Service::PostResult result)
{
    HttpResponse response;

    switch (result)
    {
    case Service::PostResult::Ok:
        response.body = message(true, "ok");
        break;
    case Service::PostResult::Busy:
        response.code = 409;
        response.body = message(false, "busy");
        break;
    case Service::PostResult::BadAddress:
        response.code = 400;
        response.body = message(false, "bad address");
        break;
    case Service::PostResult::BadAdapter:
        response.code = 400;
        response.body = message(false, "adapter: internal, or external with dev=ugenX.Y");
        break;
    case Service::PostResult::BadValue:
        response.code = 400;
        response.body = message(false, "volume: a percentage, 0 to 100");
        break;
    case Service::PostResult::NotAllowed:
        response.code = 409;
        response.body = message(false, "not allowed now");
        break;
    case Service::PostResult::RadioOff:
        response.code = 409;
        response.body = message(false, "the radio is off: start it first");
        break;
    }
    return response;
}

} // namespace

HttpServer::HttpServer(Service& service, IUsbDirectory& usb, LogBuffer& log, std::string_view page, int port)
    : m_service(service), m_usb(usb), m_log(log), m_page(page), m_wantedPort(port)
{
}

HttpServer::~HttpServer()
{
    stop();
}

// Binds the configured port. Called again after a failure, which is how the
// server comes up once the network does (or once whatever was holding the
// port lets go of it).
bool HttpServer::start()
{
    sockaddr_in address{};
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        return false;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(m_wantedPort));
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(fd, kBacklog) != 0)
    {
        close(fd);
        return false;
    }
    m_listenFd = fd;
    m_port = m_wantedPort;
    m_stopping.store(false);
    m_acceptThread = std::thread([this] { acceptLoop(); });
    logLine("web: listening on port %d", m_port.load());
    return true;
}

void HttpServer::stop()
{
    if (m_listenFd < 0)
    {
        return;
    }
    m_stopping.store(true);
    if (m_acceptThread.joinable())
    {
        m_acceptThread.join();
    }
    close(m_listenFd);
    m_listenFd = -1;
    m_port = 0;

    // wait for the requests still being served (each has a 3 s timeout)
    std::unique_lock<std::mutex> lock(m_idleMutex);
    m_idle.wait_for(lock, std::chrono::seconds(5), [this] { return m_active.load() == 0; });
}

// Take a slot for a new connection; false when all are in use. A single
// compare-and-swap, so concurrent accepts cannot both pass the limit.
bool HttpServer::reserveConnection()
{
    int current = m_active.load();

    while (current < kMaxConnections)
    {
        if (m_active.compare_exchange_weak(current, current + 1))
        {
            return true;
        }
    }
    return false;
}

void HttpServer::finishConnection()
{
    // Notify with the lock held: stop() may destroy this object as soon as
    // it sees the count reach zero, so nothing may touch it afterwards.
    std::lock_guard<std::mutex> lock(m_idleMutex);
    m_active.fetch_sub(1);
    m_idle.notify_all();
}

// Runs on the accept thread until stop().
void HttpServer::acceptLoop()
{
    while (!m_stopping.load())
    {
        pollfd waiting{m_listenFd, POLLIN, 0};

        // wake up regularly to notice that we are stopping
        if (poll(&waiting, 1, 200) <= 0)
        {
            continue;
        }

        sockaddr_in peer{};
        socklen_t peer_length = sizeof peer;
        int fd = accept(m_listenFd, reinterpret_cast<sockaddr*>(&peer), &peer_length);

        if (fd < 0)
        {
            continue;
        }
        if (!reserveConnection())
        {
            reply(fd, 503, "application/json", message(false, "too many connections"));
            close(fd);
            continue;
        }
        // one short-lived thread per connection: browsers keep idle
        // sockets open and must not block the others
        std::thread([this, fd] { serve(fd); }).detach();
    }
}

void HttpServer::serve(int fd)
{
    timeval timeout{3, 0};
    std::string request;
    char chunk[512];

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    while (request.size() < kMaxRequest && request.find("\r\n\r\n") == std::string::npos)
    {
        ssize_t got = recv(fd, chunk, sizeof chunk, 0);

        if (got <= 0)
        {
            break;
        }
        request.append(chunk, static_cast<std::size_t>(got));
    }

    // "METHOD TARGET HTTP/1.x"
    const std::size_t method_end = request.find(' ');
    const std::size_t target_end = request.find(' ', method_end + 1);

    if (method_end == std::string::npos || target_end == std::string::npos)
    {
        reply(fd, 400, "application/json", message(false, "bad request"));
    }
    else
    {
        const std::string_view method(request.data(), method_end);
        std::string_view target(request.data() + method_end + 1, target_end - method_end - 1);
        std::string_view query;
        const std::size_t question = target.find('?');

        if (question != std::string_view::npos)
        {
            query = target.substr(question + 1);
            target = target.substr(0, question);
        }
        const HttpResponse response = route(method, target, query);

        reply(fd, response.code, response.contentType, response.body);
    }
    // The slot is given back before the socket is closed: a client sees the
    // end of the answer only at the close, so its next connection can never
    // find the slot still taken by this one.
    finishConnection();
    close(fd);
}

// the console's own chip is not looked at, only listed
std::string HttpServer::adaptersJson()
{
    const std::vector<UsbDevice> found = m_usb.adapters();
    Json json;

    json.beginObject().key("internal").value(true).key("external").beginArray();
    for (const UsbDevice& device : found)
    {
        json.beginObject()
            .key("device")
            .value(device.node)
            .key("vid")
            .value(static_cast<int>(device.vid))
            .key("pid")
            .value(static_cast<int>(device.pid))
            .key("class")
            .value(static_cast<int>(device.deviceClass))
            .key("subclass")
            .value(static_cast<int>(device.subClass))
            .key("protocol")
            .value(static_cast<int>(device.protocol))
            .key("product")
            .value(device.product)
            .key("vendor")
            .value(device.vendor)
            .endObject();
    }
    json.endArray().endObject();
    return json.take();
}

// the log from line `since` on, as {"next": ..., "text": ...}
std::string HttpServer::logJson(std::string_view query)
{
    const std::string since = queryValue(query, "since");
    unsigned from = since.empty() ? 0 : static_cast<unsigned>(std::strtoul(since.c_str(), nullptr, 10));
    unsigned next = 0;
    const std::string text = m_log.since(from, next, kMaxLogBytes);
    Json json;

    json.beginObject().key("next").value(next).key("text").value(text).endObject();
    return json.take();
}

// the page, /api/status, /api/devices, /api/adapters, /api/log
HttpResponse HttpServer::routeGet(std::string_view path, std::string_view query)
{
    HttpResponse response;

    if (path == "/" || path == "/index.html")
    {
        response.contentType = "text/html; charset=utf-8";
        response.body.assign(m_page);
    }
    else if (path == "/api/status")
    {
        response.body = m_service.statusJson();
    }
    else if (path == "/api/devices")
    {
        response.body = m_service.devicesJson();
    }
    else if (path == "/api/adapters")
    {
        response.body = adaptersJson();
    }
    else if (path == "/api/log")
    {
        response.body = logJson(query);
    }
    else
    {
        response.code = 404;
        response.body = message(false, "not found");
    }
    return response;
}

HttpResponse HttpServer::route(std::string_view method, std::string_view path, std::string_view query)
{
    if (method == "GET")
    {
        return routeGet(path, query);
    }
    if (method == "POST")
    {
        // /api/radio/on and /off, then every other /api/<command> goes to Service::post()
        if (path == "/api/radio/on")
        {
            return postResponse(m_service.startRadio(queryValue(query, "adapter"), queryValue(query, "dev")));
        }
        if (path == "/api/radio/off")
        {
            return postResponse(m_service.stopRadio());
        }
        if (path == "/api/volume")
        {
            return postResponse(m_service.setVolume(queryValue(query, "v")));
        }
        if (path.size() > 5 && path.substr(0, 5) == "/api/")
        {
            return postResponse(m_service.post(path.substr(5), queryValue(query, "addr")));
        }
    }

    HttpResponse response;

    response.code = 405;
    response.body = message(false, "method not allowed");
    return response;
}

} // namespace bluev
