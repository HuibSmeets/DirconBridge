# BLE - DirCon Bridge — Release 2026.2.1

**Release date:** 2026-11-26
**Previous version:** 2026.1.1
**Type:** Minor update (status page, maintenance page, LED signalling, fixed IP)
**Status:** not yet tested on a bridge at the time of writing

## Summary

This update adds more information to the status page (sensor battery level and app connections per
sensor), a setting for the number of simultaneous app connections, a low-battery signal on the sensor
LEDs, and an optional DNS server for a fixed IP address. Sensor handling and the DirCon protocol are
unchanged.

## What's new

### Status page
- **Battery level:** the battery level of each connected sensor is shown after the sensor name as a
  battery icon and a percentage, for example *(icon 72%)*. It is green above 50 %, orange from 25 to 50 %
  and red below 25 %. Nothing is shown until the sensor's battery level has been read.
- **App connections per sensor:** the single *App connections* line is replaced by one line below each
  sensor, shown as *actual/maximum* (for example `2/5`). It counts the apps connected to the DirCon
  device of that sensor. With the combined layout both lines show the same device.

### Maintenance page
- **DirCon setup:** new setting **Maximum concurrent connections per sensor** (2 to 5, default 2),
  below the device name. A note warns that more than 2 can cause issues. The limit applies to each DirCon
  device separately; when it is reached, a new connection replaces the oldest one of that device.
- **Network setup:** new optional field **DNS server** for a fixed IP address. Empty means no DNS server is
  set, as before. The field is ignored when *Automatic (DHCP)* is selected. An invalid address is refused
  with an explanation.

### Low-battery signal
- While the battery of a connected sensor is below 25 %, the LED of that sensor (HR or Cadence/Power)
  goes off briefly every 3 seconds. This is always active and has no setting. The battery level is read
  every 5 minutes, so the signal can lag behind the real level.

## Upgrading

- **From a running bridge:** open the status page, enter maintenance mode, and upload
  `dircon_bridge_2026.2.1.ino.bin` in **System → Firmware update**. Wi-Fi network, paired sensors and
  all settings are kept.
- **Recovery (bridge does not start):** write `dircon_bridge_2026.2.1.ino.merged.bin` at address `0x0`
  over USB, as described in the *Device Recovery Guide*.
- Keep the same partition scheme as in earlier releases.

## Compatibility

- Two new stored settings (maximum connections and DNS server). Without them the bridge behaves as
  before (2 connections per device, no DNS server). Downgrading to 2026.1.1 keeps all other settings;
  the older firmware ignores the two new ones.
- No change to DirCon device names, services or the mDNS announcement.

## Version history (recent)

| Version | Highlights |
|---|---|
| 2026.2.1 | Battery level and per-sensor app connections on the status page, maximum connections setting, low-battery LED signal, DNS server for fixed IP |
| 2026.1.1 | Maintenance page in four sections, separate DirCon / network save buttons, 192.168.4.1 hint after factory reset |
| 2026.1.0 | First production release: status page, maintenance mode (settings, fixed IP, sensor pairing, firmware upload, factory reset), two-button factory reset |

---

# Earlier release notes

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

## Version history (recent)

| Version | Highlights |
|---|---|
| 2026.1.1 | Maintenance page in four sections, separate DirCon / network save buttons, 192.168.4.1 hint after factory reset |
| 2026.1.0 | First production release: status page, maintenance mode (settings, fixed IP, sensor pairing, firmware upload, factory reset), two-button factory reset |
