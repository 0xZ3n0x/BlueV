# The console's chip

What sharing the chip with the console means, and what was tried and measured.

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

On a PS5 Pro (CFI-7119, MediaTek `0e8d:3605`) the chip is shared the same way:
the 2026-11-02 runs show the same `ACL from handle ... before a link of ours`
and 2-8% of media packets without a completion report (`reports missing`).

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

The console's stack also writes its own event mask while it works, and that
switches the inquiry results off for whoever reads the events. The command
answers and the completion still come, because those cannot be masked, so a
scan looks alive and finds nothing. BlueV sends its mask again before a scan on
this chip, and again when the scan hears nothing for 3 s
([section 6](bluetooth.md#6-connections-and-pairing-corebluetoothbt_stack_linkcpp-bt_stack_inquirycpp));
without that, finding a new device is a matter of luck.

What the first versions did about it: **nothing.** The radio was opened at
start and read continuously, so a controller must not connect, disconnect or
reconnect while the program ran. Since 2026-11-23 the reads wait only while
something of ours needs an answer ([12f](#12f-reads-only-while-something-needs-them-2026-11-23)),
which is much gentler but still not proven harmless. Switch the controller on before the
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

The limitation was accepted on 2026-10-31 (later work: [12f](#12f-reads-only-while-something-needs-them-2026-11-23)). After the changes that leave the chip's name,
class, scan setting and other links alone (sections [5](bluetooth.md#5-controller-setup-btstackstart) and [6](bluetooth.md#6-connections-and-pairing-corebluetoothbt_stack_linkcpp-bt_stack_inquirycpp)) a gamepad switched off and
on while streaming still did not come back. The page and the README tell the user to
leave it alone; a USB adapter is the way to avoid it. Fewer armed reads while streaming,
or none, was not tried; reading the console's copy through `/dev/bt` was measured and
does not work ([12e](#12e-listening-to-devbt-2026-10-30)).

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
  console's stack, with the limits of [section 12](#12-the-controller-is-shared-with-the-console).
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

**Sound quality** (`quality=0|1` in the same file, `POST /api/quality`, the page's "Sound quality" box): 0 asks the headset for SBC bitpool 40 (93-byte frames at 48 kHz joint stereo, about 279 kbit/s), 1 for bitpool 53 (119-byte frames, about 357 kbit/s), in both cases limited to the range the headset offers (a headset that stops at 39 gets 39 either way). It applies from the next connection; `A2dpSession` reads it when it picks the parameters, whether we drive the setup or follow the headset's. The bigger frames mean fewer fit in a packet (two instead of three on the CSR adapter's 310-byte buffers) and more packets per second, which needs more ACL credits: it is on the page as "use it only if the sound stays clean". Normal stays the default. High ran without dropouts on the USB adapter (about 5.7 minutes with the WH-CH720N, about 80 seconds with the Creative; 172-188 packets/s, queue under 5 ms, credits 5-10) and is untested on the internal chip (few credits).

**Who can open the page** (`network=lan|local`, the page's "Who can open this page" box): `lan` is the default and the old behaviour, `local` binds the server to `127.0.0.1` only. The page asks for confirmation before it closes itself to the PC, and the notice at start-up then says to use the home screen entry. The change is made by `Service::applyNetwork()`, which stops the server (waiting for the request that asked) and starts it again; it runs from the loop when nothing is going on and from the once-a-second tick during a stream, where it can cost a short gap in the audio (the stop waits up to 200 ms for the accept thread).

**Starting at launch** (`Service::autostartRadio`, `service/settings.*`): with
`autostart` ticked on the page (only offered while a USB adapter is in use), the
file `/data/bluev/settings` holds `autostart=1` and `adapter=<vendor>:<product>`
(hex), next to the `quality` line. When BlueV starts it looks for that adapter among the USB devices, by
vendor and product because the `ugenX.Y` node changes with the port, and opens
it as if the user had pressed Start; a paired headset that is then switched on
connects by itself (about two to three seconds on the headsets tried). If the
adapter is not plugged in the radio stays off and a message says so. The
console's own chip is never started this way: its start is by hand, with the
warning of [section 12](#12-the-controller-is-shared-with-the-console). BlueV does not page the last headset itself; the
headsets tried all connect to us on their own. The file is written beside
itself and renamed, so a crash leaves the old one whole; a missing or damaged
file means the defaults (nothing is started).

`POST /api/radio/off` stops the radio again. Scan and Connect are refused
(409) while it is off; Forget only touches files and still works.

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

## 12e. Listening to `/dev/bt` (2026-10-30)

`/dev/bt` is the console's own notification device, used by `libSceBluetoothHid`. It
looked like a way to learn about a gamepad without touching USB, so a throw-away
listener (not kept) tried it on the Slim.

A payload can open it, and an 8 byte write sets that client's event mask. Events
are 16 byte records with a type byte and an 8 byte id, one copy for every client.
Writing the mask changed nothing for the gamepad.

The first run (mask only) showed no records during a gamepad off/on cycle. A device's
records reach only a client that registered its code pair with ioctl `0x8004622b`:
`044f/1000` worked for this DualSense, `06f8/1faf` was refused with `0x802f3701`.
With the pair registered, one open carried the whole lifecycle:

| byte | meaning | seen |
|---|---|---|
| `05` | gamepad connected | once, at switch-on |
| `08`, `12` | connect sequence | just before the stream starts |
| `22` | gamepad in the console's connected list, ~53 per second | all the connected time |
| `06` | gamepad disconnected | once, at switch-off |

A third run, with BlueV streaming on the internal chip and the gamepad switched off
and on, produced 18 751 records in 354.5 s, every one of type `22`, at 52.9 Hz. No
`05`, `08`, `12` or `06` appeared and the gamepad never became usable. So `22` is not
a radio heartbeat but the console repeating that the device is still in its list.
It keeps coming while the gamepad is off because the console never saw the disconnect:
our armed reads took it, and with no `06` there is nothing to react to. Reacting to
`/dev/bt` therefore does not work.

## 12f. Reads only while something needs them (2026-11-23)

Every packet that comes in on the chip's USB endpoints goes to exactly one waiting
read, ours or the console's ([section 12](#12-the-controller-is-shared-with-the-console)).
So the question is how many reads of ours wait, and when. All of this is the PS5 Slim
(CFI-2116, Marvell chip `1286:2059`, 7 ACL buffers of 1021 bytes) with an Amazon Echo Dot
as the speaker and two DualSense gamepads, unless said otherwise. Every row is one run
(or a few); none is a statistic.

| What waited | Result for the gamepad |
|---|---|
| 48 event + 16 ACL reads all the time, the way it was built | A gamepad link timed out during a stream, or a gamepad switched off and on did not come back; the session's totals were 2400 ACL packets and 200 events taken in 12 minutes, mostly the gamepad's own |
| Full reads for the setup, then 1 event read and no ACL read while streaming | A gamepad switched off and on did **not** reconnect. The log shows the replies to the console's own `Write Link Policy`, `Disconnect` and `Reject Connection Request` coming to us. One read still wins about half of the replies, and a console command whose reply is taken leaves its stack waiting |
| The same, then the radio turned off | The gamepad still did not reconnect: the damage stays after we stop reading |
| Full reads for the setup (about 7 s), then **no** read while streaming | A gamepad switched off and on **reconnected**, with the stream at the full rate (once) |
| All reads again during a stream (a control) | A gamepad broke within a minute without being touched |
| No read while streaming, gamepads connected | A gamepad later froze (connected, a button stuck, no input) and had to be switched off and on until it worked; another run lost a gamepad and froze the other during a stall of the whole chip (below) |

What is done now (the service, `Service::runSession` and friends, through
`IRadio::setReadsActive`):

| Moment | Reads |
|---|---|
| Opening the radio, closing it | on |
| Scan | on |
| Connect: paging, pairing, encryption, AVDTP | on (about 7-9 s) |
| Hanging up (`stopAudio`, `disconnect`) | on |
| Radio on and idle | **off** |
| Streaming | **off** |

The `ACL from handle ... before a link of ours` lines and the `other links` counter of the
stream status show what the setup took: 931 and 386 packets in two connects with a gamepad
connected, 692 with two connected, 0 with none. Other changes that came out of this:

- The 10 s freeze at the moment the reads were cut was not the stops (a stop takes 0 ms)
  but the volume-control channel waiting for an answer
  ([section 9a](audio.md#9a-the-headsets-volume-avrcp-coreaudioavrcp)).
- A command sent while no event read waits has its answer taken by the console's own stack
  (the 10 s `Read RSSI` and `Read Link Quality` probe did that to it for hours); the probe
  now only runs while events are read.
- With the radio on and idle nothing hears a speaker that dials in by itself: press
  **Connect** on the page. The service no longer polls the controller while idle.
- `POST /api/reads?events=N&acl=M` (-1 = all) sets what a stream keeps waiting, for
  experiments; the default is none.

**Rough patches.** Whatever the reads, the stream has patches in which the controller stops
taking packets for 10-40 s: the bitpool falls to 28, up to a few seconds of audio are cut,
the back-off pauses, and it recovers by itself ([section 10](audio.md#10-audio-streaming-loop-a2dpstreamerrun)).
They also happen with **both gamepads off** and nothing taken (`other links 0`): about 1.4%
of the audio in 4 minutes. Two gamepads connected and left alone: 0.85% in 10 minutes, one
patch. Gamepads breaking (disconnecting, freezing, "unsupported device" on reconnecting)
were noticed in or right after such patches in most runs (not every patch broke one, and
a gamepad switched off and on by hand has its own failures). Not the signal (RSSI -6
to -12 dBm, speaker next to the console), not the reads (none waited), not the loop (never
more than 20 ms), not the console's own Wi-Fi band (it was on 5 GHz; a shared antenna on the chip is not
excluded). Unproven guess: the
console's stack pages a bonded gamepad that is not connected, the chip's radio is busy
doing that, and the speaker's link starves; a gamepad being switched or reconnecting
meanwhile fails. What would show it: a stream with a gamepad switched off once, and a
console with no bonded gamepad at all.

**Recovery.** A gamepad that is connected but frozen came back when it was held off and on
again (it disconnected by itself once on the way) with BlueV's radio off. When it did not,
putting the console into rest mode and waking it cleared the console's Bluetooth every
time it was tried (several times, a few of them after a gamepad was refused as an
unsupported device). Turning the radio off alone did not.

**What to tell the user** (README): connect the speaker first, then play; keep the gamepads
connected and do not switch them while streaming; a USB adapter avoids all of it.

**Where the causes stand (2026-11-24).** What was found, how sure it is, and what is left:

| Problem | Cause | How sure |
|---|---|---|
| Crackle and dropouts of the first versions | The link kept DM1 packets (the compatibility page attempt) and the stream loop waited inside the blocking USB write: 5.5 packets of 47 needed, about 88% of the audio dropped | Proven: the rate went to 47 a second as soon as the packet types were allowed |
| A gamepad frozen, "unsupported device", not reconnecting | Reads of ours that wait take the console's own packets: replies to its HCI commands, a gamepad's connection requests. The console's stack then waits for an answer that went to us, and stays stuck until it is reset (rest mode) | Proven (one waiting read was enough, the control with all reads broke a gamepad in a minute, the replies of the console's commands show in the log). Removed while streaming; during the 7-9 s of a connect it remains |
| A controller refusing to connect or reconnect while a stream runs, with nothing being taken | The chip's one radio does not serve a new connection while it is busy with the stream; each such attempt came with a stall of the whole chip (6-12 s) | Likely, not provable from a program; it happens in the chip's firmware |
| Rough patches of 10-40 s in the stream, 0.85-6% of the audio | Not found: they happen with no gamepad, no read waiting, with the network on or off, and vary a lot between runs (6% in one 28 minute run, 0.85-1.4% in four others) | Unknown; the Echo Dot's own radio and the chip's scheduling are the candidates. A different speaker would separate them |

So on the console's own chip nothing of ours can make a gamepad connect during a stream
and nothing can be fully safe: the console's driver owns the chip and there is no way for
a second user of it to take part. Two ways out were looked at and not taken: giving the
packets back to the console (a hook in its kernel driver: nothing in a payload reaches it
without reverse engineering the kernel, and another project's survey of those structures
crashed a console, see [design.md](design.md#162-what-we-tried-or-considered-and-did-not-choose)),
and the console's own Bluetooth stack (it has no A2DP, AVDTP or SBC at all, found by another
project in the decrypted system software). What is left is the way of using it that has
worked: gamepads connected first, then the speaker, no gamepad connecting meanwhile (press
Disconnect for the speaker first); a gamepad on the USB cable takes its Bluetooth out of it.
A USB adapter, which this program supports, has none of these problems.
