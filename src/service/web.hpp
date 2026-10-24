// SPDX-License-Identifier: GPL-3.0-or-later
// A small HTTP server: the control page and the JSON API. It never touches
// the radio; it only calls the thread-safe parts of Service.
#pragma once

#include <atomic>
#include <condition_variable>
#include <string>
#include <string_view>
#include <thread>

#include "core/ports/log_reader.hpp"
#include "core/ports/usb_directory.hpp"
#include "service/sync.hpp"

namespace bluev
{

class Service;

// the range of ports the server tries
struct ServerPorts
{
    int firstPort = 7733;
    int lastPort = 7742;
};

class HttpServer
{
  public:
    using Options = ServerPorts;

    // `page`: the control page served at "/"
    HttpServer(Service& service, IUsbDirectory& usb, ILogReader& log, std::string_view page, Options options);
    ~HttpServer();

    // Listens on the first free port of the range. False if none is free.
    bool start();
    // Stops accepting, then waits for the requests being served. Safe to
    // call more than once.
    void stop() BLUEV_EXCLUDES(m_idleMutex);

    // The port in use; 0 while not listening.
    int port() const
    {
        return m_port.load();
    }
    // the port tried first (and what the console's home screen entry opens)
    static constexpr int defaultPort()
    {
        return 7733;
    }

    // Asks an older copy of this program, listening on this machine, to
    // quit. True if one answered. Other programs on nearby ports are left
    // alone: only a server that identifies itself as ours gets the request.
    static bool quitRunningInstance(Options options = {});

  private:
    struct Response
    {
        int code = 200;
        const char* contentType = "application/json";
        std::string body;
    };

    void acceptLoop();
    // Serves one connection on its own thread and closes it.
    void serve(int socketFd) BLUEV_EXCLUDES(m_idleMutex);
    // Gives the connection's slot back and wakes stop() when none is left.
    void finishConnection() BLUEV_EXCLUDES(m_idleMutex);
    // Takes a slot; false when kMaxConnections are in use (the caller answers 503).
    bool reserveConnection();
    // The answer to one request.
    Response route(std::string_view method, std::string_view path, std::string_view query);

    static constexpr int kMaxConnections = 8;

    Service& m_service;
    IUsbDirectory& m_usb;
    ILogReader& m_log;
    std::string_view m_page;
    Options m_options;
    int m_listenFd = -1;
    std::atomic<int> m_port{0};
    std::thread m_acceptThread;
    std::atomic<bool> m_stopping{false};

    // number of connections being served
    std::atomic<int> m_active{0};
    Mutex m_idleMutex;
    std::condition_variable m_idle;
};

} // namespace bluev
