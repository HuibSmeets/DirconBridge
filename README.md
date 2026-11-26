# BLE - DirCon Bridge

**Bridge your Bluetooth heart rate, cadence and power sensors to Rouvy, Zwift and other apps over Wi-Fi.**

An ESP32-S3 firmware that reads BLE sensors and offers them to training apps as **Wahoo Direct Connect
(DirCon)** devices on your local network. No cloud, no account, no computer needed. A tiny box with
four LEDs, two buttons and a USB-C power connection.

> **DIY project, no warranty.** See [Disclaimer](#disclaimer) and [License](#license).

![status](https://img.shields.io/badge/firmware-2026.1.1-blue) ![license](https://img.shields.io/badge/license-GPLv3-green) ![platform](https://img.shields.io/badge/ESP32--S3-XIAO-lightgrey)

---

## Contents

- [Features](#features)
- [How it works](#how-it-works)
- [Hardware](#hardware)
- [Quick start](#quick-start)
- [LEDs and buttons](#leds-and-buttons)
- [Status page and maintenance mode](#status-page-and-maintenance-mode)
- [Pairing sensors](#pairing-sensors)
- [Updating the firmware](#updating-the-firmware)
- [Recovery](#recovery)
- [Building from source](#building-from-source)
- [Documentation](#documentation)
- [Versioning](#versioning)
- [Disclaimer](#disclaimer)
- [License](#license)

---

## Features

- **Heart rate** sensor and **cadence sensor or power meter** (one or the other) bridged to DirCon.
- **Separate devices** (`DirCon HR`, `DirCon CAD` / `DirCon PWR`, each with battery level) or **one combined
  device** (`DirCon Sensors`) for apps that want everything in one.
- **Smart trainers are ignored** (FTMS filter), so the bridge never grabs a trainer by mistake. Connect the
  trainer to the app directly, as usual.
- **Optional pairing** locks the bridge to your own sensors; without pairing it uses the first suitable ones.
- A DirCon device is **announced only while its sensor is connected**.
- **Wi-Fi setup** from your phone (access point + captive portal), no credentials in the source code.
- **Status page** (always on, read-only, never disturbs the bridging) in any browser.
- **Maintenance mode**: sensor layout, device name, fixed IP address, pair/unpair sensors, firmware upload from a
  computer, factory reset.
- Several bridges on one network are told apart by a 4-character code or your own device name.
- Manufacturer and model of the real sensor are passed on (*Manufacturer* / *Model #* in the app).

## How it works

```
 [HR sensor]  --BLE-->  +-------------------+  --Wi-Fi (DirCon)-->  [Rouvy / app]
 [Cadence or            |  BLE - DirCon     |
  power meter] --BLE--> |  Bridge (ESP32-S3)|
                        +-------------------+
 [Smart trainer] ---------------- connected to the app directly, as before ----------->
```

The bridge scans for BLE sensors, connects, and forwards notifications to the app over TCP using the DirCon
protocol. It announces itself with mDNS (`_wahoo-fitness-tnp._tcp`), so the app finds it like any other
DirCon device. Each connected sensor appears as its own device (or all in one, in the combined layout).

## Hardware

| Part | Notes |
|---|---|
| **Seeed XIAO ESP32S3** | The target board (any ESP32-S3 should work) |
| 4 LEDs + resistors (~220 Ω) | Wi-Fi (D0), HR (D1), Cadence/Power (D3), DirCon (D4); pins are `#define`s |
| 2 push buttons to GND | Pairing = D9 (GPIO8), Wi-Fi setup = D10 (GPIO9); internal pull-ups used |
| USB-C power | Any 5 V USB charger |

Wi-Fi is **2.4 GHz only**. Schematic and enclosure files: *add your own links here*.

## Quick start

1. Flash the firmware (see [Updating the firmware](#updating-the-firmware) or
   [Building from source](#building-from-source)).
2. Connect power. The four LEDs flash once and the **Wi-Fi LED blinks**: the bridge waits for the Wi-Fi setup.
3. On your phone join the network **`DirCon-Setup-xxxx`** (no password). The setup page opens by itself; if it
   does not, open **http://192.168.4.1**. Choose your 2.4 GHz network, enter the password, *Save and start*.
4. The **Wi-Fi LED stays on** when connected. Wake your sensors; the **HR** and **Cadence / Power** LEDs turn on.
5. In your app search for sensors and pick `DirCon HR xxxx` and `DirCon CAD xxxx` / `DirCon PWR xxxx`.
   The **DirCon LED** turns on when the app is connected.

## LEDs and buttons

| LED | On | Blinking |
|---|---|---|
| **Wi-Fi** | Connected to your network | Slowly: Wi-Fi setup active |
| **HR** | Heart rate sensor connected | Pairing: searching |
| **Cadence / Power** | Cadence sensor or power meter connected | Pairing: searching |
| **DirCon** | An app is connected | – |

Buttons are read **at power-on** (hold while connecting power, until the LEDs have flashed):

| Held at power-on | Result |
|---|---|
| none | Normal start |
| Pairing button | Pairing mode |
| Wi-Fi setup button | Wi-Fi setup |
| **Both** | Factory reset: hold 10 s, then the LEDs go off one by one (right to left, 3 s each); release any button to cancel |

## Status page and maintenance mode

Open `http://dircon-bridge-<name>.local` (device name, or the 4-character code if none is set), or use the
bridge's IP address.

- **Status page** (normal operation): Wi-Fi network and signal, IP address (DHCP/fixed), connected sensors,
  layout, number of app connections, uptime. A button enters **maintenance mode**.
- **Maintenance mode** stops the bridging and offers four cards:
  1. **DirCon setup** – sensor layout (separate / combined) and device name
  2. **Network setup** – DHCP or fixed IP address, subnet mask, optional gateway
  3. **Sensors** – connected sensors, **pair / unpair**
  4. **System** – firmware update, factory reset

  *Leave maintenance mode* restarts into normal operation. It also ends by itself after 15 minutes without use.

> **No password.** Anyone on your Wi-Fi network can open these pages. Use the bridge on a network you trust.

## Pairing sensors

Pairing is optional. Paired sensors are stored in flash and the bridge then only connects to them.

- **With the button:** hold the pairing button while connecting power, keep the sensors close and awake; the
  LEDs show what was found; pairing ends automatically when both are found (or press the button again).
  Starting pairing first erases earlier pairings.
- **In maintenance mode:** the Sensors card shows what is connected; *Pair this sensor* stores it, *Unpair*
  removes it. Changes apply after leaving maintenance mode. To switch to a different sensor: unpair, leave,
  re-enter, pair the new one.

## Updating the firmware

From a running bridge, no tools needed:

1. Download the release file **`…ino.bin`** (application image).
2. Open the status page → **Enter maintenance mode** → **System** → *Choose file* → *Upload and install*.
3. Keep the power connected. The bridge restarts by itself; your settings are kept.

A failed or interrupted upload leaves the running firmware untouched.

## Recovery

If the bridge does not start at all, write the complete image over USB-C:

- Use the release file **`…ino.merged.bin`**, address **`0x0`**.
- Easiest: Espressif's web flasher in Chrome or Edge (<https://espressif.github.io/esptool-js/>), or `esptool`:

  ```
  esptool --chip esp32s3 --port <PORT> write-flash 0x0 <file>.merged.bin
  ```

  Add `--erase-all` to also erase the stored settings. If the board is not recognised, hold the XIAO's
  **BOOT** button while connecting USB.
- Step-by-step instructions for Windows, macOS and Linux are in the **Device Recovery Guide**.

## Building from source

Requirements:

- Arduino IDE 2.x
- Board package **esp32 (Arduino-ESP32) 3.x**, board *XIAO_ESP32S3*
- Library **NimBLE-Arduino 2.x**
- A partition scheme with two app slots (OTA); keep the same one for all releases

Steps:

1. Put `dircon_bridge_<version>.ino` in a folder with the same name and open it.
2. Check the `USER CONFIG` block at the top (LED and button pins, `MANUFACTURER_NAME`, optional sensor
   name/address filters, `DEBUG_SERIAL`).
3. Select the board and port, upload. For release binaries use *Sketch → Export Compiled Binary*:
   `.ino.bin` is the update file, `.ino.merged.bin` the recovery file.

No Wi-Fi credentials are stored in the source; they are entered on the setup page and kept in flash.

## Documentation

- **User manual** (PDF) – setup, LEDs and buttons, status and maintenance pages, pairing, troubleshooting
- **Device recovery guide** (PDF) – USB recovery with the web flasher or `esptool` (Windows, macOS, Linux)
- **Release notes** – changes per version
- **Project reference** – architecture, settings, build and release process (for maintainers)

## Versioning

`year.major.minor`, for example `2026.1.1`. The version is part of the file name and shown in the footer of
every web page. `2026.1.0` was the first production release.

## Disclaimer

This is a **do-it-yourself project**, provided **"as is", without warranty of any kind, express or implied**,
including merchantability and fitness for a particular purpose. You build, use and update it at your own
risk; the authors are not liable for any damage or loss. It is **not a medical device** and has **not been
certified** (CE, UKCA, FCC or similar); you are responsible for complying with the rules that apply to you.

Wahoo, Rouvy, Zwift, Bluetooth, Seeed Studio, XIAO, Espressif and other names are trademarks of their
respective owners. This project is not affiliated with, endorsed by or sponsored by them.

## License

The firmware is free software, released under the **GNU General Public License, version 3** (GPLv3).
See the `LICENSE` file and <https://www.gnu.org/licenses/gpl-3.0.html>. It uses third-party components
(Arduino-ESP32 core, ESP-IDF, NimBLE-Arduino) under their own licenses.

© 2026 PulseFlow Engineering
