# Audio

Capture, A2DP setup, the headset volume and the streaming loop.

## 3. Audio capture (`core/audio/avcap_capture.cpp`, `platform/ps5/avcap_library.cpp`)

- **The split.** `AvcapLibrary` (platform, console only) loads the library, finds the
  entry points and passes the calls on, building the parameter block of OpenAudio;
  everything that is decided is in `AvcapCapture` (core) behind `IAvcap2`
  (`core/ports/avcap.hpp`), which a test plays with `FakeAvcap`
  (`tests/unit/avcap_capture_test.cpp`).
- `sceKernelLoadStartModule` loads `/system/common/lib/libSceIpmi.sprx` and
  `libSceAvcap2.sprx`.
- The `sceAvcap2*` functions are resolved by NID with
  `kernel_dynlib_resolve(pid, handle, NID)` (Initialize, Terminate, OpenAudio,
  Close, Start, Stop, ReadAudio).
- `AvcapCapture::start`: Initialize -> OpenAudio (2 channels, all devices) ->
  `Start(handle)`. Everything is undone on failure (a step that fails takes back the
  ones before it, in the opposite order, and only those that were done); a changed error code is
  logged ("not available yet" is normal when no game or audio exists yet).
- `AvcapCapture::read` returns records of 8-byte (L/R float) frames; `0` means no new
  data or a ring overrun (the first 5 are logged), `-1` an error.
- `AvcapCapture::restart` closes and reopens; this is how audio comes back after a
  game exits and starts again. Capture is opened once for the whole service
  lifetime and keeps running between streams.

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
| Get Capabilities, Get All Capabilities | accept, media transport + SBC (the same list under the signal id asked for) |
| Set Configuration | accept; the headset's SBC parameters are adopted |
| Open / Start | accept |
| anything else | general reject |

Get All Capabilities (`0x0c`, AVDTP 1.3, which our SDP record claims) was
rejected until 2026-10-30. The WH-CH720N answers a reject by asking Get
Capabilities instead; the Redmi Buds 4 Lite do not, and stayed silent until we
gave up after twenty seconds, so a reconnect they started failed every time
(every time, and the AVDTP trace of [section 6](bluetooth.md#6-connections-and-pairing-corebluetoothbt_stack_linkcpp-bt_stack_inquirycpp) shows the sequence).

When the headset made the connection, `A2dpSession::start` first waits up to ten
seconds for its signaling channel: it opens it itself, and a request sent at the
same time is refused with "no resources" (the 2026-11-02 log shows that refusal
ending link after link). When **we** made the connection it waits
one: the Sony WH-CH720N hangs up about eight seconds after a connection the
host made if the host starts nothing, the WH-1000XM5 after two to three, and
the Creative speaker only opens the channel after that. After the second we
open the channel ourselves
(`avdtp: signaling channel opened by us`), take over after 1.5 s of silence
instead of four, and fall back to waiting up to 17 s for the headset if our own
attempt fails. With this both devices connect from our side in about ten seconds (an
external adapter) and the stream starts at 48 kHz.

`start` waits for the headset's Set Configuration (as long as the headset
keeps sending commands, however slowly; it is left to us only after four silent
seconds, and never waited for longer than twenty: its pause between commands has
been seen to reach two seconds, and a fixed four second window made both sides
configure at once), derives the encoder
parameters from it with `chooseA2dpParams`, waits 3 s for the headset's Open and
sends the Open itself when it does not come (the 2026-11-02 log shows the headset
configuring the stream and waiting for us), waits for the media channel
(inbound, up to 6 s), then waits 300 ms for a Start from the headset
and sends `START` itself if none came. PSM `0x19` is bound to the media channel
as soon as the signaling one is up, so a headset that opens its media channel
early cannot land it on the signaling channel.

**Source-driven** (used when the headset starts nothing
within 4 s): open the signaling channel, `DISCOVER`, `GET_CAPABILITIES` for the
sinks, `chooseA2dpParams`, `SET_CONFIGURATION`, `OPEN`, media channel (the
headset opens it within 1.5 s, otherwise we do), 300 ms, `START`. Start must be
accepted: an unfavourable answer ends the setup instead of streaming into a
dead endpoint.

`chooseA2dpParams(caps, quality)`: 48 kHz preferred, else 44.1 kHz; Joint Stereo
preferred, else Stereo; blocks 16 > 12 > 8 > 4; 8 sub-bands when supported;
Loudness allocation when supported, else SNR; **bitpool 40** (normal) or
53 (high, [section 12b](shared-chip.md#12b-starting-the-radio)), in both cases clamped to the sink's range.

`A2dpSession::stop` sends `CLOSE` and closes the media and signaling channels.
`a2dp_reset` clears the channels and all headset-driven state between
sessions.

**Lost answers on the console's chip.** A run with the WH-CH720N (2026-10-31) showed
AVDTP answers that never reached us: Open and Start came back only when asked again,
8 s later, and a repeated Set Configuration was refused with `0x13` (in use), which
ended that try. An ACL packet goes to the console about one time in 17 with our 16
reads. `A2dpSession::command` therefore asks once and waits (4 s on this chip, 10 s
on an adapter); an answer that never comes ends the setup. It is not asked again
and a lost answer is never read as an accept: the runs that did that are the runs
whose stream was broken (`reports missing` rising with every packet). A failed
setup is settled by connecting again (the page then says `audio setup failed;
press Connect again`).

The headset can also start the setup while we are asking it (seen in the same run: it
sent Set Configuration and Open during our Get Capabilities, and ours was refused with
`0x31`). On this chip that is followed (`followPeer`) instead of ending the try, and
its Open binds the media channel at once, so the channel it opens next does not land
on the signaling channel, which is still the one bound for that PSM. For the same
reason our own Open binds the media channel before it is sent: a device opens the
channel even when the answer to Open goes to the console.

**A media channel without the headset's configure carries no audio.** The
WH-1000XM5 run of 2026-11-02 showed `no configure request from the peer,
continuing anyway` followed by `reports missing` 516 in 534 packets: every media
frame was dropped on the headset's side. On the console's chip `openChannel` is
therefore asked to require the device's own configure for the media channel, and
the setup fails (for another Connect) instead of streaming into it. An adapter
keeps the old behaviour.

## 9a. The headset's volume (AVRCP, `core/audio/avrcp.*`)

**Status: works on the console with the WH-CH720N, the Creative Stage Air V2,
the Redmi Buds 4 Lite and the WH-1000XM5: the slider changes the volume audibly
and the headset's buttons move the slider and show the message. Not tried on a
headset without absolute volume (that path is covered by the PC tests only).**

What it does: shows the headset's own volume on the page, lets the page set it,
and announces changes made with the headset's buttons. It is the headset's
volume only: the console's volume and the captured PCM are never touched. A
headset that does not report a volume gets no slider and no message and
keeps its local buttons.

AVRCP absolute volume: we are the controller (SDP records `0x110e` and `0x110f`, [section 8](bluetooth.md#8-sdp-corebluetoothsdp_servercpp)),
the headset the target. The AVCTP channel (L2CAP psm `0x17`) is separate from
A2DP; whatever goes wrong here only means "no volume control", never a lost
stream.

- `Avrcp::listen` binds psm `0x17` while idle. Headsets open the channel
  themselves a few seconds after they have searched our SDP records for
  `0x110c` (ours now, [section 8](bluetooth.md#8-sdp-corebluetoothsdp_servercpp)) and `0x110e` (ours).
- `Avrcp::start`, after the A2DP media channel is up: notes that the link is
  ready and returns at once, so the stream is never held up. The headset searches
  our `0x110c` and `0x110e` records and opens the channel itself a few seconds
  later. While that search has been seen on the link (the SDP server notes it),
  neither `start()` nor `service()` asks for the channel: the request is refused
  with "no resources" and can stop the headset from opening its own (2026-11-02).
  Without a search, `service()` asks once after six ticks (about 6 s). The volume
  registration (`REGISTER_NOTIFICATION` for VOLUME_CHANGED) is
  sent as soon as the channel is up; its answer carries the volume 0-127, and
  each `changed` is answered by a new registration because a registration fires
  once. A headset that never opens it and refuses ours gives no volume on that
  link; nothing is retried, and the next link asks again.
- `Avrcp::service`, from the once-a-second tick: registers if the headset
  opened the channel late, and sends a volume `setVolume()` asked for
  (`SET_ABSOLUTE_VOLUME`, which the headset answers `ACCEPTED` with the volume it
  took). The page's slider can therefore lag by up to a second.
- `volume()` is -1 until the headset has told its volume; the page and the
  API treat -1 as "no volume control". It is cleared when the link goes.
- Nothing is remembered between links: every link asks the headset again, so a
  refusal that was only a bad moment costs one link at most. A headset that
  refuses the channel, or rejects or does not implement the registration,
  stays without a volume for that link (`volume()` -1, no slider, no message).
  The registration is sent once per link, so a rejection is not asked again.
- The headset sent commands of its own on the CH720N (`REGISTER_NOTIFICATION`
  for playback status and track change, `GET_PLAY_STATUS`) as if we were a
  player. They are logged and answered `NOT_IMPLEMENTED`; the link and the
  stream stay as they were.
- `Service::announceVolume`, from the tick: a change of the volume that is not
  the one the page asked for is shown as an on-screen message, "Headset volume
  N%". Once a second at most, so a button held down gives the last value. The
  first value after the link comes up is not announced.
- Log lines: `avrcp: the headset opened the channel`, `avrcp: channel opened by us`,
  `avrcp: headset volume N of 127` (when it changes),
  `avrcp: no volume control on this link (...)`, `avrcp: answer, type T pdu P: <hex>`
  for anything else.

Frames (`avrcp::buildRegisterVolume`, `buildSetVolume`, `parse`): AVCTP header
(label, single packet, command or response, profile `0x110e`), then AV/C
(command type, subunit panel `0x48`, opcode vendor dependent), the Bluetooth
SIG company id `00 19 58`, the PDU id (`0x31` REGISTER_NOTIFICATION, `0x50`
SET_ABSOLUTE_VOLUME), packet type and parameter length.

## 10. Audio streaming loop (`A2dpStreamer::run`)

The encoder is set up; at most 8 SBC frames go into a packet (bounded by the
headset's MTU and the ACL size). The loop runs until the link drops, the media
channel closes, or a stop is requested:

1. **Capture state.** Without capture, the queue is filled with wall-clock
   silence (so the stream does not stall) and `AvcapCapture::restart` is tried
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
