# BLE - DirCon Bridge — Release 2026.1.1

**Release date:** 2026-11-23
**Previous version:** 2026.1.0
**Type:** Minor update (user interface only)

## Summary

This update reorganises the maintenance page into clearly separated sections and improves the
instructions shown after a factory reset. Bridging behaviour, sensor handling, DirCon protocol
and stored settings are unchanged.

## What's new

### Maintenance page: new layout
- The page is now divided into four visibly separate cards on a shaded background:
  1. **DirCon setup** – sensor layout and device name
  2. **Network setup** – DHCP or fixed IP address, subnet mask and gateway
  3. **Sensors** – connected sensors, pair / unpair (content unchanged)
  4. **System** – firmware update and factory reset
- **Separate save buttons:** *Save DirCon setup* and *Save network setup*. Each one saves only
  its own settings; saving one never changes the other. An invalid fixed IP address is refused
  with an explanation and does not affect the DirCon setup.
- The *Restart bridge* button is renamed **Leave maintenance mode** and sits below the cards.
  The "Done" heading is removed.
- Confirmation messages now say that changes take effect after leaving maintenance mode.

### Wi-Fi setup instructions after a factory reset
- The page shown after a factory reset now also says: *"If the sign-in page does not appear,
  open http://192.168.4.1 in your browser."*

## Upgrading

- **From a running bridge:** open the status page, enter maintenance mode, and upload
  `dircon_bridge_2026.1.1.ino.bin` in **System → Firmware update**. Wi-Fi network, paired
  sensors and all settings are kept.
- **Recovery (bridge does not start):** write `dircon_bridge_2026.1.1.ino.merged.bin` at address
  `0x0` over USB, as described in the *Device Recovery Guide*.
- Keep the same partition scheme as in earlier releases.

## Compatibility

- No change to the stored settings; downgrading to 2026.1.0 keeps all settings.
- No change to DirCon device names, services or the mDNS announcement.

## Known notes

- The user manual still describes the previous maintenance page layout and will be updated
  separately.

## Version history (recent)

| Version | Highlights |
|---|---|
| 2026.1.1 | Maintenance page in four sections, separate DirCon / network save buttons, 192.168.4.1 hint after factory reset |
| 2026.1.0 | First production release: status page, maintenance mode (settings, fixed IP, sensor pairing, firmware upload, factory reset), two-button factory reset |
