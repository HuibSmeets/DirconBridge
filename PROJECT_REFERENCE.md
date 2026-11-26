# BLE - DirCon Bridge — Project reference

Purpose of this file: a complete hand-over of the project, so that work (new features, bug fixes) can
continue in a new conversation without any memory of earlier ones. Give this file **plus the current
`.ino` file** (and, for documentation changes, the files in `doc_sources/`) to the assistant.

- **Product:** BLE - DirCon Bridge, by PulseFlow Engineering (`MANUFACTURER_NAME` in the source)
- **Current firmware:** 2026.1.1 (`dircon_bridge_2026.1.1.ino`), first production line (2026.1.0 was the first production release)
- **Status of this reference:** written at the end of the first development phase (November 2026)

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
| Status page | Always on port 80 in normal operation, own task, read-only, button to enter maintenance mode |
| Maintenance mode | Bridging stopped; settings, pairing, firmware upload, factory reset; no password |
| Fixed IP | DHCP (default) or fixed IP / mask / optional gateway |
| Factory reset | In maintenance mode (confirm dialog) or both buttons at power-on (10 s hold + LED countdown) |
| Firmware update | Upload `.ino.bin` via maintenance page (Update.h into inactive OTA slot) |
| Recovery | USB: web flasher or esptool with the **merged** image at 0x0 |

Not implemented (deliberately or not yet): web password/auth, GitHub/online update, OTA rollback, ANT+,
secure-BLE bonding (Garmin HR straps with security enabled would need it; NimBLE supports it),
DNS server setting for fixed IP, settings schema version number.

---

## 2. Hardware

- **Board:** Seeed XIAO ESP32S3 (BOOT = GPIO0 button, RESET = CHIP_PU; neither is on a header pin).
- **LEDs** (active high, direct GPIO, ≈220 Ω): defaults in source: Wi-Fi D0=GPIO1, HR D1=GPIO2,
  Cadence/Power D3=GPIO4, DirCon D4=GPIO5. *The author's own build may use other pins — keep the author's
  `LED_*_GPIO` defines when a new file is produced.* Front order left→right: Wi-Fi, HR, Cadence/Power, DirCon.
- **Buttons** (to GND, internal pull-up, software debounce 40 ms): Pairing = D9 = GPIO8 (left, back);
  Wi-Fi setup = D10 = GPIO9 (right, back). Both are read **only at power-on**.
- **Power:** USB-C 5 V. Box: LEDs front, USB-C back middle, two buttons back.
- Wi-Fi: 2.4 GHz only (ESP32-S3). Setup AP uses a fixed low channel (legal everywhere).
- Flash/partitions: Arduino "default with OTA" style scheme (two app slots + NVS). **Keep the same
  partition scheme for every release** (changing it moves NVS/OTA slots and breaks "keep settings").

---

## 3. Toolchain and build

- Arduino IDE 2.x, board *XIAO_ESP32S3*, **Arduino-ESP32 core 3.x**, **NimBLE-Arduino 2.x**.
- Sketch folder name must equal the `.ino` name (e.g. `dircon_bridge_2026.1.1`).
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

- Scheme **year.major.minor**: `2026.1.1`. Production started at **2026.1.0**; earlier 2026.0.x were beta.
- Set in three places: file name, header comment (`Version:`), `#define FIRMWARE_VERSION`. It is shown in
  every page footer.
- Bump the minor number when the author asks ("update the minor version"). If the assistant bumps it on
  its own it must say so. Older files stay in the folder as history.
- Release steps: edit → bump version → compile + test on a bridge → export binaries → rename merged file
  (e.g. `DirCon_Bridge_<ver>_recovery.bin`) → update release notes → regenerate the manual if the UI or
  behaviour changed → publish.
- Documents produced so far: **User manual** (PDF, A4, 12 pages, for the current firmware), **Device Recovery
  Guide** (PDF, A4, 4 pages, version-independent), **Release notes 2026.1.1** (MD).

---

## 5. Source layout (`dircon_bridge_<ver>.ino`, ≈2000 lines, single file)

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
6. LEDs (`ledsBegin`, `ledsUpdate`), pairing (`loadFilters`, `runPairing`, `buttonHeldAtBoot`).
7. Wi-Fi setup mode (`runWifiSetup`, `PAGE_HEAD` CSS raw string, `pageHeader(sub)`, `pageFooter`, handlers).
8. Settings (`loadSettings/saveSettings`, `checkFixedIp`, maintenance flag, `eraseAllSettings`), `connectWifi`,
   `beginMdnsHost`, status page + `statusTask`, maintenance page (`sensorsSection`, `maintPage`, handlers,
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
| `settings` | `dhcp` | bool | true (default) = DHCP; saving a Wi-Fi network in setup mode resets it to true |
| `settings` | `ip`, `mask`, `gw` | string | fixed address; invalid/missing → DHCP; gateway `0.0.0.0` = none |
| `settings` | `maint` | bool | transient: enter maintenance on next boot (cleared at boot) |

Factory reset erases all three namespaces.

---

## 7. Web pages and routes

| Mode | Routes |
|---|---|
| Wi-Fi setup (AP, DNS wildcard) | `GET /`, `GET /scan`, `POST /save`, anything else → redirect to `/` |
| Normal operation (status) | `GET /` (auto-refresh 5 s), `POST /maint`, anything else → redirect |
| Maintenance | `GET /`, `POST /dircon` (group, name), `POST /network` (net, ip, mask, gw), `POST /pair` (slot, act=pair/unpair), `POST /update` (multipart `firmware`), `POST /factory`, `POST /restart` |

- Maintenance page = four cards (DirCon setup, Network setup, Sensors, System) + green **Leave maintenance mode**.
- DirCon and Network cards save **independently**; both call `saveSettings(cfgGroup, cfgName)` which writes all
  `cfg*` globals, so each handler must update only its own globals first.
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
  `36866 + group`. `MAX_CLIENTS` 2 per device.
- `buildInstance()`: combined (more than one slot in the group) → `DEVICE_NAME_PREFIX " Sensors"`; otherwise
  `DirCon <sensor name>` (connected row of the slot, else first row), then ` <custom name>` or ` <MAC4>`.
- Pairing mode: RSSI ≥ `PAIR_MIN_RSSI` (−80); trainers excluded; LED blinks while searching, solid when found;
  ends automatically 1.5 s after both found or by a second button press; start clears stored pairings first.
- Slot logic: if any row of a slot has a stored address, the other rows of that slot are `disabled`.
- Battery: polled every `BATTERY_POLL_MS`, served from cache, notified on change; hidden in combined layout.
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
- The two-button check runs after the LED self-test and after reading Wi-Fi credentials/maintenance flag,
  not as the very first statement of `setup()`.
- Possible additions: password for the web pages, settings schema version, DNS server for fixed IP, secure-BLE
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
  cross-references). The manual currently describes firmware **2026.1.1**.
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
- Typical request pattern for the future: attach the current `.ino` and this file, describe the change, and say
  whether the manual, recovery guide, release notes and version number should be updated too.
