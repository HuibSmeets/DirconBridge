# BLE - DirCon Bridge — Project reference

Purpose of this file: a complete hand-over of the project, so that work (new features, bug fixes) can
continue in a new conversation without any memory of earlier ones. Give this file **plus the current
`.ino` file** (and, for documentation changes, the files in `doc_sources/`) to the assistant.

- **Product:** BLE - DirCon Bridge, by PulseFlow Engineering (`MANUFACTURER_NAME` in the source)
- **Current firmware:** 2026.2.1 (`dircon_bridge_2026.2.1.ino`), second production line. 2026.1.0 was the first production release and 2026.1.1 the last release of the first line. **2026.2.1 has not been compiled or tested on hardware yet and is not released**; it was built from 2026.1.1 (via the development version 2026.2.dev).
- **Status of this reference:** written at the end of the first development phase (November 2026); updated 26 November 2026 for firmware 2026.2.1 (see "Changes in 2026.2.1" below)

---

## Changes in 2026.2.1 (compared with 2026.1.1)

1. **Status page, battery level:** after the sensor name, `(battery icon NN%)`; green above 50 %, orange 25..50 %, red below 25 %. Not shown while the level is unknown (not read yet in this connection).
2. **Status page, app connections per sensor:** the single "App connections" row is gone. Under each sensor line (Heart rate, Power / cadence) an indented "App connections" line shows `actual/maximum` for the DirCon device serving that slot (combined layout: both lines show the same device).
3. **Maintenance page, DirCon setup card:** new select "Maximum concurrent connections per sensor" (2..5, default 2) below the device name, with a warning that more than 2 can cause issues. The limit is enforced **per DirCon device**; a new connection at the limit evicts the oldest one of that device.
4. **Low-battery LED:** a connected sensor with a battery level below 25 % has its LED (HR or Cadence/Power) turned off for 0.2 s every 3 s. Always active, no setting.
5. **Fixed IP: DNS server:** optional "DNS server" field in the Network setup card (stored as `dns`, passed to `WiFi.config()` when set; empty = as before).
6. **Version:** `2026.2.1` in file name, header comment and `FIRMWARE_VERSION`. The user manual (regenerated from `doc_sources/`) and the release notes describe these changes; the recovery guide is version-independent and unchanged.

---

## 1. What the project does

An ESP32-S3 box that reads **BLE heart rate** and **BLE cadence sensor / power meter** and offers them
to training apps (Rouvy, Zwift, …) over **Wi-Fi using Wahoo Direct Connect (DirCon)**. DirCon devices are
announced with mDNS only while at least one of their sensors is connected.

Key functions:

| Function | Short description |
|---|---|
| Sensor bridging | HR (slot 0) and "cadence slot" (slot 1: **either** cadence sensor **or** power meter) |
| Smart trainer filter | Devices advertising FTMS (0x1826) are never used or paired; optional name-prefix list |
| Pairing | Via button at power-on (both sensors) or one at a time in maintenance mode; MAC stored in NVS |
| Layout | Separate DirCon devices (default, battery visible) or one combined device (no battery) |
| Naming | `DirCon HR xxxx`, `DirCon CAD xxxx`, `DirCon PWR xxxx`, combined: `DirCon Sensors xxxx`; `xxxx` = MAC digits or user device name |
| Model # | Manufacturer read from the sensor + BLE name, e.g. "<brand> <name>" (`devName`) |
| Wi-Fi setup | AP `DirCon-Setup-<MAC4>` + captive portal, 192.168.4.1; credentials in NVS |
| Status page | Always on port 80 in normal operation, own task, read-only, button to enter maintenance mode; shows the sensor battery level (colour coded) and the app connections per sensor as `actual/maximum` |
| Connection limit | Maximum simultaneous app connections per DirCon device, 2..5 (default 2), set in maintenance mode |
| Low-battery signal | Sensor LED goes off briefly every 3 s while that sensor's battery is below 25 % |
| Maintenance mode | Bridging stopped; settings, pairing, firmware upload, factory reset; no password |
| Fixed IP | DHCP (default) or fixed IP / mask / optional gateway / optional DNS server |
| Factory reset | In maintenance mode (confirm dialog) or both buttons at power-on (10 s hold + LED countdown) |
| Firmware update | Upload `.ino.bin` via maintenance page (Update.h into inactive OTA slot) |
| Recovery | USB: web flasher or esptool with the **merged** image at 0x0 |

Not implemented (deliberately or not yet): web password/auth, GitHub/online update, OTA rollback, ANT+,
secure-BLE bonding (Garmin HR straps with security enabled would need it; NimBLE supports it),
settings schema version number.

---

## 2. Hardware

- **Board:** Seeed XIAO ESP32S3 (BOOT = GPIO0 button, RESET = CHIP_PU; neither is on a header pin).
- **LEDs** (active high, direct GPIO, ≈220 Ω): defaults in source: Wi-Fi D0=GPIO1, HR D1=GPIO2,
  Cadence/Power D3=GPIO4, DirCon D4=GPIO5. *The author's own build may use other pins — keep the author's
  `LED_*_GPIO` defines when a new file is produced.* Front order left→right: Wi-Fi, HR, Cadence/Power, DirCon.
  The HR and Cadence/Power LEDs are plain on/off outputs (no PWM). From 2026.2.1 they go off for 0.2 s every 3 s
  while the connected sensor's battery is below 25 %.
- **Buttons** (to GND, internal pull-up, software debounce 40 ms): Pairing = D9 = GPIO8 (left, back);
  Wi-Fi setup = D10 = GPIO9 (right, back). Both are read **only at power-on**.
- **Power:** USB-C 5 V. Box: LEDs front, USB-C back middle, two buttons back.
- Wi-Fi: 2.4 GHz only (ESP32-S3). Setup AP uses a fixed low channel (legal everywhere).
- Flash/partitions: Arduino "default with OTA" style scheme (two app slots + NVS). **Keep the same
  partition scheme for every release** (changing it moves NVS/OTA slots and breaks "keep settings").

---

## 3. Toolchain and build

- Arduino IDE 2.x, board *XIAO_ESP32S3*, **Arduino-ESP32 core 3.x**, **NimBLE-Arduino 2.x**.
- Sketch folder name must equal the `.ino` name (e.g. `dircon_bridge_2026.2.1`).
- Libraries used from the core: WiFi, WebServer, DNSServer, ESPmDNS, ESP-IDF `mdns.h`, Preferences, Update.
- `DEBUG_SERIAL 0` for normal use (an unread USB serial port can stall); set 1 only for debugging
  (then `[BOOT]`, `[WiFi]`, `[MAINT]`, `[RESET]`, `[OTA]`, `[BLE]`, `[PAIR]`, `[STAT]` logs appear).
- **The assistant has no Arduino compiler/hardware**: every change so far was reviewed by reading and a
  bracket check only. Compile errors and behaviour must be verified by the author on the device. Ask the
  author for the compiler output when something fails.
- Binaries (Sketch → Export Compiled Binary), from `build/esp32.esp32.XIAO_ESP32S3/`:
  - `<name>.ino.bin` → **regular update** (maintenance page upload, app only)
  - `<name>.ino.merged.bin` → **recovery** (flash at `0x0` over USB)
  - the other files (`bootloader`, `partitions`, `boot_app0`, `_flashed`, `.elf`, `.map`) are not shipped.

---

## 4. Versioning and release process

- Scheme **year.major.minor**: `2026.2.1`. Production started at **2026.1.0**; earlier 2026.0.x were beta.
  `2026.2.dev` was used for the development builds that became 2026.2.1.
- Set in three places: file name, header comment (`Version:`), `#define FIRMWARE_VERSION`. It is shown in
  every page footer.
- Bump the minor number when the author asks ("update the minor version"). If the assistant bumps it on
  its own it must say so. Older files stay in the folder as history.
- Release steps: edit → bump version → compile + test on a bridge → export binaries → rename merged file
  (e.g. `DirCon_Bridge_<ver>_recovery.bin`) → update release notes → regenerate the manual if the UI or
  behaviour changed → publish.
- Documents produced so far: **User manual** (PDF, A4, 12 pages, for the current firmware), **Device Recovery
  Guide** (PDF, A4, 4 pages, version-independent), **Release notes 2026.1.1** (MD). The user manual (PDF, 12 pages) and the
  release notes (new 2026.2.1 section on top of the 2026.1.1 notes) have been updated for 2026.2.1.

---

## 5. Source layout (`dircon_bridge_<ver>.ino`, ≈2100 lines, single file)

Order in the file:

1. Header comment (behaviour description), includes
2. **USER CONFIG** (`MDNS_HOSTNAME`, `DEVICE_NAME_PREFIX`, `USE_MAC_SUFFIX`, ports, battery timing, `DEBUG_SERIAL`,
   sensor name/address filters, `FILTER_OUT_TRAINERS`, `TRAINER_NAME_PREFIXES`, `MANUFACTURER_NAME`, `FIRMWARE_VERSION`,
   LED pins, button pins, pairing constants, `SENSOR_GROUP` default layout)
3. **Sensor table** `SensorDef SENSORS[]` (non-const): rows `HR` (slot 0, group 0), `PWR` (slot 1, group = layout),
   `CAD` (slot 1). Each lists GATT services/characteristics (`PROP_READ/NOTIFY/LOCAL/POLLED`). Row order matters:
   power meter before cadence sensor (a device offering both is taken as power meter).
4. BLE side: `SensorRT rt[]` (runtime: `connected`, `hasFound`, `filterName/Addr`, `devName`, `cache`), `devNameC[]`
   (plain char copy for the web task), `ScanCallbacks`, `connectSensor`, `pollSensor`, `bleTask`, `bleBegin`.
5. DirCon side: `DirconDevice` class (TCP server per group, framing, subscriptions, pending reads, mDNS
   advertise via `mdns_service_add("_wahoo-fitness-tnp")`, `buildInstance()` naming), `mdnsRefresh()`, `buildGroups()`.
   The client table is `clients[MAX_CLIENTS_LIMIT]` (5); the number actually accepted is `cfgMaxClients` (setting, default
   `MAX_CLIENTS` = 2), applied where `poll()` assigns a free slot or evicts the oldest.
6. `batteryPercent(i)` (cached battery level of a connected sensor, -1 = unknown; used by the status page and the LEDs),
   LEDs (`ledsBegin`, `slotBatteryLow`, `ledsUpdate`), pairing (`loadFilters`, `runPairing`, `buttonHeldAtBoot`).
7. Wi-Fi setup mode (`runWifiSetup`, `PAGE_HEAD` CSS raw string, `pageHeader(sub)`, `pageFooter`, handlers).
8. Settings (`loadSettings/saveSettings`, `checkFixedIp`, maintenance flag, `eraseAllSettings`), `connectWifi`,
   `beginMdnsHost`, status page (`slotStatus`, `slotConns`, `statusPage`) + `statusTask`, maintenance page (`sensorsSection`, `maintPage`, handlers,
   firmware upload, factory, restart, `runMaintenance`).
9. Factory reset gesture (`factoryResetGesture`), `setup()`, `loop()`.

### Boot flow (`setup()`)

1. Serial (if `DEBUG_SERIAL`), `ledsBegin()` (all LEDs on 400 ms self-test).
2. Read buttons: `pairBtn`, `apBtn`; `loadWifiCreds()`; `takeMaintFlag()` (clears the one-time flag).
3. Both buttons → `factoryResetGesture()`; completed → `eraseAllSettings()` + restart; aborted → treat as no button.
4. Pairing button only → `runPairing()` (then continues normally).
5. Wi-Fi button only, **or no stored SSID** → `runWifiSetup()` (never returns; saving restarts).
6. `loadSettings()` (applies layout to slot-1 rows), `loadFilters()` (paired MACs), queue, `buildGroups()`, `seedDefaults()`.
7. Maintenance flag set → `runMaintenance()` (never returns).
8. `connectWifi(0)`, `beginMdnsHost()`, `startStatusServer()`, DirCon servers `begin()`, `bleBegin()`.
9. `loop()`: forwards queued BLE notifications to DirCon clients, `poll()`, `mdnsRefresh()`, `ledsUpdate()`.

### Tasks / threads

- Arduino `loop()` task: DirCon forwarding, mDNS refresh, LEDs.
- `bleTask` (8 KB stack): scanning, connecting, battery polling. Scan callback sets `hasFound`; connect happens here.
- `statusTask` (6 KB stack): web server of the status page only (stalled web clients cannot delay `loop()`).
- Shared data: `rt[]` atomics, `cacheMux` critical sections for the characteristic cache; `devNameC[]` for text.
  Minor benign races exist where the web task reads state written by the BLE task.

### Maintenance mode specifics

- Entered by `POST /maint` → sets NVS flag `maint` → restart → flag consumed at boot → `runMaintenance()`.
- Runs Wi-Fi + mDNS host + **BLE connect-only** (so the Sensors card can show/pair sensors; nothing is forwarded,
  the queue is never drained) + web server. Only the Wi-Fi LED is on.
- Ends by `POST /restart`, power cycle, or `MAINT_IDLE_MS` = 15 min without requests, or Wi-Fi not reachable
  within 60 s at its start (then restarts into normal mode).
- Pair/unpair write NVS immediately but take effect after leaving maintenance (filters are loaded at boot).

---

## 6. Stored settings (NVS) — compatibility rule

**Never change the meaning or type of an existing key; only add new keys with safe defaults.** Unknown keys
are ignored by older firmware, so downgrades keep working.

| Namespace | Key | Type | Meaning / default |
|---|---|---|---|
| `dircon` | `a_HR`, `a_PWR`, `a_CAD` | string | paired MAC (lower-case `aa:bb:..`); cleared when pairing starts, by Unpair, by factory reset |
| `wifi` | `ssid`, `pass` | string | home network; no SSID → Wi-Fi setup starts |
| `settings` | `group` | uchar | 1 = separate devices (default), 0 = combined |
| `settings` | `name` | string | device name ≤ 20 chars `[A-Za-z0-9 -_]`; empty = MAC suffix |
| `settings` | `maxcl` | uchar | maximum app connections per DirCon device, 2..5; missing/out of range → 2 (added in 2026.2.1) |
| `settings` | `dhcp` | bool | true (default) = DHCP; saving a Wi-Fi network in setup mode resets it to true |
| `settings` | `ip`, `mask`, `gw` | string | fixed address; invalid/missing → DHCP; gateway `0.0.0.0` = none |
| `settings` | `dns` | string | optional DNS server for the fixed address; invalid/missing/`0.0.0.0` = none (added in 2026.2.1) |
| `settings` | `maint` | bool | transient: enter maintenance on next boot (cleared at boot) |

Factory reset erases all three namespaces.

---

## 7. Web pages and routes

| Mode | Routes |
|---|---|
| Wi-Fi setup (AP, DNS wildcard) | `GET /`, `GET /scan`, `POST /save`, anything else → redirect to `/` |
| Normal operation (status) | `GET /` (auto-refresh 5 s), `POST /maint`, anything else → redirect |
| Maintenance | `GET /`, `POST /dircon` (group, name, maxcl), `POST /network` (net, ip, mask, gw, dns), `POST /pair` (slot, act=pair/unpair), `POST /update` (multipart `firmware`), `POST /factory`, `POST /restart` |

- Maintenance page = four cards (DirCon setup, Network setup, Sensors, System) + green **Leave maintenance mode**.
- DirCon and Network cards save **independently**; both call `saveSettings(cfgGroup, cfgName)` which writes all
  `cfg*` globals (`cfgMaxClients` and `cfgDns` included), so each handler must update only its own globals first.
- DirCon setup card: layout, device name, **Maximum concurrent connections per sensor** (select 2..5, with a warning
  that more than 2 can cause issues). Network setup card: IP mode, IP, mask, gateway, **DNS server (optional)**.
- Status page rows: Wi-Fi, IP address, Heart rate, `App connections` (indented, `actual/maximum` of the device serving
  the HR slot), Power / cadence, `App connections` (same for slot 1), Layout, Uptime. After a sensor name the battery
  is shown as `(icon NN%)` in green (> 50), orange (25..50) or red (< 25); CSS classes `.bg .bm .br`; the icon is an
  inline SVG that takes the text colour.
- Firmware upload: `Update.begin(UPDATE_SIZE_UNKNOWN)`, write chunks, `Update.end(true)`; failure shows an
  error and keeps the running firmware; success restarts. File picker is a styled label; upload button is
  disabled/grey until a file is chosen, then blue (small inline JS).
- Factory-reset page tells the user to join `DirCon-Setup-xxxx` and, if the sign-in page does not appear,
  to open `http://192.168.4.1`.
- Styling: one CSS raw string `PAGE_HEAD`; classes `.btn .sec .pri .go .dng .note .err .hint .card .sub`.
- Host name: `dircon-bridge-<sanitised device name>` (lower-case, non-alphanumerics → `-`) else
  `dircon-bridge-<mac4>`; **no `_http._tcp` advertisement** (decided against).

---

## 8. Behaviour details worth remembering

- A DirCon device is advertised/accepts apps only when `readyMask()` ≠ 0; changes of the connected set
  make the device re-announce and (combined layout) drop app connections once.
- Group = DirCon device: group 0 = HR (+ slot 1 when combined), group 1 = slot 1 when separate. TCP ports
  `36866 + group`. Max. app connections per device = `cfgMaxClients` (default 2). At the limit a new connection evicts the
  oldest of that device. The limit is per device, not a total over all devices (so the status page can show more connections
  in total than the maximum).
- `buildInstance()`: combined (more than one slot in the group) → `DEVICE_NAME_PREFIX " Sensors"`; otherwise
  `DirCon <sensor name>` (connected row of the slot, else first row), then ` <custom name>` or ` <MAC4>`.
- Pairing mode: RSSI ≥ `PAIR_MIN_RSSI` (−80); trainers excluded; LED blinks while searching, solid when found;
  ends automatically 1.5 s after both found or by a second button press; start clears stored pairings first.
- Slot logic: if any row of a slot has a stored address, the other rows of that slot are `disabled`.
- Battery: polled every `BATTERY_POLL_MS`, served from cache, notified on change; hidden from the apps in combined layout
  (the status page and the low-battery LED still use the cached value).
- Low-battery LED: `ledsUpdate()` switches the HR / Cadence-Power LED off for `BATT_DIP_LEN_MS` (200 ms) every
  `BATT_DIP_PERIOD_MS` (3000 ms) while `batteryPercent()` is 0..24 for the connected sensor of that slot. Unknown level = no signal.
  The battery is polled every 5 min, so the signal can lag behind the real level.
- The bridge cannot tell apps apart: DirCon has no app identity, only the TCP client's IP address is visible (not shown anywhere).
- Factory reset gesture: after the LED self-test, both buttons held: 10 s (LEDs off) → all on → one LED off
  every 3 s right→left (DirCon, Cad/Pwr, HR, Wi-Fi) → 3 flashes → erase + restart. Release of either button
  (40 ms debounce) aborts and boot continues as if no button was pressed.
- Security: web pages have **no authentication**; documented in the manual's small print.

---

## 9. Known limitations / ideas for later

- No compile/hardware verification by the assistant (see §3). Record here which version was last tested
  on hardware: **_(fill in)_**.
- Wi-Fi network scan in setup mode briefly disturbs the AP (single radio); usually harmless.
- Router on channel 12–14 might not be found if the stack's channel set excludes it (not observed).
- OTA has no automatic rollback; a faulty-but-booting firmware must be replaced by upload or USB recovery.
  A boot-loop guard / settings reset after repeated failed starts was suggested but not built.
- Cosmetic, seen once: after a sensor left BLE range and came back, the status page / Model # showed only the manufacturer
  without the sensor name until restart. Likely cause (from reading, not reproduced): `connectSensor()` takes the name from the
  first scan result; if that has no name, `devName` falls back to the manufacturer and is not updated while connected.
  Heart rate processing was not affected. Not fixed.
- The two-button check runs after the LED self-test and after reading Wi-Fi credentials/maintenance flag,
  not as the very first statement of `setup()`.
- Possible additions: password for the web pages, settings schema version, secure-BLE
  bonding for HR straps that require it, `_http._tcp` mDNS advertisement, ANT+ (needs different hardware).

---

## 10. Documentation sources (`doc_sources/`)

- `make_manual2.py` — builds `DirCon_Bridge_User_Manual.pdf` with reportlab (cover, contents, 12 sections,
  small print). It `exec`s the style/helper part of `make_manual.py` (first part, up to the "page decoration"
  marker) and reads images from a scratch path — adjust the `SCR` path at the top when running elsewhere.
- `gen_shots.py` — renders the status and maintenance pages with Playwright (Chromium) from the **real
  `PAGE_HEAD` CSS** in the `.ino` plus hand-ported HTML with sample data; writes `shots/*.png`. Re-run and
  compare with the firmware's `statusPage()` / `maintPage()` whenever the UI changes.
- `make_recovery.py` — builds `DirCon_Bridge_Recovery_Guide.pdf` (web flasher first, then esptool for
  Windows/macOS/Linux).
- `cp1.png`, `cp2.png` — captive-portal screenshots; `rouvy_crop.png` — Rouvy device details screenshot.
- Manual text must stay consistent with firmware (LED meanings, button table, page cards, section
  cross-references). The manual currently describes firmware **2026.2.1** (`VERSION` in `make_manual2.py`). After a UI change: run
  `python3 gen_shots.py <firmware>.ino` (writes `setup_1/2`, `status_full`, `maint_a/b/c` = three parts of the maintenance page),
  then `make_manual2.py`. Image heights come from the PNG files (`img_h`). `gen_shots.py` is a hand-ported copy of the pages' HTML:
  compare it with `statusPage()` / `maintPage()` when those change. Page 5 now uses the generated Wi-Fi setup shots (not phone captures).
- GPL v3 is stated in the manual's small print; the repository needs a matching `LICENSE` file.

---

## 11. Working agreements with the author

- Reply concisely; answer questions directly. "Don't implement yet" / "only answer" means **no code changes**.
  When the author collects several UI changes, wait for the explicit "implement" and do them together.
- Do not bump the version unasked (or say so when doing it). Keep the file name equal to the version.
- Do not mention sensor brands in source or comments (use "HR sensor", "power meter", …). Protocol names
  (Wahoo DirCon) and app names are fine.
- Keep the author's LED pin defines; keep credentials out of the source.
- Deliver files with the file-send tool; manuals as PDF (A4, blue header band on each page, blue footer band),
  release notes / this reference as Markdown.
- Be explicit about what is untested. Disclose any unrequested change.
- Ideas that were discussed but not decided are not written into this file unless the author asks.
- Typical request pattern for the future: attach the current `.ino` and this file, describe the change, and say
  whether the manual, recovery guide, release notes and version number should be updated too.
