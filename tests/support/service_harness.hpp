// SPDX-License-Identifier: GPL-3.0-or-later
// The service on a mock radio, listening on a local port, with helpers to talk
// HTTP to it.
#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>

#include "capture_sink.hpp"
#include "fake_stores.hpp"
#include "mock_radio.hpp"
#include "service/home_screen_shortcut.hpp"
#include "service/log_buffer.hpp"
#include "service/service.hpp"
#include "temp_dir.hpp"

namespace bluev::testing
{

constexpr const char* kSpeakerAddress = "00:11:22:AA:BB:CC";

class ServiceHarness : public ::testing::Test
{
  protected:
    using NiceRadio = ::testing::NiceMock<MockRadio>;
    using NiceInstaller = ::testing::NiceMock<MockInstaller>;

    ServiceHarness();
    ~ServiceHarness() override;

    // starts the service thread; false when the server did not come up
    bool startService(bool capturing = true);
    void stopService();

    // ---- HTTP ----
    int request(const std::string& method, const std::string& target, std::string& body);
    int rawRequest(const std::string& request, std::string& body);
    std::string fullAnswer(const std::string& request);
    std::string status();
    std::string stateName();
    bool waitForState(const char* state, int timeoutMs);
    bool waitFor(const std::function<bool()>& condition, int timeoutMs);

    static bool looksLikeJson(const std::string& text);

    // ---- what the fake radio does ----
    void setUpDefaultRadio();
    void simulateIncoming()
    {
        m_incoming.store(true);
    }

    CaptureSink sink;
    std::string address; // what the service shows as its network address; set before startService()
    LogBuffer log;
    FakeUsbDirectory usb;
    NiceRadio radio;
    NiceInstaller installer; // behind the home screen entry
    TempDir shortcutRoot;
    HomeScreenShortcut shortcut{installer, "icon", shortcutRoot.path()};
    std::string page = "<!doctype html><title>BlueV</title>";
    int port = 0;

    std::atomic<int> streamCalls{0};
    std::atomic<bool> lastStreamCapturing{false};
    std::atomic<int> streamingForMs{0}; // how long stream() blocks at most (0 = until a stop)
    std::atomic<bool> connectWorks{true};
    std::atomic<int> headsetVolume{-1}; // what the fake headset reports (raw, 0..127)

  private:
    static int nextPortBase();

    std::atomic<bool> m_incoming{false};
    std::atomic<bool> m_stopFlag{false};
    std::mutex m_tickMutex;
    std::function<void()> m_tick;
    StreamObserver m_observer;
    std::unique_ptr<Service> m_service;
    std::thread m_thread;
    int m_portBase = 0;
};

} // namespace bluev::testing
