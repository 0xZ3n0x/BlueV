# TODO

Open work.

## 17. TODO

Product

- Remember settings: the USB adapter, starting the radio at launch and the sound quality are done ([section 12b](shared-chip.md#12b-starting-the-radio)); reconnecting the last device by paging it is not, and no headset tried has needed it.
- Headset volume ([section 9a](audio.md#9a-the-headsets-volume-avrcp-coreaudioavrcp)): the WH-1000XM5 works now (the slider was present on every stream of the 2026-11-02 PS5 Pro run); a headset with no volume control is not tried. Media keys (play/pause) are not wanted.
- The sample rate is not a choice; a better resampler than the linear one.
- A faster Connect. Measured with a USB adapter and a known device (2026-10-30): 5-8 s when BlueV connects, 2-3 s when the headset connects itself; the rest is the waits in the AVDTP setup ([section 9](audio.md#9-a2dp-setup-a2dpsessionstart-a2dpsessiononsignal)), which the WH-CH720N's 8 s timeout keeps from being cut. With a fresh pairing it took 1 min 45 s (2026-10-21). On the internal chip it took about 105 s; after the changes of sections [6](bluetooth.md#6-connections-and-pairing-corebluetoothbt_stack_linkcpp-bt_stack_inquirycpp), [7](bluetooth.md#7-eventacl-loop-and-l2cap-btstackpoll) and 9, 0.3-0.7 s of paging and 14-30 s to the stream (2026-10-31, WH-CH720N and Creative, a few connections). Since 2026-11-02 the page keeps the console's scan off for its whole length and no longer waits 20 s between rounds; the 2026-11-02 logs (PS5 Pro, WH-1000XM5) show 0.2-1.4 s of paging, with the stream 13.0-16.5 s after Connect, still mostly the AVDTP setup.

Security

- The API has no authentication. Done: a switch on the page limits it to this console ([section 2](program.md#2-web-interface-servicewebcpp-webindexhtml)). Not done: a token for the case where the network is open; no one has needed it.

Console and hardware

- Run the new transport (`UsbHciTransport` on `UgenDevice`) and the USB directory on the console with the internal chip; the external adapter has run them. The transport's decisions are tested on the PC, `Ps5UsbDirectory` is not.
- Other adapters: only the Realtek USB family has a firmware uploader (`core/bluetooth/firmware.*`). Broadcom `.hcd` (`0xfc4c`/`0xfc4e`), Intel `.sfi`/`.ddc` and MediaTek would each need their own loader. Broadcom-family sticks with the vendor-specific class (`ff/01/01`) are untested; the descriptor lookup only accepts `e0/01/01`.
- Longer runs than 21 minutes, other speakers, and the capture running together with Remote Play.
- The internal chip (2026-11-01/02; PS5 Slim and PS5 Pro CFI-7119, MediaTek `0e8d:3605`): on the Slim the Creative connected 2 of 2 times and the WH-CH720N 2 of 7, after 4 of 4 in an earlier run; in the same hour a USB adapter connected 9 of 9, so the headset is not the cause. On the Pro (2026-11-02, WH-1000XM5): a fresh pairing ended in one failed A2DP setup and the next Connect streamed; two power-cycle reconnects after that worked first try, with the headset opening the AVDTP signaling channel itself, and the volume slider was present on every stream. Left open on this chip: after a disconnect the old link stays until the controller's supervision timeout (`0x08`), so a Connect meanwhile fails with `0x0c`, and the old handle's ACL still arrives as `ACL from handle ... before a link of ours`; 2-8% of ACL packets still go missing (`reports missing`); a second device dialing in while the first link closes.
- The internal chip has been tested on a PS5 Slim and on a PS5 Pro (CFI-7119, MediaTek `0e8d:3605`); the fat PS5 is still wanted (README, Contributing). Other models lay the chip out differently (the `0x82/0x01` layout in the code comments) and the home screen entry's `AppInstallTitleDir` is missing on firmware 12.40.
- On-screen notifications start with "Debug:". Different request fields, types and devices did not remove it (2026-10-30); the toast is drawn by the shell's JavaScript, so it is left as it is (cosmetic).
- The shared chip: reacting to `/dev/bt` does not work, because the console only reports events it saw itself ([section 12e](shared-chip.md#12e-listening-to-devbt-2026-10-30)). Fewer armed reads while streaming was not tried. What interface 3 carries is unknown.

Code and tests

- The console adapters that remain (`UgenDevice`, `AvcapLibrary`, `Ps5UsbDirectory`, `Ps5AppInstaller`, the notification and process lookups) only pass calls on and have no host tests; what they do is checked on the console. The decisions of the capture and of the HCI transport are in `core` and are tested.
- Releases are built by `.github/workflows/release.yml` when a `v*` tag is pushed on `main` (it runs `make release` with the pinned SDK); `make clean release` does the same by hand (see the README). The source is GPL-3.0-or-later (`LICENSE`), so a release has to offer the source with it: the tag in this repository does, cloned with `--recurse-submodules` (GitHub's source archive of a tag leaves the submodules out, so the README points to the clone). The SDK's own `LICENSE` was checked (October 2026): plain GPL-3.0, no linking exception, so the GPL is required, not optional.
