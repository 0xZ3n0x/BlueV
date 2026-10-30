// SPDX-License-Identifier: GPL-3.0-or-later
// The composition root: builds the real objects of the console, wires them
// together and runs the service. Nothing else in the program knows which
// implementation sits behind an interface.
#include <chrono>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/thr.h>
#include <unistd.h>

#include "core/ports/clock.hpp"
#include "core/ports/log.hpp"
#include "core/radio.hpp"
#include "icon_png.h"
#include "platform/posix/file_bond_store.hpp"
#include "platform/posix/file_firmware_store.hpp"
#include "platform/posix/log_file.hpp"
#include "platform/posix/net_address.hpp"
#include "platform/posix/quit_signal.hpp"
#include "platform/ps5/avcap_source.hpp"
#include "platform/ps5/console_model.hpp"
#include "platform/ps5/notification.hpp"
#include "platform/ps5/process_identity.hpp"
#include "platform/ps5/ps5_system.hpp"
#include "platform/ps5/ps5_usb_directory.hpp"
#include "platform/ps5/ugen_transport.hpp"
#include "service/app_log.hpp"
#include "service/home_screen_shortcut.hpp"
#include "service/service.hpp"
#include "service/settings.hpp"
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
constexpr const char* kSettingsPath = "/data/bluev/settings";
// What this process calls itself (thr_set_name), and so what it looks for
// when checking whether a copy of it is already running.
constexpr const char* kProcessName = "bluev.elf";

// Takes the lock on `lockFd`, replacing an older copy if one is running, and
// serves until told to quit. 0 on a normal end, 1 when another copy keeps the
// lock.
int run(bluev::LogBuffer& buffer, int lockFd)
{
    // an exclusive lock, released when the file is closed or the process ends
    bool haveLock = flock(lockFd, LOCK_EX | LOCK_NB) == 0;

    if (!haveLock)
    {
        // The lock is taken by an older copy that is still serving: find it by
        // name (not by probing its web server, which may not be up yet) and ask
        // it to quit peacefully, then wait for it (it may be in the middle of a
        // connection attempt, or just finishing a stream).
        constexpr int kAttempts = 400;
        constexpr std::chrono::milliseconds kPause{250};
        const pid_t other = bluev::askOtherInstanceToQuit(kProcessName);

        if (other >= 0)
        {
            bluev::logLine("asked the running copy (pid %d) to quit", static_cast<int>(other));
            bluev::notify("BlueV: closing the running copy, starting the new one");
            for (int i = 0; i < kAttempts && !haveLock; i++)
            {
                std::this_thread::sleep_for(kPause);
                haveLock = flock(lockFd, LOCK_EX | LOCK_NB) == 0;
            }
        }
    }
    if (!haveLock)
    {
        bluev::logLine("another copy is already running");
        bluev::notify("BlueV: already running");
        return 1;
    }

    // Only now claim the name: an older copy is either gone or was never
    // there, so a lookup never has two processes under it to tell apart —
    // including this one, briefly, had it claimed the name before knowing it
    // would actually get to keep running.
    thr_set_name(-1, kProcessName); // -1: the calling thread

    bluev::AvcapSource capture;
    const int opened = capture.open(); // -1 failed, 0 not running yet, 1 running

    if (opened < 0)
    {
        bluev::notify("capture unavailable - see log");
        bluev::logLine("BlueV done");
        return 0;
    }

    bluev::SystemClock clock;
    bluev::UgenTransport transport(clock);
    bluev::FileBondStore bonds(kDirBase);
    bluev::FileFirmwareStore firmware(std::string(kDirBase) + "/firmware");
    bluev::Ps5UsbDirectory usb;
    bluev::Radio radio(transport, bonds, usb, firmware, capture, clock);
    bluev::Ps5AppInstaller installer;
    bluev::HomeScreenShortcut shortcut(installer,
                                       std::string_view(reinterpret_cast<const char*>(icon_png), icon_png_len));

    // The home screen entry that opens the web interface.
    shortcut.install(bluev::kDefaultPort);

    bluev::SettingsFile settings(kSettingsPath);
    bluev::Service service(radio, usb, shortcut, settings, buffer,
                           std::string_view(reinterpret_cast<const char*>(web_page), web_page_len),
                           bluev::consoleModelName(), bluev::localIpv4Address());

    service.run(opened == 1); // true = the capture is already running
    capture.close();
    bluev::logLine("BlueV done");
    return 0;
}

} // namespace

int main()
{
    // Must be armed before anything below runs: a copy racing to replace
    // us could send SIGTERM before we are ready to treat it as "quit
    // peacefully" rather than its default disposition (terminate at once).
    bluev::installQuitSignalHandling();

    bluev::LogBuffer buffer;
    bluev::LogFile file;
    bluev::AppLog log(buffer, [&file](const std::string& text) { file.append(text); }, bluev::sendNotification);

    bluev::setLogSink(&log);
    if (!file.open(kDirBase, kLogPath)) // no place to keep the log: nothing is started
    {
        return 1;
    }
    bluev::logLine("========================================");
    bluev::logLine("BlueV %s", BLUEV_VERSION);

    const int lockFd = open(kLockPath, O_CREAT | O_RDWR, 0644);
    const int status = run(buffer, lockFd);

    bluev::setLogSink(nullptr);
    if (lockFd >= 0)
    {
        close(lockFd); // releases the lock
    }
    return status;
}
