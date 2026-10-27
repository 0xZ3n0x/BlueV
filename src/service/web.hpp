// SPDX-License-Identifier: GPL-3.0-or-later
// A small HTTP server: the control page and the JSON API. It never touches
// the radio; it only calls the thread-safe parts of Service.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/ports/usb_directory.hpp"
#include "service/log_buffer.hpp"

namespace bluev
{

class Service;

// the port the server listens on, and what the console's home screen entry opens
constexpr int kDefaultPort = 7733;

class HttpServer
{
  public:
    // `page`: the control page served at "/"
    HttpServer(Service& service, IUsbDirectory& usb, LogBuffer& log, std::string_view page, int port);
    ~HttpServer();

    // Listens on the configured port. False if it is in use (the caller
    // keeps retrying; see Service::run()).
    bool start();
    // Stops accepting, then waits for the requests being served. Safe to
    // call more than once.
    void stop();

    // The port in use; 0 while not listening.
    int port() const
    {
        return m_port.load();
    }

  private:
    struct Response
    {
        int code = 200;
        const char* contentType = "application/json";
        std::string body;
    };

    void acceptLoop();
    // Serves one connection on its own thread and closes it.
    void serve(int socketFd);
    // Gives the connection's slot back and wakes stop() when none is left.
    void finishConnection();
    // Takes a slot; false when kMaxConnections are in use (the caller answers 503).
    bool reserveConnection();
    // The answer to one request.
    Response route(std::string_view method, std::string_view path, std::string_view query);

    static constexpr int kMaxConnections = 8;

    Service& m_service;
    IUsbDirectory& m_usb;
    LogBuffer& m_log;
    std::string_view m_page;
    int m_wantedPort;
    int m_listenFd = -1;
    std::atomic<int> m_port{0};
    std::thread m_acceptThread;
    std::atomic<bool> m_stopping{false};

    // number of connections being served
    std::atomic<int> m_active{0};
    std::mutex m_idleMutex;
    std::condition_variable m_idle;
};

} // namespace bluev
