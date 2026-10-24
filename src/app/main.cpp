// SPDX-License-Identifier: GPL-3.0-or-later
// The composition root: builds the real objects of the console, wires them
// together and runs the service. Nothing else in the program knows which
// implementation sits behind an interface.
#include <chrono>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "core/ports/clock.hpp"
#include "core/ports/log.hpp"
#include "core/radio.hpp"
#include "icon_png.h"
#include "platform/posix/file_bond_store.hpp"
#include "platform/posix/log_file.hpp"
#include "platform/posix/net_address.hpp"
#include "platform/ps5/avcap_source.hpp"
#include "platform/ps5/ps5_system.hpp"
#include "platform/ps5/ps5_usb_directory.hpp"
#include "platform/ps5/ugen_transport.hpp"
#include "service/app_log.hpp"
#include "service/home_screen_shortcut.hpp"
#include "service/service.hpp"
#include "web_page.h"

// The version comes from the VERSION file, through the build.
#ifndef BLUEV_VERSION
#define BLUEV_VERSION "unknown"
#endif

namespace
{

constexpr const char* kDirBase = "/data/bluev";
constexpr const char* kLogPath = "/data/bluev/bluev.log";
constexpr const char* kLockPath = "/data/bluev/bluev.lock";

// An exclusive lock on a file, released when the object goes away (or when
// the process ends), so that only one copy runs.
class LockFile
{
  public:
    explicit LockFile(const char* path) : m_fd(open(path, O_CREAT | O_RDWR, 0644))
    {
    }

    ~LockFile()
    {
        if (m_fd >= 0)
        {
            close(m_fd); // closing releases the lock
        }
    }

    LockFile(const LockFile&) = delete;
    LockFile& operator=(const LockFile&) = delete;

    bool tryLock()
    {
        return m_fd >= 0 && flock(m_fd, LOCK_EX | LOCK_NB) == 0;
    }

  private:
    int m_fd;
};

// The lock is taken by an older copy that is still serving: ask it to quit
// and wait for it (it may be in the middle of a connection attempt).
bool takeOverFromRunningCopy(LockFile& lock)
{
    constexpr int kAttempts = 400;
    constexpr std::chrono::milliseconds kPause{250};

    if (!bluev::HttpServer::quitRunningInstance())
    {
        return false;
    }
    bluev::logLine("asked the running copy to quit");
    for (int i = 0; i < kAttempts; i++)
    {
        std::this_thread::sleep_for(kPause);
        if (lock.tryLock())
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    bluev::LogBuffer buffer;
    bluev::LogFile file;
    bluev::NotificationQueue notifications(bluev::sendNotification);
    bluev::AppLog log(buffer, notifications, [&file](const std::string& text) { file.append(text); });

    bluev::setLogSink(&log);
    if (!file.open(kDirBase, kLogPath)) // no place to keep the log: nothing is started
    {
        return 1;
    }
    notifications.start();
    bluev::logLine("========================================");
    bluev::logLine("BlueV %s", BLUEV_VERSION);

    LockFile lock(kLockPath);
    int status = 1;

    if (lock.tryLock() || takeOverFromRunningCopy(lock))
    {
        bluev::AvcapSource capture;
        const int opened = capture.open(); // -1 failed, 0 not running yet, 1 running
        if (opened < 0)
        {
            bluev::notify("capture unavailable - see log");
        }
        else
        {
            bluev::SystemClock clock;
            bluev::UgenTransport transport(clock);
            bluev::FileBondStore bonds(kDirBase);
            bluev::Ps5UsbDirectory usb;
            bluev::Radio radio(transport, bonds, usb, capture, clock);
            bluev::Ps5SystemInfo system;
            bluev::Ps5AppInstaller installer;
            bluev::HomeScreenShortcut shortcut(installer,
                                               std::string_view(reinterpret_cast<const char*>(icon_png), icon_png_len));

            // The home screen entry that opens the web interface. It always names
            // the first port; when another program holds that one the server moves
            // on and the entry no longer matches.
            shortcut.install(bluev::HttpServer::defaultPort());

            bluev::Service service({radio, usb, system, shortcut, buffer,
                                    std::string_view(reinterpret_cast<const char*>(web_page), web_page_len),
                                    bluev::localIpv4Address()});
            service.run(opened == 1); // true = the capture is already running
            capture.close();
        }
        bluev::logLine("BlueV done");
        status = 0;
    }
    else
    {
        bluev::logLine("another copy is already running");
        bluev::notify("BlueV: already running");
    }

    notifications.stop();
    bluev::setLogSink(nullptr);
    return status;
}
