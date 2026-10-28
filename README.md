<p align="center">
  <img src="homebrew/bluev/sce_sys/icon0.png" alt="BlueV" width="160">
</p>

<h1 align="center">BlueV</h1>

<h3 align="center">Bluetooth audio for PS5</h3>

<p align="center">
  Play your games through a Bluetooth speaker or headphones, controlled from a web page.
</p>

---

BlueV is a payload for jailbroken PS5 consoles. It takes the sound your console is
playing, encodes it, and sends it to a Bluetooth speaker or headphones. You control it
from a simple web page, on the console itself or from a phone or PC on the same network.

## Features

- Streams the console's audio to any Bluetooth speaker or headphones that supports A2DP (SBC)
- Works with a USB Bluetooth adapter or with the console's own Bluetooth chip
- Control page for scanning, connecting, pairing and disconnecting, also in the console's browser
- Remembers paired devices and accepts them when they connect by themselves
- A "BlueV" entry on the home screen that opens the control page
- Optional launcher entry for [ps5-payload-websrv](https://github.com/ps5-payload-dev/websrv)

## What you need

- A PS5 that runs payloads (for example with `elfldr` listening on port 9021)
- A Bluetooth speaker or headphones
- **Recommended:** a USB Bluetooth adapter. A CSR 4.0 adapter (`0a12:0001`) is tested.
  Realtek adapters such as the TP-Link UB600 (RTL8761BU) work with a firmware file
  on the console; see [Realtek adapters](#realtek-adapters).
  The console's own chip also works, but it is shared with your gamepad (see
  [Good to know](#good-to-know)).

## Install

1. Get `bluev.elf` (see [Build](#build)).
2. Send it to the console:

   ```sh
   nc --send-only <console-ip> 9021 < build/bluev.elf
   ```

   BlueV starts, shows a notification, and adds a **BlueV** entry to the home screen.
   Sending the file again replaces the running copy.

Optional launcher entry: run `make homebrew` and copy `build/homebrew/bluev/` to
`/data/homebrew/` on the console.

## Use

1. Open the control page: the **BlueV** entry on the home screen, or
   `http://<console-ip>:7733` from another device.
2. Under **Bluetooth adapter**, pick your USB adapter and press **Start**.
   (Using the internal chip? Read the warning on the page first.)
3. Put your speaker or headphones in pairing mode and press **Scan**.
4. Press **Connect** next to it. The first time takes a few seconds longer because it pairs.
5. Start a game. The sound comes out of the Bluetooth device.

Next time, switch the device on: paired devices connect by themselves. Use **Disconnect**
to end the stream, **Turn off** to give the adapter back, **Stop BlueV** to quit.

## Good to know

- **Internal chip and your gamepad.** The console's own chip is shared with the system's
  Bluetooth, which your gamepad uses. While the internal radio is on, switching the gamepad
  off and on can leave it connected but without input until the console restarts.
  Switch the gamepad on **before** you press Start and leave it alone. A USB adapter does
  not have this problem.
- Connecting takes about 10 seconds with a USB adapter and can take over a minute with
  the internal chip.
- The control page has no password. Anyone on your network can open it, so use it on a
  network you trust.
- Devices that are probably not audio devices (TVs and the like) are marked in the list.
- BlueV uses SBC, the codec every Bluetooth audio device supports. It does not send
  microphone audio.
- Tested on a PS5 Slim only. With a USB adapter, a Creative Stage Air V2 and Sony WH-CH720N
  headphones work. With the internal chip the Creative works, but the WH-CH720N could not be
  made to work (see [Contributing](#contributing)).

## Realtek adapters

Realtek USB adapters (the RTL8761BU in the UB600, the UB500, and other Realtek
sticks) start in a ROM mode that answers Bluetooth commands and appears in the
adapter list, but the radio cannot scan. BlueV uploads the missing patch, the
same way Linux's `btrtl` does, from files you put on the console:

```text
/data/bluev/firmware/rtl8761bu_fw.bin
/data/bluev/firmware/rtl8761bu_config.bin      (optional)
```

Copy them with your usual file-transfer payload (create `/data/bluev/firmware/`
first if your tool does not). The files are in the
`linux-firmware` package (`/lib/firmware/rtl_bt/` on Linux, `firmware-realtek` on
Debian and Ubuntu), or in the
[`rtl_bt` tree of linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/rtl_bt).
The Windows driver package from TP-Link works too: BlueV accepts both the
`Realtech` and the `RTBTCore` file formats, as long as the file is named
`rtl8761bu_fw.bin`.

If the file is missing, **Start** stops with
`this adapter needs firmware: put rtl8761bu_fw.bin in /data/bluev/firmware/`.
The patch is uploaded whenever the chip is back in ROM mode (after a console
power cycle, or when the adapter is plugged in). A Realtek chip that already
carries a vendor patch is put back into ROM mode first, the way Linux's `btrtl`
does, and the file is uploaded again. A file meant for another chip is refused
(the log names the one this chip wants). Other Realtek chips use their own file
(for example `rtl8761cu_fw.bin`, `rtl8821cu_fw.bin`); the error names the one
the adapter wants.

Only the RTL8761BU (the UB600) has been tested on hardware. The loader also says
it knows the other USB chips in Linux's `btrtl` table, but they are untested: in
particular the secure-boot parts (RTL8852AU/BU/CU, RTL8851BU, RTL8922AU,
RTL8852BTU) take their patch from the key-id security sections, a path that has
not been run on a console yet.

Only Realtek is supported so far. Broadcom, Intel and MediaTek dongles that need
a firmware patch are not handled yet; the ones whose ROM works (such as the ASUS
BT400) are left alone.

## Troubleshooting

- **Nothing in the list:** the device must be in pairing mode, and close to the console.
  A Realtek adapter that finds nothing while its firmware is missing is explained in
  [Realtek adapters](#realtek-adapters); **Start** reports the exact file it wants.
- **No sound:** check that the page shows *Streaming*, and that the game is making sound
  (BlueV streams what the console plays).
- **Logs:** the *Log* card on the control page, or the file `/data/bluev/bluev.log` on the
  console. The file is kept small: at 512 KB it becomes `bluev.log.1` (replacing the one
  before), so the two files together stay below 1 MB. BlueV does not write to the console's
  kernel log.
- **Pairings** are kept in `/data/bluev/`. Use **Forget** on the page to remove one.

## Contributing

BlueV was written and tested by one person on one console, so tests on other hardware are the
most useful thing you can send. Please open an issue or a pull request with what you found,
including the log (the *Log* card, or `/data/bluev/bluev.log`).

**Try it on other consoles.** I only have a **PS5 Slim**. If you have a **PS5 Pro** or the
original **fat PS5**, please try it and report what happens: does the radio start, which
endpoints does the log show, does a speaker connect and stream? The first lines of the log
(console model, adapter, endpoints) are the interesting part.

**Try other Bluetooth adapters.** The USB adapter tested here is a CSR 4.0
(`0a12:0001`). The Realtek path is new (see [Realtek adapters](#realtek-adapters));
reports for the UB600 and other Realtek sticks are welcome. Results with other brands
are welcome too, including the ones that do not work. Only the Realtek family has a
firmware uploader so far; Broadcom (`.hcd`), Intel (`.sfi`/`.ddc`) and MediaTek dongles
that boot from ROM are not handled yet.

**Try other speakers and headphones, especially on the internal chip.** With the internal
chip the Creative Stage Air V2 works, but I could not get the Sony WH-CH720N to work. It
works with a USB adapter. If you can test it, or other headphones, on the internal chip, please
share the log.

**Improve the internal chip support.** The internal chip is shared with the console's own
Bluetooth stack, which makes it the hard part (see [`src/ARCHITECTURE.md`](src/ARCHITECTURE.md),
section 12, for what is known and what was tried). Ideas and fixes there are very welcome.
I will review every pull request.

**Code changes.** The tests run on a PC (`tests/run.sh`) and the code is formatted with
`clang-format`. Please add a test for new behaviour (the fakes in `tests/support/` make a
Bluetooth controller and a speaker easy to script) and keep `third_party/` untouched. Open
work is listed in section 18 of `src/ARCHITECTURE.md`. Please do not send changes that read
or write the console's kernel memory.

## Build

Requirements: the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)
(expected in `/opt/ps5-payload-sdk`, or set `TOOLCHAIN=<path>/bin`), `git` and `python3`.

```sh
git clone --recurse-submodules <this repository>
cd <repository>
make              # build/bluev.elf
make homebrew     # the launcher entry in build/homebrew/bluev/
make release      # build/release/bluev.elf and its .sha256, for a release (see below)
```

`make release` makes the file to publish: optimised, without the symbol table and the
compiler note, and with the paths of the machine that built it taken out of the binary (the
build fails if one is left). Run it from a clean checkout of the tag (`make clean release`).

Tests run on a PC, on x86-64 or arm64 (they need `clang++`, `cmake` and `ninja`):

```sh
tests/run.sh              # all tests
tests/run.sh address      # with AddressSanitizer (also: thread)
```

or with the CMake presets `debug`, `release`, `asan` and `tsan` (build in `build-host/<preset>`):

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

## For developers

How BlueV works, how the code is organised and what was learned about the console's
Bluetooth chip is in [`src/ARCHITECTURE.md`](src/ARCHITECTURE.md).

| Folder | What is in it |
|---|---|
| `src/` | the program: `core` (Bluetooth and audio, no console code), `service` (web server and state), `platform` (the console and the file system), `app` (`main`) |
| `tests/` | unit, protocol (fake controller and speaker) and web API tests |
| `web/` | the control page |
| `homebrew/` | the launcher entry and the icon |
| `tools/` | `usbprobe`, a USB diagnostic payload |
| `third_party/` | libsbc and googletest (git submodules) |

## Legal notes

(General information, not legal advice.)

- BlueV is not affiliated with or endorsed by Sony Interactive Entertainment. PlayStation is a
  trademark of Sony Interactive Entertainment Inc.
- BlueV contains no exploit and no copy protection circumvention. It needs a console on which you
  already run payloads.
- This repository contains no Sony code, binaries or decrypted system libraries. It has the
  names of system functions, their NIDs, file paths and the data layouts the program needs, found
  by running software on a console, to make it work with other Bluetooth devices.
- Running homebrew may go against Sony's terms for your console, and what the law says about it
  differs from country to country. Check what applies to you; use is at your own risk.

## License

Copyright (C) 2026 0xZ3n0x

BlueV is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of
the License, or (at your option) any later version. See [LICENSE](LICENSE). It is distributed
in the hope that it will be useful, but without any warranty.

It is under the GPL because of the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)
that builds it: every payload built with it contains the SDK's start-up code, and its headers
are GPL-3.0-or-later. BlueV itself includes `ps5/kernel.h` (`src/platform/ps5/ps5_api.hpp`)
and calls the SDK's `kernel_dynlib_*` functions (`avcap_source.cpp`, `ps5_system.cpp`,
`ps5_usb_directory.cpp`). The third-party parts keep their own licenses, which work with the GPL:

- `third_party/sbc` (libsbc from BlueZ) is LGPL-2.1-or-later, built unmodified
- `third_party/googletest` is BSD-3-Clause and is only used for the tests
- the FreeBSD USB headers the SDK ships (`dev/usb/*.h`, `libusb.h`) are BSD licensed

## Credits

- BlueV started as an investigation of
  [ghost-toothAPI](https://gbatemp.net/threads/ghosttoothapi-custom-payload-framework-for-bluetooth-audio-and-bluetooth-gamepads.684768/),
  a closed-source PS5 payload that sends the console's audio to Bluetooth devices, because it
  did not work on my own PS5. Its author published only the binary, without source code or a
  license. BlueV was then written again from the beginning, inspired by that project: the
  structure, the web interface, USB adapter support, the logging and the tests are new, but the
  Bluetooth and A2DP logic follows what the investigation showed. Thanks to its author for
  showing that this is possible. If you are the author and have questions or concerns, please
  open an issue.
- [libsbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git) from BlueZ (LGPL), used unmodified
- [ps5-payload-sdk](https://github.com/ps5-payload-dev) and
  [ps5-payload-websrv](https://github.com/ps5-payload-dev/websrv)
- [GoogleTest](https://github.com/google/googletest) for the tests

BlueV is not affiliated with or endorsed by Sony Interactive Entertainment. PlayStation is
a trademark of Sony Interactive Entertainment Inc. Use it at your own risk.
