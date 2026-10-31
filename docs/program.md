# The program

How the program starts, its web interface, logging, threads and the home screen entry.

## 1. Top level (`app/main.cpp`, `service/service.cpp`)

```
log file -> lock (flock)
  -> AvcapLibrary::load, AvcapCapture::start   prepare audio capture
  -> Service::run
       HttpServer::start HTTP server thread
       loop: idle / scanning / connecting / streaming, driven by commands
             and by headsets dialing in; the radio (BtStack::start) is
             opened when the user starts it ([section 12b](shared-chip.md#12b-starting-the-radio))
  -> AvcapCapture::close
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
- Files in `/data/bluev/` (`main.cpp`, `FileBondStore`, `SettingsFile`): `bluev.log`,
  `bluev.lock`, `settings` ([section 12b](shared-chip.md#12b-starting-the-radio)), and one `bond-<ours>-<theirs>.key` (23 bytes: address 6, link
  key 16, key type 1) plus `bond-<theirs>.name` per paired device ([section 12b](shared-chip.md#12b-starting-the-radio)).
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
it comes up once the network does). No authentication: by default it listens on every address, so anyone on the local network can use it; the page can limit it to this console (`POST /api/network`, below). A client that hangs up before the answer is written must not end the program: answers are sent with `MSG_NOSIGNAL` (`SO_NOSIGPIPE` on macOS), because the default `SIGPIPE` would kill the whole payload.

| Endpoint | Purpose |
|---|---|
| `GET /` | the embedded control page |
| `GET /api/status` | state, message, current peer, the headset's `volume` (percent, -1 when it has none), stream numbers (rate, packets/s, queue) |
| `GET /api/devices` | scan results plus paired devices, with `bonded`/`connected` flags |
| `GET /api/log?since=N` | log lines from the ring buffer (300 lines) |
| `POST /api/scan` | start a 30 s inquiry |
| `POST /api/connect?addr=AA:BB:CC:DD:EE:FF` | connect to a device (pairs it when no bond exists) |
| `POST /api/disconnect` | end the stream and drop the link |
| `POST /api/forget?addr=...` | delete a device's bond files |
| `POST /api/autostart?on=1\|0` | open the radio with the USB adapter in use when BlueV starts ([section 12b](shared-chip.md#12b-starting-the-radio)); needs the radio on with a USB adapter, `409` otherwise; `on=0` always works; `/api/status` shows `radio.autostart` |
| `POST /api/network?v=lan\|local` | who can open the page: `lan` (the default) every address of the console, `local` only the loopback address, which the console's own browser (the home screen entry) uses; saved in the settings file (`network=`); the radio thread starts the server again right after the answer, so the answer still arrives, and a PC or phone is shut out from then on; `/api/status` shows `network` |
| `POST /api/quality?v=0\|1` | the sound quality of the next stream, 0 normal (bitpool 40), 1 high (bitpool 53); saved in the settings file; `/api/status` shows `quality` |
| `POST /api/volume?v=N` | set the headset's volume, N = 0..100 percent; only while streaming to a headset that reports a volume ([section 9a](audio.md#9a-the-headsets-volume-avrcp-coreaudioavrcp)) |
| `POST /api/quit` | stop the service |

State-changing calls are `POST` only. `scan`, `connect` and `forget` are
refused unless the service is idle.

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

## 14. Threads, ownership and tests

| Thread | Does |
|---|---|
| radio (main) | everything in `core/bluetooth/` and `core/audio/`, the service state machine |
| web accept + one per request | HTTP only; touches the radio never |

Rules that keep this race-free:

- Radio state is owned by the radio thread. Other threads reach it through
  three boundary objects only: the pending command and status snapshot in
  `Service` (one mutex), the log ring (`LogBuffer`, its own
  mutex), and a few atomic flags and values (`m_quit`, `m_disconnectRequested`,
  `m_restartServer`, `m_askedVolume`). The
  streaming loop publishes its numbers with `Service::publishStream` (a few
  times a second, under the service mutex) and polls an atomic stop flag.
- ThreadSanitizer (`tests/run.sh thread`) runs the service tests, including 8
  clients firing random requests, to catch races.
- Nothing on the radio thread sleeps or waits for I/O other than the
  controller itself.

Tests: `tests/run.sh [plain|thread|address] [gtest filter]` builds everything
except the PS5 adapters on a PC (CMake + Ninja, googletest submodule) and runs
about 350 tests: the Bluetooth stack against a fake controller and a fake
speaker (pairing, paging, L2CAP, flow control, AVDTP, the streaming loop), the
service against a gmock radio over real HTTP (including 8 clients firing random
requests), and the file/log/notification helpers. The PS5 adapters under
`src/platform/ps5/` are compile-checked by the console `make` only.

## 15. Home screen entry

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
