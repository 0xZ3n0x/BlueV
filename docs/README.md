# BlueV architecture

Developer documentation: how the program is built and what was learned about the
console. To install and use BlueV, see the [README](../README.md).

A PS5 payload that captures the console's audio mix (the mix Remote Play
uses), encodes it with SBC and streams it over A2DP to a Bluetooth headset or
speaker through the console's USB Bluetooth controller. It does not touch the
system's own Bluetooth stack (DualSense etc.); it shares the controller with it
and talks raw HCI.

It runs on the PS5 Slim as a web-driven service: a long-running process that
the user controls from a web page (sections [1](program.md#1-top-level-appmaincpp-serviceservicecpp) and [2](program.md#2-web-interface-servicewebcpp-webindexhtml)).

The whole program is C++20 (no exceptions, no RTTI), organised as ports and
adapters so that every layer runs on a PC under gtest/gmock. Naming: classes
`PascalCase`, methods `camelCase`, members `m_snake_case`, namespace `bluev`.
Dependency rule: `app -> service -> core`; `platform -> core/ports`; `core`
never includes `platform` or `service`. Why it is built this way, and what was
tried and dropped, is in sections [16](design.md#16-design-decisions) and [17](todo.md#17-todo).

| Path | Role |
|---|---|
| `src/app/main.cpp` | composition root: lock file, builds the real objects, runs the service |
| `src/core/ports/` | interfaces: `IClock`, `ILogSink`, `IHciTransport`, `IUsbDevice`, `IBondStore`, `IFirmwareStore`, `IAudioSource`, `IAvcap2`, `IUsbDirectory`, `IAppInstaller`. An interface exists only where a test needs a fake of something it cannot run (the controller, the console, time, files); the rest are plain classes or `std::function` |
| `src/core/bluetooth/bt_stack*.cpp` | `BtStack`: controller start/stop, inquiry, paging/pairing/encryption, event and ACL loop, L2CAP, credit flow control, bonds |
| `src/core/bluetooth/` (rest) | `UsbHciTransport` (HCI over USB: the reads, commands and queues, on top of `IUsbDevice`), `HciPacketQueue`, `handleSdpRequest` (SDP), addresses, names, `device_filter` (TV classification, EIR names), `L2capReassembler`, HCI opcodes |
| `src/core/audio/` | `A2dpSession` (AVDTP), `A2dpStreamer` (capture, resample, SBC, RTP), `Avrcp` (the headset's volume, [section 9a](audio.md#9a-the-headsets-volume-avrcp-coreaudioavrcp)), `avdtp`, `a2dp_params`, `PcmFifo`, `LinearResampler`, `rtp.hpp` |
| `src/core/radio.*` | `IRadio` (what the service needs) and `Radio` (wires `BtStack` + `A2dpSession` + `Avrcp` + `A2dpStreamer`) |
| `src/service/service.*` | `Service`: state machine driven by the web interface; depends on `IRadio`, `IUsbDirectory` and the concrete `HomeScreenShortcut` / `LogBuffer` |
| `src/service/web.*` | `HttpServer`: control page and JSON API |
| `src/service/log_buffer.*`, `app_log.*` | log ring + file buffer, the program's `ILogSink` (also sends the on-screen notifications) |
| `src/service/home_screen_shortcut.*` | the home screen entry (files + `IAppInstaller`) |
| `src/service/json.hpp` | JSON writer |
| `src/platform/ps5/` | the only code that knows the console: `UgenDevice` (the ugen USB_FS ioctls), `Ps5UsbDirectory` (the USB device list + libSceUsbd descriptors), `AvcapLibrary` (libSceAvcap2), `Ps5AppInstaller`, `askOtherInstanceToQuit` (process list lookup), `consoleModelName()`, `sendNotification()` |
| `src/platform/posix/` | `FileBondStore`, `LogFile` (plain files; also used by the tests) |
| `web/index.html` | the control page, embedded at build time (`tools/embed.py`) |
| `third_party/sbc/` | BlueZ libsbc 2.2, git submodule, built unmodified (not tested) |
| `src/compat/config.h` | build settings for libsbc (force-included) |
| `tests/` | gtest/gmock: `unit/`, `protocol/` (fake controller + fake speaker + fake audio), `service/` (mock radio, real HTTP); `tests/run.sh [plain|thread|address] [filter]` |

Build for the console: `git submodule update --init` (libsbc), then `make`. Host tests: `tests/run.sh`. libsbc is the
upstream release 2.2 without changes; `src/compat/config.h` only supplies the
build settings. The console build uses libsbc's MMX and SSE paths; the host tests
use its plain C path on every CPU (x86-64 and arm64), which gives the same frames
byte for byte.

## Where to read

| File | What is in it |
|---|---|
| [The program](program.md) | How the program starts, its web interface, logging, threads and the home screen entry. |
| [Bluetooth stack](bluetooth.md) | The USB transport, controller setup, connections, L2CAP and SDP. |
| [Adapter firmware](adapter-firmware.md) | Uploading the firmware some USB adapters need. |
| [Audio](audio.md) | Capture, A2DP setup, the headset volume and the streaming loop. |
| [The console's chip](shared-chip.md) | What sharing the chip with the console means, and what was tried and measured. |
| [Design decisions](design.md) | Choices made, and what was tried and not chosen. |
| [TODO](todo.md) | Open work. |
