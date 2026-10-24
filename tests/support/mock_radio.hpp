// SPDX-License-Identifier: GPL-3.0-or-later
// A gmock radio for the service tests.
#pragma once

#include <gmock/gmock.h>

#include "core/ports/app_installer.hpp"
#include "core/ports/shortcut.hpp"
#include "core/ports/system_info.hpp"
#include "core/radio.hpp"

namespace bluev::testing
{

class MockRadio : public IRadio
{
  public:
    MOCK_METHOD(bool, open, (const std::string& node), (override));
    MOCK_METHOD(std::string, openError, (), (override));
    MOCK_METHOD(void, close, (), (override));
    MOCK_METHOD(void, setTick, (std::function<void()> tick), (override));
    MOCK_METHOD(void, setStreamObserver, (IStreamObserver * observer), (override));
    MOCK_METHOD(void, poll, (std::uint32_t timeoutMs), (override));
    MOCK_METHOD(int, scan, (std::uint32_t timeoutMs), (override));
    MOCK_METHOD(std::vector<RadioDevice>, devices, (), (override));
    MOCK_METHOD(std::vector<BondEntry>, bonds, (), (override));
    MOCK_METHOD(bool, hasBond, (const BdAddr& peer), (override));
    MOCK_METHOD(void, forget, (const BdAddr& peer), (override));
    MOCK_METHOD(long, foreignRequests, (), (override));
    MOCK_METHOD(bool, linkUp, (), (override));
    MOCK_METHOD(bool, connectTo, (const BdAddr& peer), (override));
    MOCK_METHOD(bool, adoptIncoming, (), (override));
    MOCK_METHOD(void, disconnect, (), (override));
    MOCK_METHOD(std::string, peerName, (), (override));
    MOCK_METHOD(std::string, peerAddress, (), (override));
    MOCK_METHOD(void, resetAudio, (), (override));
    MOCK_METHOD(void, listenAudio, (), (override));
    MOCK_METHOD(bool, startAudio, (bool initiatedByUs), (override));
    MOCK_METHOD(void, stopAudio, (), (override));
    MOCK_METHOD(bool, stream, (bool capturing), (override));
    MOCK_METHOD(void, requestStop, (), (override));
};

class MockInstaller : public IAppInstaller
{
  public:
    MOCK_METHOD(bool, registerTitle, (const std::string& titleId, const std::string& root), (override));
    MOCK_METHOD(bool, unregisterTitle, (const std::string& titleId), (override));
};

class MockShortcut : public IShortcut
{
  public:
    MOCK_METHOD(bool, install, (int port), (override));
    MOCK_METHOD(bool, remove, (), (override));
};

class FakeSystemInfo : public ISystemInfo
{
  public:
    std::string modelName() override
    {
        return "CFI-2002 B01";
    }
};

} // namespace bluev::testing
