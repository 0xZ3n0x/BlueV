# Bluetooth stack

The USB transport, controller setup, connections, L2CAP and SDP.

## 4. HCI transport (`core/bluetooth/usb_hci_transport.cpp`, `platform/ps5/ugen_device.cpp`)

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

`UsbHciTransport::detectRoles` (the fallback) runs once at startup (from
`BtStack::start`): it opens with the fat
layout, sends `Read Buffer Size` (`0x1005`) every 1.5 s and watches on which IN
pipe the Command Complete arrives. If it arrives on the second pipe the layout
is the Slim one: the device is closed, the standard layout (`0x81`, `0x82`,
`0x02`) is selected with `select()` and it is reopened. The result is kept for the lifetime of the
process; nothing is re-detected per connection.

- HCI **commands** go out as control transfers (`USB_FS_SETUP`,
  `bmRequestType=0x20`) in `UsbHciTransport::command`.
- `UsbHciTransport::pump(timeout)` restarts idle reads and `reap()` collects finished
  transfers. Consecutive packets inside one transfer are split into two
  queues (`HciPacketQueue` in `core/bluetooth/`, events and ACL, 32 slots each);
  the oldest packet is dropped when a queue is full. The queue and the test for
  the Command Complete that role detection waits for hold no kernel call
  (`tests/unit/hci_packet_queue_test.cpp`).
- **The split between `UsbHciTransport` and `UgenDevice`.** Everything the
  transport decides is in `core` and runs on the PC: how many reads are opened
  (48 events, 16 ACL; a transfer that will not open ends the loop, and fewer reads
  are enough as long as there is one of each), when an idle read is started (at the
  top of every `pump`, so a read that completes is started again by the next
  `pump`, not before), what an error or an empty read counts as (counted, nothing
  queued), the 1 s wait for the single output transfer, the order of closing
  (stop every transfer that is running, then take the table down). Only the
  calls to the kernel are in `UgenDevice`, behind `IUsbDevice`
  (`core/ports/usb_device.hpp`): the table of 65 transfers with a 0x44c byte buffer
  each, `USB_FS_START/STOP/COMPLETE`, the control request, and the wait (`usleep`).
  A test plays the kernel with `FakeUsbDevice` (`tests/support/`), finishing
  transfers by hand and moving the clock when the transport waits
  (`tests/protocol/usb_hci_transport_test.cpp`).
- `UsbHciTransport::sendAcl` uses a single output transfer and waits up to 1 s for the
  previous one.
- The system's own traffic passes through the same controller, so replies to
  other parties' commands are counted and the first 50 are logged, not
  processed.
- Diagnostics: per-endpoint counters (`UsbHciTransport::describe`), logged when
  the transport closes.

## 5. Controller setup (`BtStack::start`)

1. `IHciTransport::open`; failure means "radio busy".
2. Role detection and reopen ([section 4](#4-hci-transport-corebluetoothusb_hci_transportcpp-platformps5ugen_devicecpp)).
3. HCI Reset for an external adapter; the console's own chip is never reset.
4. Adapter firmware ([section 5a](adapter-firmware.md#5a-adapter-firmware-corebluetoothfirmware-realtek_firmware)): `Read Local Version`, and for a Realtek USB
   chip still in ROM mode upload its patch, then reset again.
5. `Read Buffer Size` gives the ACL size and credit count (default 7 x 1021
   when there is no answer; the Slim reports 8 x 1021).
6. `Set Event Mask`, Secure Simple Pairing on, `Write Inquiry Mode` (`0x0c45`,
   value 2: results with RSSI and EIR, which carries the device name),
   plus a few read-only commands whose replies are logged for diagnostics.
   An adapter also gets Class of Device `04 01 00` and the local name `BlueV`;
   the console's chip keeps its own.
7. `drop_stale_links`: on an adapter, scanning off, then `Disconnect` for handles
   1-8 to clear links left behind by a killed payload. On the console's chip only
   our own link is dropped, because those handles can be the gamepad's.
8. Page scan on, so a paired device can connect to us. The console's chip gets
   back the setting its stack had (`restoreScan`), or page scan when that was off.

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

**On the console's chip** the stack's own scanning competes with us for radio time,
so page and inquiry scan are off for each 4 s Create Connection and back on for the
listen window (a device can only reach us with page scan on), and a scan pauses
inquiry scan only, so a gamepad can still reach the console. A copy killed during
that pause leaves page scan off until the next start. Adapters are left as they were.

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
effect of [section 12](shared-chip.md#12-the-controller-is-shared-with-the-console)). With the master role (confirmed with `Role Discovery`,
logged as `link role: ... we are the master`) the same setup ran 4.5 minutes
with a game and audio and no loss, so far one run: not proven.

**`encrypt_link`**: waits for authentication (30 s), then for the link to
encrypt itself (0.5 s for a device we have a key for, 2 s after a new pairing)
and forces it with Set Connection Encryption (20 s). The WH-CH720N and the Redmi
Buds never encrypt by themselves, the Creative speaker does within 0.2 s; the
wait used to be 2 s for all and cost every connection that long (2026-10-30). A
failed authentication with a stored key deletes that bond. The link's packet
types are left to the controller: forcing multi-slot types with Change
Connection Packet Type right after encryption was tried on the Slim and
coincided with the speaker dropping the link a second later (reason 0x08)
in most sessions, and with it removed 4 of 4 reconnects were clean.

**Pairing**: PIN request answered with `0000`, IO capability
`NoInputNoOutput`, user confirmation answered automatically. The link key
notification writes `bond-<ours>-<theirs>.key` (not saved for TV-like devices).

**Timing in the log**: where the time of a connection goes can be read from
three lines, each with seconds to one decimal (every log line also carries the
time of day):
- `connect: paging N s, security N s` (`BtStack::connectTo`; `(failed)` is added
  when the security step failed, and `connect: no link after N s` when paging
  gave up) or, for a device that dialed in, `connect: security N s`. Paging
  includes the listen windows between the rounds.
- `connect: link N s, audio N s, until the stream N s` (`Service::runSession`,
  just before streaming starts). The audio part is the AVDTP setup and, for a
  device that dialed in, the time it took to start the procedure.
- The lines of the rounds themselves (`connection: no answer (try N)`) and of
  AVDTP ([section 9](audio.md#9-a2dp-setup-a2dpsessionstart-a2dpsessiononsignal)) show what happened inside those totals. Every AVDTP
  signal is logged with its first bytes, ours as `avdtp: > command 0x01 label N: <hex>`
  and the device's as `avdtp: < command|answer ...` (`unreadable` for a frame
  that is not a single packet), so a device that stays silent shows as a `>` with
  no `<` after it.

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
    `bindInbound` (AVDTP `0x19` and AVCTP `0x17` are bound while idle, see
    sections [9](audio.md#9-a2dp-setup-a2dpsessionstart-a2dpsessiononsignal) and [9a](audio.md#9a-the-headsets-volume-avrcp-coreaudioavrcp); two PSMs at most, binding a PSM again replaces its channel).
    A configure request of ours is repeated every 6 s with a new id. On the
    console's chip the answer can be slow or go to the console, so it is repeated
    every 4 s with the same id (a new id can read as a second configuration), never once the device sent its own, and 4 s
    after that ours is taken as accepted. Channels the device opens are not covered:
    their single request is not repeated. The device's own configure request can be
    lost the same way, which leaves the channel half open on its side: when none
    came 3 s after ours was accepted, the channel is closed and asked for again,
    once (`reopenChannel`). Every signaling command on this chip is logged
    (`l2cap: >` and `l2cap: <`, with its bytes).
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
Service Attribute and Service Search Attribute PDUs. Two records:

- `0x10001`: service class A2DP Source (`0x110a`), protocol list (L2CAP/AVDTP),
  profile (`0x110d`), name `BlueV`. Found by `0x110a`, `0x110d`, `0x0019`,
  `0x0100` and `0x1002`.
- `0x10002`: service class A/V Remote Control (`0x110e`), protocol list
  (L2CAP psm `0x17`/AVCTP 1.4), profile AVRCP 1.4, supported features
  category 1. We are only the controller, and only for the volume ([section
  9a](audio.md#9a-the-headsets-volume-avrcp-coreaudioavrcp)). Found by `0x110e`, `0x0017`, `0x0100` and `0x1002`.

A pattern matches a record when it names any one of the record's UUIDs, so a
search for L2CAP or the public browse group finds both records.
