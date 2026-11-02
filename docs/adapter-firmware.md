# Adapter firmware

Uploading the firmware some USB adapters need.

## 5a. Adapter firmware (`core/bluetooth/firmware.*`, `realtek_firmware.*`)

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
