# Design decisions

Choices made, and what was tried and not chosen.

## 13. Choices made for the PS5 Slim

- **Endpoint layout** read from the USB descriptors ([section 4](bluetooth.md#4-hci-transport-corebluetoothusb_hci_transportcpp-platformps5ugen_devicecpp)); a controller
  with slower timing gets longer waits; paging is not relied on, because the
  Slim's controller pages unreliably (devices mostly dial in themselves).
- **Service mode**: a persistent process with a web API and page, per-device
  bonds, automatic acceptance of paired devices; the user chooses the device.
- **AVDTP**: headset-driven setup ([section 9](audio.md#9-a2dp-setup-a2dpsessionstart-a2dpsessiononsignal)) and early inbound binding.
- **Flow control** thresholds ([section 7](bluetooth.md#7-eventacl-loop-and-l2cap-btstackpoll)).
- **Arithmetic**: remaining times are signed (an unsigned underflow once hung
  the program), the RSSI is signed, the SBC capability list is walked by length.

## 16. Design decisions

### 16.1 What we chose, and why

| Decision | Why |
|---|---|
| Ports and adapters: `app -> service -> core`, `platform -> core/ports` | Every layer runs on a PC under gtest/gmock; everything that touches the console is in `src/platform/ps5/`, and `core` has no console header |
| C++20, no exceptions, no RTTI (`-fno-exceptions -fno-rtti` on the console) | Small payload, no surprises from the console's runtime; the interfaces are header only, so tests may use RTTI |
| The stack does not sleep or read the clock itself: time comes from `IClock`, passes only inside `poll()` | Tests move a fake clock, so timeouts of 20 s run in milliseconds; the code still uses the same waits as on the console |
| One radio thread; web threads only post one pending command and read snapshots under a mutex | The HCI side is never touched from two threads; a second command gets `Busy` instead of a race |
| The radio is off at start; the user starts it from the page | Nothing opens a USB device until asked: the internal chip is shared with the gamepad ([section 12](shared-chip.md#12-the-controller-is-shared-with-the-console)) |
| A USB adapter is the recommended path | The chip is ours alone: no gamepad problem, 11 s from selecting a speaker to audio against about 105 s on the internal chip ([12b](shared-chip.md#12b-starting-the-radio)) |
| Endpoint layout read from the USB descriptors (`libSceUsbd`), not guessed by behaviour | Behaviour detection armed reads on the wrong pipes and broke the gamepad's reconnection ([12c](shared-chip.md#12c-what-the-usb-descriptors-say-libsceusbd-toolsusbprobe)) |
| A paired device that dials in is accepted; an unpaired one is refused and counted (`foreign_requests`) | The console is not pairable by whoever walks past; a speaker that reconnects by itself just works |
| Pairings are files, per adapter and device; names per device | The same speaker can be paired with the internal chip and with an adapter at once; plain files are easy to inspect and to test |
| AVDTP: wait for the device to start the procedure (1 s if we made the connection, 20 s if it did), then drive it ourselves | Headsets differ: the Sony WH-CH720N hangs up after about 8 s if the host does nothing, the WH-1000XM5 after 2-3 s (was 3 s here, 2026-10-25), the Creative speaker opens its channel later; starting at once collided with a device that drives (link timeout, 2026-10-18) |
| Flow control: at most 5 unconfirmed packets, a missing completion report counts as done after 31 ms | Limits of 3 packets and 100 ms cap the stream near 40 packets/s and dropped audio (`bt_stack.cpp`) |
| Streaming: as many SBC frames per packet as the MTU allows (at most 8), the queue cut to 50 ms, sending paced to the clock | Low delay without bursts; silence fills the gaps when the capture has nothing |
| SBC only, joint stereo preferred, bitpool 40 ("normal") or 53 ("high", chosen on the page) within the device's range, 48 kHz preferred, linear resampling to 44.1 kHz | Every audio device speaks SBC; the parameters are simple and were enough in the soak test ([12d](shared-chip.md#12d-soak-test-on-the-consoles-chip-2026-10-24)) |
| libsbc is an unmodified submodule; `src/compat/config.h` supplies its build settings | Upstream code stays upstream; it is not tested here |
| A small HTTP/1.0 server of our own, with a JSON writer; a thread per connection, at most 8, `503` when full | The API is a handful of routes; the connection slot is given back before the socket is closed so a client never meets a full server |
| One control page, embedded in the ELF (`tools/embed.py`) | Nothing else to deploy |
| Notifications are sent straight from the calling thread (`sceKernelSendNotificationRequest`) | There are only a few per session (start-up, "Streaming to ...", failures); a queue and a worker thread with a 400 ms hold were not worth it. If the console is found to drop messages sent close together, a queue can come back (git history) |
| Log: a ring of 300 lines for the page and a file flushed once a second, turned over at 512 KB; nothing in the kernel log; time stamps in UTC | The page can show the log without reading a file; the file survives a crash and cannot grow without limit; the console's kernel log stays readable |
| One copy at a time (`flock` on `/data/bluev/bluev.lock`); a newer copy finds the old one by process name (`thr_set_name`/`sysctl(KERN_PROC_PROC)`) and asks it to quit (`SIGTERM`) | Sending the ELF again replaces the running program; finding it by name works even before its web server is up, unlike probing it over HTTP |
| A fixed web server port (7733), not a range with a fallback | The home screen entry always names 7733; a fallback port made it silently stop matching. `start()` already retries every 2 s, which also covers the port being briefly held by something else |
| The home screen entry is registered through the system installer, with a marker file ([section 15](program.md#15-home-screen-entry)) | It is retried only when it was not accepted before |
| Host build with CMake (plain, thread and address sanitizer), console build with the Makefile | The two builds have different compilers and flags; the host build is where the tests live |
| `clang-format` (Microsoft style, pointers on the left, braces always) on everything but `third_party` | One style, no review noise |

### 16.2 What we tried or considered and did not choose

| Idea | Why not |
|---|---|
| Read the gamepad's state from the system (`scePadOpen`, libSceMbus) to open the radio only when no pad uses Bluetooth | `scePadOpen` is refused for our process (`0x809b0081`), Mbus answers nothing useful; getting past it means changing the process's credentials, judged too risky (12, [12a](shared-chip.md#12a-telling-whether-a-gamepad-uses-the-bluetooth-chip)) |
| Sniff the USB endpoints to learn the pad's state | Reading is what breaks the console's stack (12) |
| Survey the console's kernel structures to find who owns `/dev/bt` and `/dev/bluetooth_hid` | The survey crashed the console; no kernel memory probe again |
| Detach the console's kernel driver or take the chip over | Breaks the gamepad; ruled out |
| A run-time knob for the number of armed reads | Used for the measurements, then removed (12) |
| Detect the endpoint roles by behaviour on the console's chip | Replaced by the descriptors ([12c](shared-chip.md#12c-what-the-usb-descriptors-say-libsceusbd-toolsusbprobe)) |
| `libusb` for the HCI traffic | Works, with no advantage over ugen, and synchronous reads with timeouts split events ([12c](shared-chip.md#12c-what-the-usb-descriptors-say-libsceusbd-toolsusbprobe)) |
| Interface 3 of the console's chip as a second controller | It refuses the HCI commands ([12c](shared-chip.md#12c-what-the-usb-descriptors-say-libsceusbd-toolsusbprobe)) |
| Starting AVDTP ourselves at once to save the 9 s the speaker waits | Interferes when the speaker drives; see the AVDTP row above |
| Shorter page rounds and a longer listen window to speed up Connect | Noted and deliberately not applied: the speaker mostly connects to us by itself during the listen window (see 17) |
| The headset's battery over the hands-free profile (AT commands on RFCOMM, an audio gateway record in SDP) | Tried as a probe on the console (2026-10-30), then dropped. Of three headsets only the Redmi Buds opened the hands-free channel, and only when it connected to us by itself; it reported the battery as `AT+IPHONEACCEV` steps of 10. The WH-CH720N and the Creative speaker never searched for the record. Getting the battery from every headset would need us to open the channel ourselves (an SDP client and an RFCOMM initiator) with no promise that they answer |
| A launcher menu (an extension for the ps5-payload-websrv launcher) next to the page | It could only draw its own list (`showCarousel`), not HTML, so it did less than the page, and it needed `Access-Control-Allow-Origin: *` for its sandboxed frame. Removed: the page opens from the home screen entry and from any browser, and a payload manager starts the program |
| A confirmation window before starting the internal radio | Replaced by the warning text only, on the page |
| Compute the NIDs from the function names (SDK `nid_encode`) | Its implementation is not in the SDK libraries; the 8 NIDs stay as strings, each next to its name |
| Test `src/platform/ps5/` on the PC | It cannot run there; it is compiled by the console build and checked on the console, with the external adapter |
| Convert `tools/usbprobe` (C) to C++ | A separate diagnostic payload; not worth it |
