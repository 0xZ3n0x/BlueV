# BlueV — architecture

Developer documentation: how the program is built and what was learned about the
console. To install and use BlueV, see the [README](../README.md).

A PS5 payload that captures the console's audio mix (the mix Remote Play
uses), encodes it with SBC and streams it over A2DP to a Bluetooth headset or
speaker through the console's USB Bluetooth controller. It does not touch the
system's own Bluetooth stack (DualSense etc.); it shares the controller with it
and talks raw HCI.

It runs on the PS5 Slim as a web-driven service: a long-running process that
the user controls from a web page (sections 1 and 2).

The whole program is C++20 (no exceptions, no RTTI), organised as ports and
adapters so that every layer runs on a PC under gtest/gmock. Naming: classes
`PascalCase`, methods `camelCase`, members `m_snake_case`, namespace `bluev`.
Dependency rule: `app -> service -> core`; `platform -> core/ports`; `core`
never includes `platform` or `service`. Why it is built this way, and what was
tried and dropped, is in sections 17 and 18.

| Path | Role |
|---|---|
| `src/app/main.cpp` | composition root: lock file, builds the real objects, runs the service |
| `src/core/ports/` | interfaces: `IClock`, `ILogSink`, `IHciTransport`, `IBondStore`, `IAudioSource`, `IUsbDirectory`, `IAppInstaller`. An interface exists only where a test needs a fake of something it cannot run (the controller, the console, time, files); the rest are plain classes or `std::function` |
| `src/core/bluetooth/bt_stack*.cpp` | `BtStack`: controller start/stop, inquiry, paging/pairing/encryption, event and ACL loop, L2CAP, credit flow control, bonds |
| `src/core/bluetooth/` (rest) | `handleSdpRequest` (SDP), addresses, names, `device_filter` (TV classification, EIR names), `L2capReassembler`, HCI opcodes |
| `src/core/audio/` | `A2dpSession` (AVDTP), `A2dpStreamer` (capture, resample, SBC, RTP), `avdtp`, `a2dp_params`, `PcmFifo`, `LinearResampler`, `rtp.hpp` |
| `src/core/radio.*` | `IRadio` (what the service needs) and `Radio` (wires `BtStack` + `A2dpSession` + `A2dpStreamer`) |
| `src/service/service.*` | `Service`: state machine driven by the web interface; depends on `IRadio`, `IUsbDirectory` and the concrete `HomeScreenShortcut` / `LogBuffer` |
| `src/service/web.*` | `HttpServer`: control page and JSON API |
| `src/service/log_buffer.*`, `app_log.*` | log ring + file buffer, the program's `ILogSink` (also sends the on-screen notifications) |
| `src/service/home_screen_shortcut.*` | the home screen entry (files + `IAppInstaller`) |
| `src/service/json.hpp` | JSON writer |
| `src/platform/ps5/` | the only code that knows the console: `UgenTransport` (raw HCI over ugen), `Ps5UsbDirectory` (the USB device list + libSceUsbd descriptors), `AvcapSource` (libSceAvcap2), `Ps5AppInstaller`, `askOtherInstanceToQuit` (process list lookup), `consoleModelName()`, `sendNotification()` |
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

## 1. Top level (`app/main.cpp`, `service/service.cpp`)

```
log file -> lock (flock)
  -> AvcapSource::open   prepare audio capture
  -> Service::run
       HttpServer::start HTTP server thread
       loop: idle / scanning / connecting / streaming, driven by commands
             and by headsets dialing in; the radio (BtStack::start) is
             opened when the user starts it (section 12b)
  -> AvcapSource::close
```

- `/data/bluev/bluev.lock` is taken with
  `flock(LOCK_EX|LOCK_NB)` so only one copy runs. If the lock is held, the new
  copy looks for another process already named `bluev.elf` (elfldr names an
  injected process after the uploaded ELF itself, `ki_comm`) through the
  kernel's process list (`sysctl(KERN_PROC_PROC)`, `platform/ps5/process_identity.cpp`)
  — not over HTTP, so this works even before the old copy's web server has
  come up. It sends it `SIGTERM`, which the old copy treats exactly like
  `POST /api/quit` (finish what is running, then stop), and waits up to 100 s
  for the lock. Only once it has the lock (an older copy is gone or there
  never was one) does this copy call `thr_set_name` on itself — a lookup
  never has two processes to choose between while it is still running.
- Files in `/data/bluev/` (`main.cpp`, `FileBondStore`): `bluev.log`,
  `bluev.lock`, and one `bond-<ours>-<theirs>.key` (23 bytes: address 6, link
  key 16, key type 1) plus `bond-<theirs>.name` per paired device (section 12b).
- Start-up order is the log, the lock, the capture,
  `Service::run`; shutdown is the reverse (the HTTP server stops and waits for
  requests in flight before the radio is closed). Each of these owns its state
  in a class; the only global is the pointer to the log sink
  (`setLogSink`).

### Service states

| State | Meaning |
|---|---|
| `idle` | page scan on; a paired device that dials in is accepted automatically |
| `scanning` | inquiry running (30 s); results are listed in the UI |
| `connecting` | paging or adopting an inbound link, securing it, setting up AVDTP |
| `streaming` | audio is flowing; ends when the device leaves or on `disconnect` |

A session (`Service::runSession`) either connects out (`BtStack::connectTo`,
started from the UI) or adopts an inbound link (`BtStack::adoptIncoming`), then
runs `A2dpSession::start` -> `A2dpStreamer::run` -> `A2dpSession::stop` ->
`BtStack::disconnect`, resets the AVDTP state and goes back to `idle`.

## 2. Web interface (`service/web.cpp`, `web/index.html`)

One accept thread (it polls every 200 ms so it notices a stop request); each
connection is served by a short-lived thread (browsers keep idle sockets open
and must not block the others), at most 8 at a time, reserved with a
compare-and-swap. `stop()` stops accepting and then waits for the requests in
flight. Port 7733, fixed; `start()` is retried every 2 s if it is taken (also how
it comes up once the network does). No authentication: anyone on the local network can use it.

| Endpoint | Purpose |
|---|---|
| `GET /` | the embedded control page |
| `GET /api/status` | state, message, current peer, stream numbers (rate, packets/s, queue) |
| `GET /api/devices` | scan results plus paired devices, with `bonded`/`connected` flags |
| `GET /api/log?since=N` | log lines from the ring buffer (300 lines) |
| `POST /api/scan` | start a 30 s inquiry |
| `POST /api/connect?addr=AA:BB:CC:DD:EE:FF` | connect to a device (pairs it when no bond exists) |
| `POST /api/disconnect` | end the stream and drop the link |
| `POST /api/forget?addr=...` | delete a device's bond files |
| `POST /api/quit` | stop the service |

State-changing calls are `POST` only. `scan`, `connect` and `forget` are
refused unless the service is idle.

## 3. Audio capture (`platform/ps5/avcap_source.cpp`)

- `sceKernelLoadStartModule` loads `/system/common/lib/libSceIpmi.sprx` and
  `libSceAvcap2.sprx`.
- The `sceAvcap2*` functions are resolved by NID with
  `kernel_dynlib_resolve(pid, handle, NID)` (Initialize, Terminate, OpenAudio,
  Close, Start, Stop, ReadAudio).
- `AvcapSource::start`: Initialize -> OpenAudio (2 channels, all devices) ->
  `Start(handle)`. Everything is undone on failure; a changed error code is
  logged ("not available yet" is normal when no game or audio exists yet).
- `AvcapSource::read` returns records of 8-byte (L/R float) frames; `0` means no new
  data or a ring overrun (the first 5 are logged), `-1` an error.
- `AvcapSource::restart` closes and reopens; this is how audio comes back after a
  game exits and starts again. Capture is opened once for the whole service
  lifetime and keeps running between streams.

## 4. HCI transport (`platform/ps5/ugen_transport.cpp`)

The controller is `/dev/ugen0.2`. 65 transfers are set up with the FreeBSD
`USB_FS_*` ioctls:

| Transfers | Role | Size |
|---|---|---|
| 0-47 | HCI events (async reads) | `0x104` |
| 48-63 | ACL in (async reads) | `0x44c` |
| 64 | ACL out | `0x44c` |

The **endpoint numbers differ between hardware revisions**:

| | events | ACL in | ACL out |
|---|---|---|---|
| fat PS5 | `0x82` | `0x81` | `0x01` |
| PS5 Slim | `0x81` | `0x82` | `0x02` |

The layout is read from the device's descriptors at radio start
(`Ps5UsbDirectory::discoverLayout`, libSceUsbd, read only; the first `e0/01/01` interface
with an interrupt in, a bulk in and a bulk out, the bulk out taking the number
of the bulk in), the same for the console's chip and for adapters. Everything
else, opening, commands, events and ACL, goes through ugen. If the descriptors
cannot be read, an adapter gets the standard layout and the console's chip the
detection by behaviour below.

`UgenTransport::detectRoles` (the fallback) runs once at startup (from
`BtStack::start`): it opens with the fat
layout, sends `Read Buffer Size` (`0x1005`) every 1.5 s and watches on which IN
pipe the Command Complete arrives. If it arrives on the second pipe the layout
is the Slim one: the device is closed, the standard layout (`0x81`, `0x82`,
`0x02`) is selected with `select()` and it is reopened. The result is kept for the lifetime of the
process; nothing is re-detected per connection.

- HCI **commands** go out as control transfers (`USB_FS_SETUP`,
  `bmRequestType=0x20`) in `UgenTransport::command`.
- `UgenTransport::pump(timeout)` restarts idle reads and `reap()` collects finished
  transfers. Consecutive packets inside one transfer are split into two
  software rings (events and ACL, 32 slots each); the oldest packet is dropped
  when a ring is full.
- `UgenTransport::sendAcl` uses a single output transfer and waits up to 1 s for the
  previous one.
- The system's own traffic passes through the same controller, so replies to
  other parties' commands are counted and the first 50 are logged, not
  processed.
- Diagnostics: per-endpoint counters (`UgenTransport::describe`), logged when
  the transport closes.

## 5. Controller setup (`BtStack::start`)

1. `IHciTransport::open`; failure means "radio busy".
2. Role detection and reopen (section 4).
3. HCI Reset for an external adapter; the console's own chip is never reset.
4. Adapter firmware (section 5a): `Read Local Version`, and for a Realtek USB
   chip still in ROM mode upload its patch, then reset again.
5. `Read Buffer Size` gives the ACL size and credit count (default 7 x 1021
   when there is no answer; the Slim reports 8 x 1021).
6. `Set Event Mask`, Secure Simple Pairing on, `Write Inquiry Mode` (`0x0c45`,
   value 2: results with RSSI and EIR, which carries the device name),
   Class of Device `04 01 00`, local name `BlueV`, plus a few
   read-only commands whose replies are logged for diagnostics.
7. `drop_stale_links`: scanning off, then `Disconnect` for handles 1-8 to clear
   links left behind by a killed payload.
8. Page scan on, so a paired device can connect to us.

### 5a. Adapter firmware (`core/bluetooth/firmware.*`, `realtek_firmware.*`)

Some USB controllers boot into a limited ROM: they enumerate, answer HCI and
appear in the adapter list, but the radio cannot scan until the host uploads a
patch. `Read Local Version` names the family (`firmwareFamily`, by
manufacturer). Only Realtek has an uploader; CSR runs from ROM, and Broadcom,
Intel and MediaTek are logged as "not supported yet" and left to start as they
are, because many of their sticks do work without a patch.

For the Realtek USB family, a match in the table in `realtek_firmware.cpp`
(LMP subversion, HCI revision and version, the way `btrtl` matches them; an HCI
version of 0 is a wildcard, which the RTL8761CU row uses) means the chip is
still in ROM mode; after the upload those numbers change, so a later start skips
the step. A chip that matches no row is first sent `0xfc66` (drop the vendor
patch) and has its version read again after 200 ms, as `btrtl` does; if it is
still unknown it is started as it is. The sequence:

1. `Read ROM version` (`0xfc6d`).
2. Read the security key id from `RTL_SEC_PROJ` (`0xfc61`).
3. Read `rtl8761bu_fw.bin` (and the optional `rtl8761bu_config.bin`) from
   `/data/bluev/firmware/`, through `IFirmwareStore`. The RTL8852C prefers
   `rtl8852cu_fw_v2.bin` and falls back to `rtl8852cu_fw.bin`.
4. Parse the container: check the extension signature at the end and that its
   project id belongs to this chip (a file for another Realtek chip is
   refused). `Realtech` v1: a patch table; keep the patch whose chip id is
   `romVersion + 1`, and stamp the file's version into its last four bytes.
   `RTBTCore` v2: sections and subsections; keep the ones for this ROM version
   and insert them in priority order (equal priorities in reverse file order,
   the way `btrtl` inserts them). For a chip with a non-zero key id, only the
   security subsections carrying that key id are kept, and the config file is
   not used; without a key id the security sections are skipped and the config
   blob is appended.
5. Send it in 252-byte fragments with `0xfc20` (an index byte, `0x7f` wraps to
   `1` because `0x80` marks the end, and the last fragment has `0x80` set), one
   command at a time.
6. HCI Reset, then the normal bring-up continues.

A missing or unusable file fails `start()` with the file name and the directory,
rather than leaving an adapter that cannot scan. A failed upload says to unplug
and replug the adapter, because the chip keeps whatever part of the patch it
already took. The parsers and the fragmenter are pure functions and the HCI
surface is the four-function `FirmwareHci`, so they run on the PC; `BtStack`
supplies `hciSync`, `cmd`, `poll` and the last Command Complete.

Only the RTL8761BU has been run on hardware. The other rows, including the
secure-boot parts whose patch comes from the key-id security sections, follow
the same path as `btrtl` but are untested.

The uploader is not in the console code: it is in `core` and reads through
`IFirmwareStore`, whose console-side implementation is `FileFirmwareStore`
(`/data/bluev/firmware`). `installFirmware` is the family dispatch; a
Broadcom/Intel/MediaTek loader is a new module and one branch there, not a
change to the transport or the stack.

## 6. Connections and pairing (`core/bluetooth/bt_stack_link.cpp`, `bt_stack_inquiry.cpp`)

**Inquiry** (`BtStack::scan`, `runInquiry`): GIAC, up to 30 s. `onInquiryRecord`
keeps Audio/Video class devices and takes the name from EIR data when present.
`is_junk` marks TVs and soundbars (minor class, name list); this is only an
"other device" hint in the UI and never influences a choice.

**Outbound** (`BtStack::connectTo`): selects the device's bond (`setBond`), then
`page_device`: up to 4 rounds of a 4 s Create Connection followed by a 20 s
window in which the device may connect to us instead. Packet types and role
switch alternate between rounds (all types / DM1 only). A late "ACL already
exists" (`0x0b`) is first given time to complete as our own link before any
cleanup. Then `encrypt_link`.

**Inbound** (`BtStack::poll`, connection request): only devices that
have a bond file are accepted (as the **master** of the link, see below); others
are logged and left to the console. A connection
complete event from a bonded device is accepted even if the target changed
meanwhile. A fresh link resets authentication, encryption, credit and
in-flight state. `BtStack::adoptIncoming` then runs `encryptLink`.

**Role.** The connection is accepted with the master role (`Accept Connection
Request`, role 0). With the default (stay the slave) the console is the
slave of the speaker's piconet while it is the master of its controllers'
piconet, and the chip has to share its time between the two. On the Slim a
DualSense that was connected before the speaker lost its link (`reason 0x08`,
supervision timeout) after 22 s and after 126 s of streaming in two runs in a
row (nothing of the console's own was taken beforehand, so it is not the
effect of section 12). With the master role (confirmed with `Role Discovery`,
logged as `link role: ... we are the master`) the same setup ran 4.5 minutes
with a game and audio and no loss, so far one run: not proven.

**`encrypt_link`**: waits for authentication (30 s), then for the link to
encrypt itself (2 s) and forces it with Set Connection Encryption (20 s). A
failed authentication with a stored key deletes that bond. The link's packet
types are left to the controller: forcing multi-slot types with Change
Connection Packet Type right after encryption was tried on the Slim and
coincided with the speaker dropping the link a second later (reason 0x08)
in most sessions, and with it removed 4 of 4 reconnects were clean.

**Pairing**: PIN request answered with `0000`, IO capability
`NoInputNoOutput`, user confirmation answered automatically. The link key
notification writes `bond-<ours>-<theirs>.key` (not saved for TV-like devices).

**Teardown** (`BtStack::disconnect`): disconnects the current handle, cancels any
pending Create Connection, resets link state and turns page scan back on.
If the device already dialed in again during teardown (counted with a
connection generation number) that new link is kept for the next session.

## 7. Event/ACL loop and L2CAP (`BtStack::poll`)

`poll(ms)` is the heart of all Bluetooth work; every wait calls it:

- `IHciTransport::pump`, then the **HCI events** from the ring are handled (connections,
  disconnections, authentication, encryption, Command Complete/Status, Number
  of Completed Packets, PIN/link key/IO capability requests, inquiry results).
- **ACL packets** are reassembled into L2CAP frames (up to 4 KB) and routed by
  channel id:
  - CID 1: signaling (connect/configure/disconnect/echo/information requests).
    Channels opened by the headset are accepted here, for PSMs bound with
    `bindInbound` (AVDTP `0x19` is bound while idle, see section 9).
  - CID `0x50`: SDP (`sdp_handle`).
  - Others: the frame handler of the registered channel.
- A tick runs once a second; the service uses it to publish snapshots and to
  honour stop requests during streaming.
- **Flow control**: ACL credit counter plus timestamps of packets in flight.
  `canSend` is true only with credit and at most 5 unconfirmed packets;
  packets without a completion report after 31 ms count as completed
  ("completions assumed"). Limits of 3 packets and 100 ms cap the stream near
  40 packets/s; a 44.1/48 kHz stream needs 49-53, and with those limits the
  Slim dropped about 20 ms of audio every 10 s.

Channel API: `openChannel` (connection request, then configure requests
every 6 s until the channel is `up` and `configured`; a "no resources" answer is
retried; gives up after 14 s), `closeChannel`, `bindInbound`.

## 8. SDP (`core/bluetooth/sdp_server.cpp`)

A minimal SDP server so headsets recognise us as an A2DP source: Service Search,
Service Attribute and Service Search Attribute PDUs. One record (`0x10001`):
service class A2DP Source (`0x110a`), protocol list (L2CAP/AVDTP), profile
(`0x110d`), name `BlueV`. A match on the A2DP UUIDs (`0x110a`,
`0x110d`, `0x0019`, `0x0100`, `0x1002`) counts as found.

## 9. A2DP setup (`A2dpSession::start`, `A2dpSession::onSignal`)

AVDTP has two roles: the initiator (INT) sends the commands and the acceptor
(ACP) answers. Normally the audio source is INT. Some headsets and speakers,
when they reconnect by themselves, act as INT as well; refusing their commands
made them close the channel. Both ways are supported:

**Headset-driven** (a reconnecting headset): `a2dp_listen` binds PSM `0x19`
while the service is idle, so the headset's first channel request is accepted
immediately. Inbound commands are answered by `on_signal_frame`:

| Command | Answer |
|---|---|
| Discover | accept, one SBC source endpoint (seid 1) |
| Get Capabilities | accept, media transport + SBC |
| Set Configuration | accept; the headset's SBC parameters are adopted |
| Open / Start | accept |
| anything else | general reject |

When the headset made the connection, `A2dpSession::start` first waits up to twenty
seconds for its signaling channel. When **we** made the connection it waits
one: the Sony WH-CH720N hangs up about eight seconds after a connection the
host made if the host starts nothing, the WH-1000XM5 after two to three, and
the Creative speaker only opens the channel after that. After the second we
open the channel ourselves
(`avdtp: signaling channel opened by us`), take over after 1.5 s of silence
instead of four, and fall back to waiting for the headset if our own attempt
fails. With this both devices connect from our side in about ten seconds (an
external adapter) and the stream starts at 48 kHz.

`start` waits for the headset's Set Configuration (as long as the headset
keeps sending commands, however slowly; it is left to us only after four silent
seconds, and never waited for longer than twenty: its pause between commands has
been seen to reach two seconds, and a fixed four second window made both sides
configure at once), derives the encoder
parameters from it with `choose_params`, waits for the Open and the media
channel (inbound, up to 6 s), then waits 300 ms for a Start from the headset
and sends `START` itself if none came.

**Source-driven** (used when the headset starts nothing
within 4 s): open the signaling channel, `DISCOVER`, `GET_CAPABILITIES` for the
sinks, `chooseA2dpParams`, `SET_CONFIGURATION`, `OPEN`, media channel (the
headset opens it within 1.5 s, otherwise we do), 300 ms, `START` (an
unfavourable answer still lets streaming be attempted).

`choose_params(caps)`: 48 kHz preferred, else 44.1 kHz; Joint Stereo
preferred, else Stereo; blocks 16 > 12 > 8 > 4; 8 sub-bands when supported;
Loudness allocation when supported, else SNR; **bitpool locked at 40**
(clamped to the sink's range).

`A2dpSession::stop` sends `CLOSE` and closes the media and signaling channels.
`a2dp_reset` clears the channels and all headset-driven state between
sessions.

## 10. Audio streaming loop (`A2dpStreamer::run`)

The encoder is set up; at most 8 SBC frames go into a packet (bounded by the
headset's MTU and the ACL size). The loop runs until the link drops, the media
channel closes, or a stop is requested:

1. **Capture state.** Without capture, the queue is filled with wall-clock
   silence (so the stream does not stall) and `AvcapSource::restart` is tried
   every 500 ms. With capture, records are read.
2. **FIFO** (24000 stereo frames, int16): 48 kHz float is clamped to +-1 and
   scaled by 32767. A 44.1 kHz sink gets linear resampling. A capture error
   resets the FIFO and enters the waiting state.
3. **Latency trim:** if the FIFO exceeds ~50 ms (rate/20) the excess is dropped
   ("trimmed"); this keeps the stream from falling more than ~160 ms behind.
4. **Packetising:** when a packet's worth of samples is queued and
   `canSend()` is true, a packet is sent. Sending pauses while it is more
   than 25 ms ahead of the wall clock and the FIFO is below 80 ms; at 80 ms or
   more there is no rate limit. A packet is a 12-byte RTP header (v2, PT 96,
   seq, timestamp = samples sent, SSRC 1) + the SBC frame count byte + the SBC
   frames, sent to the L2CAP media channel with `BtStack::send`.
5. `poll(0)` after a send, `poll(2)` otherwise, so events and credits are
   processed.
6. Every 10 s a status line is logged (packets, queue ms, trimmed, overruns,
   restarts, credits, assumed completions, missing reports). The same numbers
   feed `/api/status`.

## 11. Notifications and logging

- `AppLog::notify` calls `sendNotification` (`sceKernelSendNotificationRequest`)
  directly on the calling thread.
- What appears on the screen is kept short: `BlueV ready: open http://<ip>:7733`
  once the web server is up (the address comes from the network interface, and
  is only shown, not logged; without one it points at the home screen entry),
  `Streaming to <device>` when audio starts, `BlueV: home screen entry ready`
  after the entry was installed or updated, and the messages that need the
  user to act or know about a failure (the radio did not start and why,
  capture unavailable, another copy is running, "turn the device off and on").
  Details (format, bitpool, device found) are in the log only.
- `AppLog::line` formats the line, then under the `LogBuffer` mutex only touches memory
  (the numbered ring buffer of 300 lines and a pending-text buffer for the
  file). The file is written by `flush()`, called once a second from the
  service tick and on close, with its own lock, so a log call never waits for
  file I/O. A crash can lose at most the last second of the file. Nothing is
  written to the console's kernel log: it is shared with the whole system and
  BlueV's lines (about a tenth of what a 25 s look at it showed) only made it
  harder to read; the page and the file are where to look.
- The file (`/data/bluev/bluev.log`, `LogFile`) is appended to and stays small:
  when it reaches 512 KB it becomes `bluev.log.1`, replacing the one before,
  and a new file starts, so at most two files of 512 KB exist. While streaming
  it grows about 64 KB an hour (one status line every 10 s).
- Lines carry `HH:MM:SS` (UTC) stamps. Messages have fixed prefixes
  (`found ...`, `l2cap: ...`, `avdtp: ...`, `stream: ...`), which keeps logs
  comparable between runs.

## 12. The controller is shared with the console

The Bluetooth chip also serves the console's own stack (the DualSense). A
USB IN packet is delivered to exactly one of the transfers queued on its
endpoint, so **every packet our reads take is a packet the console's stack
never sees**. Measured on a PS5 Slim, always with a DualSense that was
connected over Bluetooth:

| What ran | A controller that is switched off and on again |
|---|---|
| no payload | reconnects |
| USB opened, no reads at all | reconnects |
| one event read and one ACL read, no commands | does not reconnect |
| 48 + 16 reads (this program) | does not reconnect |

The harm is lasting: after the reads stopped the console still did not accept
the controller until it was restarted. The log shows what happens: with a
single read we took the console's own `Command Status` (`Exit Sniff Mode`),
its link events and an L2CAP frame of the controller's link. A command whose
answer is lost leaves the console's HCI layer waiting, and the chip takes one
command at a time, so nothing else goes out, including accepting the
controller's connection request. The commands we send and the settings we
change are not the cause (the listen-only run sent none).

A controller that is already connected is not disturbed: its input does not
use the HCI event/ACL endpoints that we read (no packet of it ever appeared in
the dumps), which is why audio and a connected controller work together. What
breaks is every moment when the console needs HCI events for the controller:
connecting, disconnecting, reconnecting, link changes.

What the program does about it: **nothing yet.** The radio is opened at
start and read continuously, so a controller must not connect, disconnect or
reconnect while the program runs. Switch the controller on before the
program starts and leave it alone while it runs. Stopping the program does
not repair a controller that has been switched off and on in the meantime
(the later measurements below).

What was tried and dropped:

- *Reading the controller's state from the system's pad library* so that the
  radio could stay closed until a DualSense is on Bluetooth
  (`scePadGetBluetoothAddress` tells Bluetooth from a cable). The library
  initialises and the logged-in user is found, but `scePadOpen` is refused
  for this process (`0x809b0081`, an error from outside the pad module, so
  most likely a permission limit). Getting past it means changing the
  process's credentials, which was judged too risky.
- *Sniffing the USB endpoints* to learn the controller's state: reading is the
  very thing that breaks the console's stack.
- *Looking at the console's kernel structures* to find which process owns the
  Bluetooth device nodes (`/dev/bt`, `/dev/bluetooth_hid`; the raw
  `/dev/ugen0.2` that we use is the same chip seen from USB). A survey of the
  descriptor tables crashed the console and was abandoned.

More measurements (2026-10-21, same console, pad connected before the payload
started; "breaks" means that after the test the pad, switched off and on, comes
back as connected but without input until the console is restarted). Together
with the table above: one event read is harmless, one ACL read is not, and
four event reads are not:

| What the payload did | Pad afterwards |
|---|---|
| nothing (process running, radio never opened) | fine |
| opened the endpoints and closed them, no reads, no commands | fine |
| 1 event read on the event endpoint, 5 s, no commands | fine |
| 1 event read + full setup (event mask, class of device, name, inquiry mode, scan, link cleanup) + 30 s inquiry | fine |
| 4 event reads + the same | breaks |
| 48 event + 16 ACL reads, even with no command and no packet received | breaks |
| 48 + 16 reads with a stolen pad request or disconnect event | breaks |

So the damage comes from the number of reads armed on the shared endpoints,
not from the commands, the setup writes or the closing sequence (a
close without `USB_FS_STOP` breaks as well). With few reads the replies are
missed (46 commands, 17 replies with one read; 37 and 28 with four), and the
bonded pad's inbound request and its disconnect event were seen to be taken
by the payload ("ignored: not paired", "another link closed") before the
system could see them. Waiting for the remaining replies before closing does
not help: they never come (the controller drops a command sent while another
is outstanding).

Working rule: the pad is switched on before the payload starts and is left
alone while it runs. (A run-time knob for the number of armed reads was used for
the measurements above; it is not part of the program.)

Ideas that remain open: a second Bluetooth adapter for the audio (the console's
own chip stays untouched); reading the console's own copy of the events
instead of taking them from USB; or accepting the limitation above.

## 12c. What the USB descriptors say (libSceUsbd, `tools/usbprobe`)

libSceUsbd is the system's libusb-1.0 (default context, loaded at run time).
Its configuration descriptors can be read without any bus traffic, and the
console's driver state per interface with `KernelDriverActive`:

| Device | Interfaces and endpoints | Kernel driver |
|---|---|---|
| CSR adapter `0a12:0001` | 0: `81` interrupt in (events, 16), `82` bulk in, `02` bulk out (64); 1: isochronous `83/03` | none (ugen only); no string descriptors |
| console chip, MediaTek `0e8d:3603` | 0: `81` interrupt in (16), `01` bulk out, `82` bulk in, `02` bulk out, `8f` interrupt in (2); 1: isochronous `83/03`; 2: `8a` in / `0a` out interrupt (64/512); **3: `8c` interrupt in (16), `0b` bulk out, `8d` bulk in, `0d` bulk out**; 4: isochronous `8e/0e` | attached to interfaces **0 and 3**, none on 1, 2, 4 |
| DualSense `054c:0ce6` | | attached to interfaces 0 and 3 |

So the standard layout assumed for external adapters is right. The console's
chip has a second group of the same shape on interface 3 (`8c`/`8d`/`0b`/`0d`);
what it is used for is not known. Our radio uses group 0 (`81`, `82`, `02`).
BlueV uses libusb for one thing only: reading the configuration descriptor at
radio start to find the endpoints (`Ps5UsbDirectory::discoverLayout`). On the console's chip
(Slim) it gives events `0x81`, ACL in `0x82`, ACL out `0x02` without the
detection by behaviour, whose 48 + 16 reads on the wrong pipes for a few seconds
had broken the gamepad's reconnection; the radio opens in 3 s. Measured
(2026-10-24): internal radio on, the speaker connected and streaming for a
while, the gamepad (Bluetooth) kept working; the adapter streams as before. One
run each. The probes in `tools/` also use libusb.

Is interface 3 a controller of its own? a throw-away probe (not kept) sent the read-only
commands Read BD_ADDR, Read Local Version and Read Buffer Size with
`wIndex = 3` (one read armed on `0x8c`): all three were refused with EIO, and
nothing came on `0x8c`. The gamepad was connected over Bluetooth during that
run and kept working afterwards (one run, one read). Interface 3 does not take HCI commands as class control
requests, so it is not a second controller that can be used this way. What it
carries is unknown; writing to its bulk endpoints (`0x0b`, `0x0d`) was not tried,
the console's driver is attached to it.

HCI through libusb on the USB adapter (a throw-away probe, not kept; 2026-10-24, CSR
`0a12:0001`, BlueV's radio off): `sceUsbdOpen` and `ClaimInterface(0)` succeed;
commands as `ControlTransfer(0x20, 0, 0, 0, ...)` return the number of bytes
sent; events come with `InterruptTransfer(0x81, ...)`. Read BD_ADDR, Read
Local Version and Read Buffer Size were answered in 3 to 5 ms (the same as
through ugen), Inquiry gave Command Status, an Inquiry Result and Inquiry
Complete. Two quirks: the Command Complete of the first command (Reset) was
lost in one run when no read had been made yet, and came after 147 ms in the
next run after a read that timed out; and in that run an event arrived as a
lone one byte piece (the tail of a 17 byte event split over two reads, the
head lost when a read with a timeout ended). So libusb works for HCI, with no
advantage over ugen, and synchronous reads with timeouts need care with the
event framing. ACL transfers and asynchronous transfers through libusb were
not tried.

Adapters whose Bluetooth interface is vendor specific (class `ff`, subclass and
protocol `01/01`) are mostly the Broadcom family (BCM20702 / BCM2045 sticks and
the OEM modules sold under Broadcom, ASUS, Foxconn, Lite-On, IMC, Dell, Toshiba,
Apple and Belkin ids; from memory of Linux's btusb, not checked on these
devices). The descriptor lookup only accepts `e0/01/01`, so for them it fails and
the standard layout is used, which they use too. The real risk there is missing
firmware, not addresses; try a real adapter of that family before widening the
lookup.

## 12d. Soak test on the console's chip (2026-10-24)

The internal radio (layout from the descriptors) with the Creative speaker at
48 kHz, SBC bitpool 40, and a game running, for 21 minutes (the gamepad,
connected over Bluetooth, was not touched):

| Measure | Result |
|---|---|
| Packets per second | min 53.4, mean 53.6, max 53.8 (67 238 packets) |
| Queue | 0 to 16 ms, mean 7.9 |
| trimmed / capture overruns / restarts | 0 / unchanged (3, from the start) / 0 |
| ACL credits | 2 to 5 |
| Link losses, foreign requests | none |
| Missing completion reports | about 66 a minute (assumed by the flow control) |

Heard by the user: no dropouts, sound good. One run of 21 minutes; longer
runs and other speakers were not tried.

## 12a. Telling whether a gamepad uses the Bluetooth chip

Looked at on 2026-10-23, nothing tells it reliably from our process:

| Source | Result |
|---|---|
| `scePadOpen` / `scePadGetHandle` (libScePad) | refused, `0x809b0081`, an answer from the device bus daemon, not from the pad library |
| `sceMbusGetDeviceInfo_`, `...ByBusId_` (libSceMbus, queried with a throw-away probe that is not kept) | `sceMbusInit` works, the daemon answers `0x809b0005` / `0x809b00ff`, the same with the pad on Bluetooth, switched off and wired |
| USB listing (`GET /api/adapters`) | a wired DualSense shows as `054c:0ce6`, and it shows the same whether the controller's "communication method" is USB or Bluetooth (cable plugged in) |
| `sceUsbdKernelDriverActive` (libSceUsbd, `tools/usbprobe`) | the console's driver is attached to interfaces 0 and 3 of the DualSense in both settings |

One more measurement (2026-10-24), with the controller's communication method
set to USB and the cable plugged in: the internal radio opened, a speaker
connected and streamed for a while with the controller working. When the cable
was pulled the controller went back to Bluetooth and sent two connection
requests (34 s apart) that the payload's reads took ("inbound connection from
... class 002508 ignored"), and the controller did not come back. So the USB
setting protects only while the cable stays in. A controller that is plugged
in and left alone was not affected in that run.

So the warning before starting the internal radio is plain text: switch the
gamepad on first and leave it alone while the radio runs.

## 12b. Starting the radio

The payload starts with the radio off: no USB device is opened, no command is
sent, nothing is read. The control page (or `POST /api/radio/on`) chooses the
controller and starts it:

- `adapter=internal`: the console's own chip (`/dev/ugen0.2`), shared with the
  console's stack, with the limits of section 12.
- `adapter=external&dev=ugenX.Y`: an adapter in a USB port, found with
  `GET /api/adapters` (opens every other ugen node to read its device
  information; the console's own chip is not touched). It is driven with the
  standard layout (events 0x81, ACL in 0x82, ACL out 0x02) and the chip to
  itself. Tested with a CSR adapter (`0a12:0001`, HCI 4.0, 10 ACL buffers of
  310 bytes): it opens at once, the commands are answered in milliseconds,
  the speaker connected on the first page, paired in 1 s and streamed 44.1 kHz
  with `trimmed 0` (115 packets/s of 3 SBC frames, because of the small ACL
  buffers). From selecting the speaker to audio: 11 s (the internal chip:
  about 105 s). Pairings are kept per adapter:
  `bond-<our address>-<their address>.key` holds the key (the link key belongs
  to the pair), `bond-<their address>.name` the device's name, so the same
  speaker can be paired with the console's chip and with a USB adapter at the
  same time. The list shows the pairings of the adapter that is in use (none
  while the radio is off); Forget removes the device for every adapter. Files
  from before this change (`bond-<their address>.key`) are not used; the
  device is paired once more.

`POST /api/radio/off` stops the radio again. Scan and Connect are refused
(409) while it is off; Forget only touches files and still works.

## 13. Choices made for the PS5 Slim

- **Endpoint layout** read from the USB descriptors (section 4); a controller
  with slower timing gets longer waits; paging is not relied on, because the
  Slim's controller pages unreliably (devices mostly dial in themselves).
- **Service mode**: a persistent process with a web API and page, per-device
  bonds, automatic acceptance of paired devices; the user chooses the device.
- **AVDTP**: headset-driven setup (section 9) and early inbound binding.
- **Flow control** thresholds (section 7).
- **Arithmetic**: remaining times are signed (an unsigned underflow once hung
  the program), the RSSI is signed, the SBC capability list is walked by length.

## 14. Threads, ownership and tests

| Thread | Does |
|---|---|
| radio (main) | everything in `radio/` and `audio/`, the service state machine |
| web accept + one per request | HTTP only; touches the radio never |

Rules that keep this race-free:

- Radio state is owned by the radio thread. Other threads reach it through
  three boundary objects only: the pending command and status snapshot in
  `Service` (one mutex), the log ring (`Logger`, its own
  mutex), and two atomic flags (`m_quit`, `m_disconnect_requested`). The
  streaming loop publishes its numbers with `service_publish_stream` (a few
  times a second, under the service mutex) and polls an atomic stop flag.
- ThreadSanitizer (`tests/run.sh thread`) runs the service tests, including 8
  clients firing random requests, to catch races.
- Nothing on the radio thread sleeps or waits for I/O other than the
  controller itself.

Tests: `tests/run.sh [plain|thread|address] [gtest filter]` builds everything
except the PS5 adapters on a PC (CMake + Ninja, googletest submodule) and runs
about 210 tests: the Bluetooth stack against a fake controller and a fake
speaker (pairing, paging, L2CAP, flow control, AVDTP, the streaming loop), the
service against a gmock radio over real HTTP (including 8 clients firing random
requests), and the file/log/notification helpers. The PS5 adapters under
`src/platform/ps5/` are compile-checked by the console `make` only.

## 15. Launcher entry (ps5-payload-websrv)

`homebrew/bluev/` is an extension for the launcher of
[ps5-payload-websrv](https://github.com/ps5-payload-dev/websrv): `make homebrew`
collects `eboot.elf`, `homebrew.js` and `sce_sys/icon0.png` in
`build/homebrew/bluev/`, which is copied to `/data/homebrew/` on the
console. The launcher prefers `homebrew.js` over `eboot.elf`, takes the tile
icon from `sce_sys/icon0.png`, and runs the script in a sandboxed frame
(`allow-scripts allow-modals`: no popups, no navigation, origin `null`).

- The tile starts the payload (`{path, cwd, daemon}`) when `/api/status` does
  not answer, and otherwise opens a control menu built with `showCarousel`:
  state, the devices (tap to connect, "Forget this pairing" in the item's
  options), scan, disconnect, stop. This is the web interface again, inside the
  console's own launcher.
- The frame reaches the payload on `http://127.0.0.1:7733`, so the menu works in
  the console's own browser; from a PC the launcher's loopback address is the
  PC, use the page on port 7733 there.
- The server answers every request with `Access-Control-Allow-Origin: *` and
  answers `OPTIONS` with 204, which the sandboxed origin needs. It also means
  any web page in a browser on the network can call the unauthenticated API.
- The icon is `homebrew/bluev/sce_sys/icon0.png` (512x512, full-bleed background; the
  console rounds the corners itself).

## 16. Home screen entry

When the program starts (before the radio is opened), `AppShortcut::install`
makes sure the console's home screen has an entry that opens the web interface
in the console's browser. It is the mechanism the payload manager uses:

- A title directory `/user/app/BLUV00001/sce_sys/` with a `param.json` and an
  `icon0.png` (the icon is embedded in the program, `tools/embed.py`).
  `param.json` carries `"deeplinkUri": "http://127.0.0.1:7733/"`, so starting
  the entry sends the browser to the web server of the running program, and
  `"applicationCategoryType": 65536` as in the payload manager's entry.
- The system's app installer registers it. `libSceAppInstUtil` must **not** be
  linked: with its stub library in the link the payload does not start at all on
  the console. The module is loaded at run time instead (like `libSceAvcap2` for
  the capture) and the functions are looked up by name with
  `kernel_dynlib_dlsym` (`Initialize`, `Terminate`, `AppUnInstall`, `AppInstallAll`);
  `AppInstallTitleDir` is not found, by name or by the NID the payload manager
  uses, on firmware 12.40, so, like the payload manager, `AppInstallAll` registers
  the new title of `/user/app`.
- Nothing is done when the files are as they should be and a marker file
  (`.registered`) says the installer accepted them; files that are right but were
  never registered (the installer failed before) are tried again. A different port
  or icon installs it again. A failure is only logged, the program carries on.
- `POST /api/uninstall-shortcut` unregisters the entry (`sceAppInstUtilAppUnInstall`)
  and deletes the files.

The entry only works while the program runs, since its content is the web server:
send the ELF first, then use the home screen entry. The title id is fixed;
the port in the deeplink (7733) is also the only one the server ever binds,
so the entry cannot fall out of sync with it. The host test
checks the files and the calls to the installer with a fake that behaves like the
console's (no `AppInstallTitleDir`). On the console the installer accepts the
directory (logged as `shortcut: the home screen entry opens ...`).

## 17. Design decisions

### 17.1 What we chose, and why

| Decision | Why |
|---|---|
| Ports and adapters: `app -> service -> core`, `platform -> core/ports` | Every layer runs on a PC under gtest/gmock; everything that touches the console is in `src/platform/ps5/`, and `core` has no console header |
| C++20, no exceptions, no RTTI (`-fno-exceptions -fno-rtti` on the console) | Small payload, no surprises from the console's runtime; the interfaces are header only, so tests may use RTTI |
| The stack does not sleep or read the clock itself: time comes from `IClock`, passes only inside `poll()` | Tests move a fake clock, so timeouts of 20 s run in milliseconds; the code still uses the same waits as on the console |
| One radio thread; web threads only post one pending command and read snapshots under a mutex | The HCI side is never touched from two threads; a second command gets `Busy` instead of a race |
| The radio is off at start; the user starts it from the page | Nothing opens a USB device until asked: the internal chip is shared with the gamepad (section 12) |
| A USB adapter is the recommended path | The chip is ours alone: no gamepad problem, 11 s from selecting a speaker to audio against about 105 s on the internal chip (12b) |
| Endpoint layout read from the USB descriptors (`libSceUsbd`), not guessed by behaviour | Behaviour detection armed reads on the wrong pipes and broke the gamepad's reconnection (12c) |
| A paired device that dials in is accepted; an unpaired one is refused and counted (`foreign_requests`) | The console is not pairable by whoever walks past; a speaker that reconnects by itself just works |
| Pairings are files, per adapter and device; names per device | The same speaker can be paired with the internal chip and with an adapter at once; plain files are easy to inspect and to test |
| AVDTP: wait for the device to start the procedure (1 s if we made the connection, 20 s if it did), then drive it ourselves | Headsets differ: the Sony WH-CH720N hangs up after about 8 s if the host does nothing, the WH-1000XM5 after 2-3 s (was 3 s here, 2026-10-25), the Creative speaker opens its channel later; starting at once collided with a device that drives (link timeout, 2026-10-18) |
| Flow control: at most 5 unconfirmed packets, a missing completion report counts as done after 31 ms | Limits of 3 packets and 100 ms cap the stream near 40 packets/s and dropped audio (`bt_stack.cpp`) |
| Streaming: as many SBC frames per packet as the MTU allows (at most 8), the queue cut to 50 ms, sending paced to the clock | Low delay without bursts; silence fills the gaps when the capture has nothing |
| SBC only, joint stereo preferred, bitpool 40 within the device's range, 48 kHz preferred, linear resampling to 44.1 kHz | Every audio device speaks SBC; the parameters are simple and were enough in the soak test (12d) |
| libsbc is an unmodified submodule; `src/compat/config.h` supplies its build settings | Upstream code stays upstream; it is not tested here |
| A small HTTP/1.0 server of our own, with a JSON writer; a thread per connection, at most 8, `503` when full; `Access-Control-Allow-Origin: *` | The API is a handful of routes; the launcher frame has the origin `null` and needs CORS; the connection slot is given back before the socket is closed so a client never meets a full server |
| One control page, embedded in the ELF (`tools/embed.py`) | Nothing else to deploy; the launcher menu stays small (section 15) |
| Notifications are sent straight from the calling thread (`sceKernelSendNotificationRequest`) | There are only a few per session (start-up, "Streaming to ...", failures); a queue and a worker thread with a 400 ms hold were not worth it. If the console is found to drop messages sent close together, a queue can come back (git history) |
| Log: a ring of 300 lines for the page and a file flushed once a second, turned over at 512 KB; nothing in the kernel log; time stamps in UTC | The page can show the log without reading a file; the file survives a crash and cannot grow without limit; the console's kernel log stays readable |
| One copy at a time (`flock` on `/data/bluev/bluev.lock`); a newer copy finds the old one by process name (`thr_set_name`/`sysctl(KERN_PROC_PROC)`) and asks it to quit (`SIGTERM`) | Sending the ELF again replaces the running program; finding it by name works even before its web server is up, unlike probing it over HTTP |
| A fixed web server port (7733), not a range with a fallback | The home screen entry always names 7733; a fallback port made it silently stop matching. `start()` already retries every 2 s, which also covers the port being briefly held by something else |
| The home screen entry is registered through the system installer, with a marker file (section 16) | It is retried only when it was not accepted before |
| Host build with CMake (plain, thread and address sanitizer), console build with the Makefile | The two builds have different compilers and flags; the host build is where the tests live |
| `clang-format` (Microsoft style, pointers on the left, braces always) on everything but `third_party` | One style, no review noise |

### 17.2 What we tried or considered and did not choose

| Idea | Why not |
|---|---|
| Read the gamepad's state from the system (`scePadOpen`, libSceMbus) to open the radio only when no pad uses Bluetooth | `scePadOpen` is refused for our process (`0x809b0081`), Mbus answers nothing useful; getting past it means changing the process's credentials, judged too risky (12, 12a) |
| Sniff the USB endpoints to learn the pad's state | Reading is what breaks the console's stack (12) |
| Survey the console's kernel structures to find who owns `/dev/bt` and `/dev/bluetooth_hid` | The survey crashed the console; no kernel memory probe again |
| Detach the console's kernel driver or take the chip over | Breaks the gamepad; ruled out |
| A run-time knob for the number of armed reads | Used for the measurements, then removed (12) |
| Detect the endpoint roles by behaviour on the console's chip | Replaced by the descriptors (12c) |
| `libusb` for the HCI traffic | Works, with no advantage over ugen, and synchronous reads with timeouts split events (12c) |
| Interface 3 of the console's chip as a second controller | It refuses the HCI commands (12c) |
| Starting AVDTP ourselves at once to save the 9 s the speaker waits | Interferes when the speaker drives; see the AVDTP row above |
| Shorter page rounds and a longer listen window to speed up Connect | Noted and deliberately not applied: the speaker mostly connects to us by itself during the listen window (see 18) |
| One front end for the launcher menu and the web page | The launcher can only draw its own list (`showCarousel`), not HTML; the home screen entry opens the same page in the console's browser. A menu described by the server (`/api/ui`) was designed and dropped because the page already does everything |
| A confirmation window before starting the internal radio | Replaced by the warning text only, on the page and in the launcher menu |
| Compute the NIDs from the function names (SDK `nid_encode`) | Its implementation is not in the SDK libraries; the 8 NIDs stay as strings, each next to its name |
| Test `src/platform/ps5/` on the PC | It cannot run there; it is compiled by the console build and checked on the console, with the external adapter |
| Convert `tools/usbprobe` (C) to C++ | A separate diagnostic payload; not worth it |

## 18. TODO

Product

- Remember settings: the last adapter, starting the radio at boot, reconnecting the last device.
- Media keys and volume (AVRCP): the L2CAP channel for it (psm `0x17`) is refused and the SDP record only describes the audio source.
- Choose the SBC quality and the sample rate; a better resampler than the linear one.
- A faster Connect. Measured: 43 s (2026-10-18) and 1 min 45 s with a fresh pairing (2026-10-21), mostly paging that gets no answer and the 20 s listen window during which the speaker connects to us. First candidate: shorter first page round (about 2-3 s gain), then a longer or continuous listen window.
- A launcher menu with the same abilities as the page (external adapter, log) if it is ever wanted.

Security

- The API has no authentication and answers every origin; anyone on the network can control it. Options: listen on the loopback address only (the home screen entry and the launcher need nothing else), a switch for the local network, or a token.

Console and hardware

- Run the transport and USB directory after the move to the SDK's USB structures on the console (external adapter first). Nothing in them is tested on the PC.
- Other adapters: only the Realtek USB family has a firmware uploader (`core/bluetooth/firmware.*`). Broadcom `.hcd` (`0xfc4c`/`0xfc4e`), Intel `.sfi`/`.ddc` and MediaTek would each need their own loader. Broadcom-family sticks with the vendor-specific class (`ff/01/01`) are untested; the descriptor lookup only accepts `e0/01/01`.
- Longer runs than 21 minutes, other speakers, and the capture running together with Remote Play.
- The Sony WH-CH720N does not work on the internal chip (it works on a USB adapter); the Creative speaker does. Find out why.
- Only a PS5 Slim was tested; reports from a PS5 Pro and the fat PS5 are wanted (README, Contributing); other models lay the chip out differently (the `0x82/0x01` layout in the code comments) and the home screen entry's `AppInstallTitleDir` is missing on firmware 12.40.
- On-screen notifications start with "Debug:". Tried (2026-10-24): the request laid out as 11 ints, `useIconImageUri = 1`, `targetId = -1` and the icon `cxml://psnotification/tex_icon_system`, which is reported to remove the label; the console still showed "Debug:". Find which fields give a plain toast (`src/platform/ps5/notification.cpp`).
- Open ideas for the shared chip: read the console's own copy of the events instead of taking them from USB; what interface 3 carries.

Code and tests

- Make `UgenTransport` testable by putting the ioctls behind a small interface, so that the ring and the packet splitting run on the PC.
- `AvcapSource` and the other console adapters have no host tests.
- A CI job: host tests (plain, thread, address), the console build and a `clang-format` check.
- `WithRadioOn.ManyClientsAtOnceGetValidAnswers` failed once in a plain run and one thread-sanitizer run failed once; neither came back in many repeats. Find the cause.
- Releases are built by hand with `make clean release` (see the README). The source is GPL-3.0-or-later (`LICENSE`), so a release has to offer the source with it (the tag's source archive does). Before the first one, check the SDK's own `LICENSE` in its repository for a linking exception.
