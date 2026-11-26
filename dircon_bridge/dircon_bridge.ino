/*
 * BLE -> Wahoo Direct Connect (DirCon) bridge: BLE heart rate sensor + cadence sensor / power meter
 * Version: 2026.2.1 (year.major.minor, see FIRMWARE_VERSION)
 * Target : Seeed XIAO ESP32S3 (any ESP32-S3 / ESP32 works)
 * Needs  : Arduino-ESP32 core 3.x, NimBLE-Arduino 2.x
 *
 * How it works
 *   - Wi-Fi and the DirCon TCP server(s) start first. Sensors are found by a background BLE
 *     task; as soon as one connects, its data is bridged. If it drops out, it is found and
 *     reconnected again.
 *   - A DirCon device is announced over mDNS (and accepts app connections) only while at least
 *     one of the BLE sensors it carries is connected to the bridge, and it offers only the
 *     services of the sensors that are connected. When no sensor is left, the announcement is
 *     withdrawn. In the combined layout (SENSOR_GROUP 0) the device appears as soon as the
 *     first sensor connects; when the second one connects later (or one drops out) the mDNS
 *     announcement is updated and apps connected to that device are disconnected once, so they
 *     reconnect and discover the changed set of services.
 *   - Two sensor slots: HR (group 0) and a "cadence slot" (group SENSOR_GROUP). The cadence slot
 *     takes EITHER a cadence sensor (CSC) OR a power meter (Cycling Power, power + cadence),
 *     whichever is found or paired; it is never both. A smart trainer's power then reaches the app
 *     directly, as before, while the power meter's own power + cadence replace it when one is used.
 *       SENSOR_GROUP 1 -> two DirCon devices ("DirCon HR", "DirCon CAD" or "DirCon PWR"),
 *                          each with its own battery level.
 *       SENSOR_GROUP 0 -> ONE DirCon device ("DirCon Sensors") exposing both sensors, NO battery info.
 *     Unpaired, the first cadence sensor or power meter that is seen is used (a device offering
 *     both is taken as power meter). Pairing a sensor locks the slot to that one device.
 *   - Status page: once on Wi-Fi, http://<ip>/ (or http://dircon-bridge-xxxx.local/) shows the state
 *     of the bridge. It runs in its own task and only reads; it never disturbs the bridging. Its
 *     "Enter maintenance mode" button restarts the bridge into maintenance mode: no BLE, no DirCon,
 *     only a settings page (sensor layout, device name, pair / unpair the connected sensors,
 *     firmware upload from a computer, factory reset, restart). The next restart (button on the page, power cycle or 15 min without any use) is
 *     back to normal operation. No password: use it on a trusted network only.
 *   - HR and cadence notifications are forwarded to the app immediately (BLE callback ->
 *     queue -> loop(), which wakes the moment a value arrives).
 *   - Battery: polled from the sensor every BATTERY_POLL_MS, served from cache to DirCon
 *     reads and pushed as a DirCon notification when the level changes.
 *   - The sensor's values (battery, Model #, ...) are read during the BLE connect, i.e. before
 *     the DirCon device is announced, so an app finds them ready when it connects. A read
 *     that is still missing (failed battery read, being retried) is held for a few seconds.
 *   - Pairing: hold the pairing button (PAIR_BUTTON_GPIO) while powering on. The bridge then
 *     first forgets all stored sensors, then scans for a HR and a cadence sensor (the BLE HR and
 *     BLE CAD LEDs blink until their sensor is found, then stay on; pairing only one is fine). Pairing ends when the button is pressed
 *     again, or by itself shortly after both sensors have been found. The found addresses are
 *     stored in NVS and normal operation starts.
 *     Normal operation: a paired sensor is the only one that is connected for its type; a type
 *     that was not paired connects to any sensor found (subject to the HR_/CADENCE_/POWER_SENSOR_NAME/
 *     ADDRESS config filters). Pairing and ending it with nothing found = no filtering at all.
 *   - Wi-Fi setup: the Wi-Fi network is not in the source. When none is stored, or when the AP
 *     button (WIFI_AP_BUTTON_GPIO) is held at power-on, the bridge does nothing else than open an
 *     access point "DirCon-Setup-xxxx" (open, no password) with a web page on 192.168.4.1 (phones
 *     show it as a sign-in page). Pick a 2.4 GHz network from the list (or type its name), enter the
 *     password and press "Save and start": the settings are stored in flash and the bridge restarts
 *     into normal operation. Both buttons held together: factory reset (see factoryResetGesture()). A stored network
 *     that is out of reach does not start the setup; the bridge keeps trying to connect to it.
 *
 * Status: written without access to a compiler or real hardware. The DirCon framing is
 * from memory of the open implementations (qdomyos-zwift "dirconpacket"/"dirconprocessor").
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <mdns.h>          // ESP-IDF mDNS API (used for several instances of one service type)
#include <esp_mac.h>
#include <lwip/sockets.h>  // TCP keepalive options
#include <NimBLEDevice.h>
#include <Preferences.h>   // non-volatile storage (NVS) for the paired sensors and the Wi-Fi network
#include <WebServer.h>     // Wi-Fi setup page, status page, maintenance page
#include <Update.h>        // firmware upload (OTA into the inactive app slot)
#include <DNSServer.h>     // captive portal
#include <vector>
#include <algorithm>
#include <atomic>
#include <string>

// ============================================================================
// USER CONFIG
// ============================================================================
// The Wi-Fi network (2.4 GHz only) is not set here: it is entered on the setup web page of the bridge
// and stored in flash (see "Wi-Fi setup" in the header comment).
static const char*    MDNS_HOSTNAME      = "dircon-bridge";
static const char*    DEVICE_NAME_PREFIX = "DirCon";         // shown in the app: "DirCon HR", "DirCon CAD", ...
// 1 = append the last 4 hex digits of this board's MAC to the DirCon device names ("DirCon HR 3A4F")
//     and to the mDNS hostname ("dircon-bridge-3a4f"), so several bridges on one network can be told apart.
// 0 = names exactly as configured above.
#define USE_MAC_SUFFIX 1
static const char*    COMBINED_SEPARATOR = " ";              // between names in a combined device: "DirCon HR CAD"
static const uint16_t DIRCON_BASE_PORT   = 36866;            // group g listens on BASE + g
static const uint32_t BATTERY_POLL_MS    = 5UL * 60UL * 1000UL;  // 5 min (anything 5..10 min is fine)
static const uint32_t BATTERY_READ_WAIT_MS = 5000;               // an app's battery read that arrives before the sensor is read waits up to this long for the real value
static const uint32_t BATTERY_RETRY_MS   = 3000;                 // until the first real battery read after a connect, retry this often

// 1 = print debug output on the USB serial port. 0 = no logging at all and the serial port is
// never opened (use 0 for normal running: a USB serial port nobody reads can stall the program).
#define DEBUG_SERIAL 0

// --- Which BLE devices to connect to --------------------------------------------------------
// NAME     : accept only devices whose advertised name STARTS with this text ("" = any device
//            that offers the right service). Many sensors advertise their brand and model in the name.
// ADDRESS  : optional, accept only this MAC "aa:bb:cc:dd:ee:ff" ("" = any)
// A sensor paired with the pairing button (stored in NVS) overrides both of these for that sensor.
// Set DEBUG_SERIAL 1 once to see the names of the devices that are found.
#define HR_SENSOR_NAME          ""
#define HR_SENSOR_ADDRESS       ""
#define CADENCE_SENSOR_NAME     ""
#define CADENCE_SENSOR_ADDRESS  ""
#define POWER_SENSOR_NAME       ""      // power meter, see "Cadence slot" in the sensor table
#define POWER_SENSOR_ADDRESS    ""

// --- Smart trainers are ignored -----------------------------------------------------------------
// Smart trainers broadcast power and cadence too, but the app gets those from the trainer itself.
// FILTER_OUT_TRAINERS 1: devices that advertise the Fitness Machine Service (0x1826, FTMS) are never
// used or paired. Trainers that do not advertise it can be excluded by name: add the START of their
// BLE name to the list (case-sensitive), e.g. { "TrainerName", "OtherTrainer", nullptr }. Keep nullptr last.
// Set DEBUG_SERIAL 1 to see the names of the devices found.
#define FILTER_OUT_TRAINERS 1
static const char* const TRAINER_NAME_PREFIXES[] = { nullptr };

// Text shown as "Manufacturer" in the app (Device Information Service, Manufacturer Name String).
// Put your own brand name here. Same for both sensors, at most 64 characters.
#define MANUFACTURER_NAME  "PulseFlow Engineering"

// Firmware version: year.major.minor. Shown in the footer of the Wi-Fi setup page and in the debug
// log at start-up. Keep it equal to the number in the file name when a new version is made.
#define FIRMWARE_VERSION   "2026.2.1"

// --- Status LEDs ------------------------------------------------------------------------------
// One GPIO per LED, -1 = LED not fitted. XIAO ESP32S3 pins: D0=1 D1=2 D2=3 D3=4 D4=5 D5=6 D6=43 D7=44
// D8=7 D9=8 D10=9. GPIO3 (D2) is a boot strapping pin, better not used for an LED.
#define LED_WIFI_GPIO        1   // D0: on = connected to Wi-Fi
#define LED_BLE_HR_GPIO      2   // D1: on = the BLE heart rate sensor is connected
#define LED_BLE_CAD_GPIO     4   // D3: on = the BLE cadence sensor is connected
#define LED_DIRCON_GPIO      5   // D4: on = an app is connected to (one of) the DirCon device(s)
// 0 = LED lights when the pin is HIGH (pin -> resistor -> LED -> GND)
// 1 = LED lights when the pin is LOW  (3.3V -> resistor -> LED -> pin)
#define LED_ACTIVE_LOW       0

// --- Pairing button -----------------------------------------------------------------------------
// Pushbutton between this GPIO and GND (internal pull-up used). XIAO ESP32S3: D9 = GPIO8.
// Only used at power-on (start pairing) and during pairing (press again = finish).
#define PAIR_BUTTON_GPIO  8
#define PAIR_DEBOUNCE_MS  40     // software debounce time
#define PAIR_BLINK_MS     300    // half period of the BLE LED blink while a sensor is still being searched
#define PAIR_DONE_HOLD_MS 1500   // when both sensors are found: how long the LEDs stay lit before pairing ends by itself
// Pairing ignores sensors weaker than this (dBm), so a neighbour's sensor is not paired by accident.
// Hold the sensor close to the bridge. -127 = accept any signal strength.
#define PAIR_MIN_RSSI     -80

// --- Wi-Fi setup button -------------------------------------------------------------------------
// Pushbutton between this GPIO and GND (internal pull-up used). XIAO ESP32S3: D10 = GPIO9.
// Held at power-on = start the Wi-Fi setup (access point + web page). Holding it together with the
// pairing button = factory reset: keep both held 10 s, then the LEDs count down (see factoryResetGesture()).
#define WIFI_AP_BUTTON_GPIO  9

// --- mDNS / DirCon layout ---------------------------------------------------------------------
#define SENSOR_GROUP   1   // 1 = HR and cadence as two DirCon devices (both batteries visible)
                            // 0 = one combined DirCon device (no battery info is forwarded)
static_assert(SENSOR_GROUP == 0 || SENSOR_GROUP == 1, "SENSOR_GROUP must be 0 or 1");
// SENSOR_GROUP is only the default: a layout saved on the maintenance page (NVS) overrides it.

// ============================================================================
// Logging
// ============================================================================
#if DEBUG_SERIAL
  #define LOG(...) Serial.printf(__VA_ARGS__)
#else
  #define LOG(...) do {} while (0)
#endif

// Characteristic property flags (the first three are the values DirCon uses on the wire)
#define PROP_READ   0x01
#define PROP_WRITE  0x02
#define PROP_NOTIFY 0x04
#define PROP_LOCAL  0x40    // internal only: generated by the bridge itself, never read from the sensor
#define PROP_POLLED 0x80    // internal only: bridge re-reads it from the sensor every
                            // BATTERY_POLL_MS instead of subscribing. Never sent on the wire.

constexpr uint8_t  MAX_SENSORS  = 3;   // NimBLE default allows 3 simultaneous connections
constexpr uint8_t  MAX_GROUPS   = 3;
constexpr uint8_t  MAX_SERVICES = 3;   // services per sensor
constexpr uint8_t  MAX_CHARS    = 3;   // characteristics per service
constexpr uint8_t  MAX_CLIENTS  = 2;   // default for simultaneous TCP clients per DirCon device (changeable in maintenance mode)
constexpr uint8_t  MAX_CLIENTS_LIMIT = 5;   // upper end of the setting; size of the client table
constexpr size_t   MAX_VALUE    = 64;  // max bytes of one BLE value we forward
constexpr uint16_t SVC_BATTERY_UUID = 0x180F;
constexpr uint16_t SVC_DEVINFO_UUID = 0x180A;   // Device Information Service
constexpr uint16_t CHR_MANUF_UUID   = 0x2A29;   // Manufacturer Name String
constexpr uint16_t CHR_MODEL_UUID   = 0x2A24;   // Model Number String

struct CharDef {
  uint16_t uuid;
  uint8_t  props;      // PROP_READ / PROP_NOTIFY / PROP_POLLED ...
};

struct ServiceDef {
  uint16_t uuid;
  uint8_t  numChars;
  CharDef  chars[MAX_CHARS];
};

struct SensorDef {
  const char* name;        // short label, also used in the DirCon device name
  const char* namePrefix;  // only accept devices whose name starts with this ("" = any)
  const char* address;     // only accept this MAC "aa:bb:cc:dd:ee:ff" ("" = any)
  uint8_t     group;       // DirCon device / mDNS entry this sensor belongs to
  uint8_t     slot;        // sensors with the same slot are ALTERNATIVES: only one of them is connected
  uint8_t     numServices;
  ServiceDef  services[MAX_SERVICES];   // services[0] = primary (scanned for, put in mDNS TXT)
};

// Battery Service 0x180F / Battery Level 0x2A19 (read + notify toward the app, polled toward the sensor).
#define SVC_BATTERY  { SVC_BATTERY_UUID, 1, { { 0x2A19, PROP_READ | PROP_NOTIFY | PROP_POLLED } } }

// Device Information Service 0x180A, generated by the bridge (nothing is read from the sensor):
//   Manufacturer Name String 0x2A29 = MANUFACTURER_NAME (fixed text from the config above)
//   Model Number String      0x2A24 = BLE name of the sensor (e.g. "<brand> <model> 0249E23C"),
//                                     shown as "Model #" in the app
#define SVC_DEVINFO  { SVC_DEVINFO_UUID, 2, { { CHR_MANUF_UUID, PROP_READ | PROP_LOCAL }, \
                                              { CHR_MODEL_UUID, PROP_READ | PROP_LOCAL } } }

// ----------------------------------------------------------------------------
// THE SENSOR TABLE
// ----------------------------------------------------------------------------
SensorDef SENSORS[] = {   // not const: the "group" of the cadence slot comes from the settings (NVS)
  // Heart rate sensor (Heart Rate service)
  { "HR", HR_SENSOR_NAME, HR_SENSOR_ADDRESS, 0, 0, 3, {
      { 0x180D, 2, { { 0x2A37, PROP_NOTIFY },       // Heart Rate Measurement
                     { 0x2A38, PROP_READ   } } },   // Body Sensor Location
      SVC_BATTERY,
      SVC_DEVINFO } },
  // Cadence slot (slot 1): EITHER a power meter OR a cadence sensor, whichever is found / paired.
  // The power meter is listed first: a device that advertises both services is taken as power meter.
  // Power meter (Cycling Power service). Cadence is part of the
  // Power Measurement packet (crank revolution data) when the meter supports it; forwarded as is.
  { "PWR", POWER_SENSOR_NAME, POWER_SENSOR_ADDRESS, SENSOR_GROUP, 1, 3, {
      { 0x1818, 3, { { 0x2A63, PROP_NOTIFY },       // Cycling Power Measurement
                     { 0x2A65, PROP_READ   },       // Cycling Power Feature
                     { 0x2A5D, PROP_READ   } } },   // Sensor Location
      SVC_BATTERY,
      SVC_DEVINFO } },
  // Cadence sensor (CSC, crank only)
  { "CAD", CADENCE_SENSOR_NAME, CADENCE_SENSOR_ADDRESS, SENSOR_GROUP, 1, 3, {
      { 0x1816, 2, { { 0x2A5B, PROP_NOTIFY },       // CSC Measurement
                     { 0x2A5C, PROP_READ   } } },   // CSC Feature
      SVC_BATTERY,
      SVC_DEVINFO } },
};
const uint8_t NUM_SENSORS = sizeof(SENSORS) / sizeof(SENSORS[0]);
static_assert(sizeof(SENSORS) / sizeof(SENSORS[0]) <= MAX_SENSORS, "raise MAX_SENSORS (and NimBLE max connections)");

// ============================================================================
// Helpers
// ============================================================================
// 16-bit BLE UUID -> 128-bit big-endian (Bluetooth base UUID), as used by DirCon
static void uuid128(uint16_t u16, uint8_t out[16]) {
  static const uint8_t base[16] = {0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,
                                   0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB};
  memcpy(out, base, 16);
  out[2] = u16 >> 8;
  out[3] = u16 & 0xFF;
}
static bool uuidMatches(const uint8_t* in, uint16_t u16) {
  uint8_t t[16];
  uuid128(u16, t);
  return memcmp(in, t, 16) == 0;
}

// ============================================================================
// Sensor state
// ============================================================================
struct CharCache {
  bool    valid = false;
  bool    settled = false;   // no quick retry needed: read from the sensor in this connection, or the sensor does not have it
  uint8_t len   = 0;
  uint8_t data[MAX_VALUE];
};

struct SensorRT {
  NimBLEClient*     client = nullptr;
  std::atomic<bool> connected{false};
  std::atomic<bool> hasFound{false};   // scan found a matching device, bleTask should connect
  bool              disabled = false;    // another sensor of the same slot is paired: this one is not used
  std::string       filterName;          // accept only names starting with this ("" = any); from config
  std::string       filterAddr;          // accept only this MAC ("" = any); from config or paired (NVS)
  std::string       foundAddr;
  std::string       foundName;           // BLE name seen in the scan
  std::string       devName;             // name of the connected sensor (shown as Model # in DirCon)
  uint8_t           foundType = 0;
  uint32_t          lastPoll = 0;
  bool              exposed[MAX_SERVICES] = {true, true, true};  // set by buildGroups()
  CharCache         cache[MAX_SERVICES][MAX_CHARS];
};
static SensorRT rt[MAX_SENSORS];
static char devNameC[MAX_SENSORS][48];     // copy of devName for the status page (plain buffer: safe to read from another task)

// Slots: slot 0 = heart rate, slot 1 = cadence sensor OR power meter (alternatives in one slot).
constexpr uint8_t NUM_SLOTS = 2;
static bool slotConnected(uint8_t slot) {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) if (SENSORS[i].slot == slot && rt[i].connected) return true;
  return false;
}
static bool slotBusy(uint8_t slot) {            // a sensor of this slot is connected or about to be
  for (uint8_t i = 0; i < NUM_SENSORS; i++)
    if (SENSORS[i].slot == slot && (rt[i].connected || rt[i].hasFound)) return true;
  return false;
}

struct NotifyEvent {
  uint8_t sensor, svc, ch, len;
  uint8_t data[MAX_VALUE];
};
static QueueHandle_t notifyQueue;
static portMUX_TYPE  cacheMux = portMUX_INITIALIZER_UNLOCKED;

// Answers for the STATIC readable characteristics until the real value has been read from the
// sensor. Without them an app that reads e.g. the CSC Feature of a not-yet-connected sensor
// would get an error reply and may drop the DirCon connection.
// The battery level and the Model # are deliberately NOT seeded: until they are known, a read is
// held for a few seconds (see parkRead) and then answered with an error. A made-up value would be
// accepted by the app as real and never corrected.
static void seedDefaults() {
  for (uint8_t s = 0; s < NUM_SENSORS; s++)
    for (uint8_t a = 0; a < SENSORS[s].numServices; a++)
      for (uint8_t c = 0; c < SENSORS[s].services[a].numChars; c++) {
        const CharDef& cd = SENSORS[s].services[a].chars[c];
        if (!(cd.props & PROP_READ)) continue;
        CharCache& cc = rt[s].cache[a][c];
        switch (cd.uuid) {
          case CHR_MANUF_UUID: {                                   // brand name from MANUFACTURER_NAME
            size_t n = min(strlen(MANUFACTURER_NAME), MAX_VALUE);
            memcpy(cc.data, MANUFACTURER_NAME, n);
            cc.len = n; cc.valid = true;
            break;
          }
          case 0x2A38: cc.data[0] = 0x00; cc.len = 1; cc.valid = true; break;                 // body sensor location: other
          case 0x2A5C: cc.data[0] = 0x02; cc.data[1] = 0x00; cc.len = 2; cc.valid = true; break; // CSC feature: crank data
        }
      }
}

// ============================================================================
// BLE side: one NimBLE client per sensor, scan + connect + poll task
// ============================================================================
// True for smart trainers (see FILTER_OUT_TRAINERS): they are never bridged or paired.
static bool isSmartTrainer(const NimBLEAdvertisedDevice* dev) {
#if FILTER_OUT_TRAINERS
  if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0x1826))) return true;       // Fitness Machine Service
#endif
  if (TRAINER_NAME_PREFIXES[0]) {
    std::string name = dev->getName();
    for (const char* const* p = TRAINER_NAME_PREFIXES; *p; p++)
      if (name.rfind(*p, 0) == 0) return true;
  }
  return false;
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (isSmartTrainer(dev)) return;
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      SensorRT& r = rt[i];
      const SensorDef& d = SENSORS[i];
      if (r.disabled || slotBusy(d.slot)) continue;
      if (!dev->isAdvertisingService(NimBLEUUID(d.services[0].uuid))) continue;
      if (!r.filterName.empty() && dev->getName().rfind(r.filterName, 0) != 0) continue;
      if (!r.filterAddr.empty() && strcasecmp(dev->getAddress().toString().c_str(), r.filterAddr.c_str()) != 0) continue;
      r.foundAddr = dev->getAddress().toString();
      r.foundType = dev->getAddress().getType();
      r.foundName = dev->getName();
      r.hasFound  = true;
      LOG("[BLE] %s found: %s (%s)\n", d.name, r.foundAddr.c_str(), dev->getName().c_str());
      break;
    }
  }
} scanCallbacks;

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* c, int reason) override {
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      if (rt[i].client == c) {
        rt[i].connected = false;
        LOG("[BLE] %s disconnected (reason %d)\n", SENSORS[i].name, reason);
      }
    }
  }
} clientCallbacks;

// Read one characteristic from the sensor into the cache. A changed value is also queued so
// subscribed DirCon clients get a notification.
static void refreshChar(uint8_t i, uint8_t si, uint8_t ci, NimBLERemoteCharacteristic* ch) {
  NimBLEAttValue v = ch->readValue();
  size_t n = min((size_t)v.size(), MAX_VALUE);
  if (n == 0) {                                         // read failed, keep the old value
    LOG("[BLE] %s 0x%04X read failed, will retry\n", SENSORS[i].name, SENSORS[i].services[si].chars[ci].uuid);
    return;
  }
  CharCache& cc = rt[i].cache[si][ci];
  bool changed = !cc.valid || cc.len != n || memcmp(cc.data, v.data(), n) != 0;

  portENTER_CRITICAL(&cacheMux);
  memcpy(cc.data, v.data(), n);
  cc.len = n;
  cc.valid = true;
  cc.settled = true;
  portEXIT_CRITICAL(&cacheMux);

  LOG("[BLE] %s 0x%04X/0x%04X read: %u (%u bytes)\n", SENSORS[i].name,
      SENSORS[i].services[si].uuid, SENSORS[i].services[si].chars[ci].uuid, v.data()[0], (unsigned)n);

  if (changed && (SENSORS[i].services[si].chars[ci].props & PROP_NOTIFY)) {
    NotifyEvent e;
    e.sensor = i; e.svc = si; e.ch = ci; e.len = n;
    memcpy(e.data, v.data(), n);
    xQueueSend(notifyQueue, &e, 0);
  }
}

// Model # shown in the app = BLE name of the sensor. A DirCon device can show only one Model #, so
// in a combined device (both slots in one group) the names of the connected sensors are joined.
// The text is stored in the Model # characteristic of every sensor row of the group: whichever
// row is connected (and so visible to the app) then has it.
static void publishModel(uint8_t i) {
  uint8_t g = SENSORS[i].group;
  std::string text;
  for (uint8_t s = 0; s < NUM_SENSORS; s++) {
    if (SENSORS[s].group != g) continue;
    if ((s == i || rt[s].connected) && !rt[s].devName.empty()) { if (!text.empty()) text += " + "; text += rt[s].devName; }
  }
  if (text.empty()) return;
  size_t n = min(text.size(), MAX_VALUE);
  for (uint8_t s = 0; s < NUM_SENSORS; s++) {
    if (SENSORS[s].group != g) continue;
    for (uint8_t a = 0; a < SENSORS[s].numServices; a++) {
      if (!rt[s].exposed[a]) continue;
      for (uint8_t c = 0; c < SENSORS[s].services[a].numChars; c++) {
        if (SENSORS[s].services[a].chars[c].uuid != CHR_MODEL_UUID) continue;
        CharCache& cc = rt[s].cache[a][c];
        portENTER_CRITICAL(&cacheMux);
        memcpy(cc.data, text.data(), n);
        cc.len = n;
        cc.valid = true;
        portEXIT_CRITICAL(&cacheMux);
      }
    }
  }
  LOG("[BLE] Model # of group %u: %s\n", g, text.c_str());
}

static bool connectSensor(uint8_t i) {
  SensorRT& r = rt[i];
  const SensorDef& d = SENSORS[i];
  r.hasFound = false;

  LOG("[BLE] connecting %s ...\n", d.name);
  if (!r.client->connect(NimBLEAddress(r.foundAddr, r.foundType))) {
    LOG("[BLE] %s connect failed\n", d.name);
    return false;
  }

  // Values from an earlier connection may be stale: mark them as not yet read in this one.
  for (uint8_t si = 0; si < d.numServices; si++)
    for (uint8_t ci = 0; ci < d.services[si].numChars; ci++) r.cache[si][ci].settled = false;

  // The Model # that the DirCon app shows = manufacturer of the sensor + its BLE name. The name
  // alone is often just a number. The manufacturer is read from the sensor's own Device Information
  // Service (Manufacturer Name String); a sensor without it is shown by its name only.
  std::string manuf;
  NimBLERemoteService* dis = r.client->getService(NimBLEUUID(SVC_DEVINFO_UUID));
  if (dis) {
    NimBLERemoteCharacteristic* mc = dis->getCharacteristic(NimBLEUUID(CHR_MANUF_UUID));
    if (mc && mc->canRead()) {
      NimBLEAttValue mv = mc->readValue();
      manuf.assign(reinterpret_cast<const char*>(mv.data()), mv.size());
      while (!manuf.empty() && (manuf.back() == '\0' || manuf.back() == ' ')) manuf.pop_back();   // trim
    }
  }
  r.devName = r.foundName;
  if (!manuf.empty()) {
    // Do not repeat the brand when the BLE name already starts with it ("<Brand> Sense ..." + "<Brand> Electro Oy").
    std::string brand = manuf.substr(0, manuf.find(' '));
    bool dup = r.foundName.size() >= brand.size() &&
               strncasecmp(r.foundName.c_str(), brand.c_str(), brand.size()) == 0;
    if (!dup) r.devName = r.foundName.empty() ? manuf : manuf + " " + r.foundName;
  }
  LOG("[BLE] %s manufacturer '%s', Model # '%s'\n", d.name, manuf.c_str(), r.devName.c_str());
  strlcpy(devNameC[i], r.devName.c_str(), sizeof(devNameC[i]));
  publishModel(i);

  for (uint8_t si = 0; si < d.numServices; si++) {
    const ServiceDef& sd = d.services[si];
    if (sd.chars[0].props & PROP_LOCAL) continue;       // generated by the bridge: nothing to read from the sensor
    if (si > 0 && !r.exposed[si]) continue;             // service is not forwarded to the app: do not bother the sensor
    NimBLERemoteService* svc = r.client->getService(NimBLEUUID(sd.uuid));
    if (!svc) {
      if (si == 0) {                                    // primary service is mandatory
        LOG("[BLE] %s: primary service 0x%04X not found\n", d.name, sd.uuid);
        r.client->disconnect();
        return false;
      }
      LOG("[BLE] %s: optional service 0x%04X not on sensor, skipped\n", d.name, sd.uuid);
      for (uint8_t ci = 0; ci < sd.numChars; ci++) r.cache[si][ci].settled = true;   // nothing to wait for
      continue;
    }
    for (uint8_t ci = 0; ci < sd.numChars; ci++) {
      uint8_t props = sd.chars[ci].props;
      NimBLERemoteCharacteristic* ch = svc->getCharacteristic(NimBLEUUID(sd.chars[ci].uuid));
      if (!ch) { LOG("[BLE] %s: char 0x%04X not on sensor\n", d.name, sd.chars[ci].uuid); r.cache[si][ci].settled = true; continue; }

      // Real value replaces the default answer (kept if the sensor does not have the characteristic)
      if ((props & PROP_READ) && ch->canRead()) refreshChar(i, si, ci, ch);

      // Notify chars are subscribed on the sensor, except POLLED ones (battery).
      if ((props & PROP_NOTIFY) && !(props & PROP_POLLED) && ch->canNotify()) {
        bool ok = ch->subscribe(true,
          [i, si, ci](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
            NotifyEvent e;
            e.sensor = i; e.svc = si; e.ch = ci;
            e.len = min(len, MAX_VALUE);
            memcpy(e.data, data, e.len);
            xQueueSend(notifyQueue, &e, 0);   // loop() wakes immediately and forwards it
          });
        if (!ok) { LOG("[BLE] %s: subscribe failed\n", d.name); r.client->disconnect(); return false; }
      }
    }
  }
  r.lastPoll = millis();
  r.connected = true;
  LOG("[BLE] %s ready\n", d.name);
  return true;
}

// Periodically re-read every POLLED characteristic (battery level).
static void pollSensor(uint8_t i) {
  SensorRT& r = rt[i];
  const SensorDef& d = SENSORS[i];
  r.lastPoll = millis();
  if (!r.client->isConnected()) return;
  for (uint8_t si = 0; si < d.numServices; si++) {
    if (!r.exposed[si]) continue;
    const ServiceDef& sd = d.services[si];
    NimBLERemoteService* svc = nullptr;
    for (uint8_t ci = 0; ci < sd.numChars; ci++) {
      if (!(sd.chars[ci].props & PROP_POLLED)) continue;
      if (!svc) svc = r.client->getService(NimBLEUUID(sd.uuid));
      if (!svc) break;
      NimBLERemoteCharacteristic* ch = svc->getCharacteristic(NimBLEUUID(sd.chars[ci].uuid));
      if (ch && ch->canRead()) refreshChar(i, si, ci, ch);
    }
  }
}

// True while a polled characteristic (battery) has not yet been read from the sensor in the
// current connection: it is retried every BATTERY_RETRY_MS instead of waiting for BATTERY_POLL_MS.
static bool batteryPending(uint8_t i) {
  const SensorDef& d = SENSORS[i];
  for (uint8_t si = 0; si < d.numServices; si++) {
    if (!rt[i].exposed[si]) continue;
    for (uint8_t ci = 0; ci < d.services[si].numChars; ci++)
      if ((d.services[si].chars[ci].props & PROP_POLLED) && !rt[i].cache[si][ci].settled) return true;
  }
  return false;
}

static void bleTask(void*) {
  NimBLEScan* scan = NimBLEDevice::getScan();
  for (;;) {
    bool needScan = false, anyConnected = false;
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      if (rt[i].connected) {
        anyConnected = true;
        uint32_t every = batteryPending(i) ? BATTERY_RETRY_MS : BATTERY_POLL_MS;
        if (millis() - rt[i].lastPoll >= every) pollSensor(i);
        continue;
      }
      if (rt[i].disabled || slotConnected(SENSORS[i].slot)) continue;   // not wanted / its slot is taken
      needScan = true;
      if (rt[i].hasFound) {
        if (scan->isScanning()) scan->stop();
        if (!connectSensor(i)) rt[i].hasFound = false;
      }
    }
    if (needScan && !scan->isScanning()) {
      // Time-boxed scan windows: the controller filters duplicate reports within a scan, so
      // restarting lets a sensor that dropped out be found again. While another sensor is
      // already streaming, scan with a lower duty cycle so its notifications are not delayed.
      scan->setWindow(anyConnected ? 32 : 80);
      scan->clearResults();
      scan->start(8000, false, true);
    } else if (!needScan && scan->isScanning()) {
      scan->stop();
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

static bool bleInited = false;   // pairing mode may already have initialised NimBLE

static void bleBegin() {
  if (!bleInited) { NimBLEDevice::init("dircon-bridge"); bleInited = true; }
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    rt[i].client = NimBLEDevice::createClient();
    rt[i].client->setClientCallbacks(&clientCallbacks, false);
    rt[i].client->setConnectTimeout(5000);   // ms in NimBLE-Arduino 2.x
  }
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(160);   // 100 ms
  scan->setWindow(80);      // 50 ms -> leaves radio time for Wi-Fi
  xTaskCreate(bleTask, "ble", 8192, nullptr, 1, nullptr);
}

// ============================================================================
// DirCon side: one instance per group (TCP server + mDNS service)
// ============================================================================
// Header (6 bytes): version | message id | sequence | response code | length (big endian)
constexpr uint8_t DC_VERSION = 0x01;
enum : uint8_t {
  DC_DISCOVER_SERVICES        = 0x01,
  DC_DISCOVER_CHARACTERISTICS = 0x02,
  DC_READ_CHARACTERISTIC      = 0x03,
  DC_WRITE_CHARACTERISTIC     = 0x04,
  DC_ENABLE_NOTIFICATIONS     = 0x05,
  DC_NOTIFICATION             = 0x06,   // unsolicited, device -> app
};
enum : uint8_t {
  RC_OK = 0, RC_UNKNOWN_MSG = 1, RC_UNEXPECTED = 2, RC_SERVICE_NOT_FOUND = 3,
  RC_CHAR_NOT_FOUND = 4, RC_OP_NOT_SUPPORTED = 5,
};

static void sendMsg(WiFiClient& s, uint8_t id, uint8_t seq, uint8_t rc,
                    const uint8_t* payload, size_t n) {
  uint8_t out[6 + 160];
  if (n > 160) n = 0;
  out[0] = DC_VERSION; out[1] = id; out[2] = seq; out[3] = rc;
  out[4] = n >> 8;     out[5] = n & 0xFF;
  if (n) memcpy(out + 6, payload, n);
  s.write(out, 6 + n);
}

// Detect an app that vanished without closing the connection (crash, Wi-Fi loss, power off) within
// roughly 10-15 s instead of minutes, so the DirCon LED goes out and the connection slot is freed.
static void enableKeepAlive(WiFiClient& s) {
  int fd = s.fd();
  if (fd < 0) return;
  int on = 1, idle = 5, interval = 2, count = 3;   // first probe after 5 s of silence, then every 2 s, give up after 3
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

// User settings (maintenance page, NVS namespace "settings"); loaded early in setup().
static uint8_t cfgGroup = SENSOR_GROUP;    // 1 = separate devices, 0 = combined
static String  cfgName;                    // "" = use the MAC suffix in the device names
static uint8_t cfgMaxClients = MAX_CLIENTS;  // simultaneous app connections per DirCon device (2..MAX_CLIENTS_LIMIT)
static bool      cfgDhcp = true;           // true = address from DHCP, false = fixed address below
static IPAddress cfgIp, cfgMask, cfgGw;    // used when cfgDhcp is false (gateway 0.0.0.0 = none)
static IPAddress cfgDns;                   // optional DNS server for the fixed address (0.0.0.0 = none)

// Last two bytes of this board's Wi-Fi MAC as 4 hex digits ("3A4F"), same on every call.
// Readable from the chip before Wi-Fi is started.
static String macSuffix() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char t[5];
  snprintf(t, sizeof(t), "%02X%02X", m[4], m[5]);
  return String(t);
}

class DirconDevice {
 public:
  uint8_t     group;
  uint16_t    port;
  String      instance;
  WiFiServer* server = nullptr;

  struct Client {
    WiFiClient sock;
    bool       active = false;
    uint32_t   since = 0;
    uint8_t    buf[128];
    size_t     len = 0;
    uint8_t    subs[MAX_SENSORS][MAX_SERVICES] = {{0}};  // bit c = notifications on for char c
    struct PendingRead {                                   // read requests whose reply is held back
      bool active = false; uint8_t seq = 0, s = 0, a = 0, c = 0; uint32_t deadline = 0;
    } pending[2];
  } clients[MAX_CLIENTS_LIMIT];

  DirconDevice(uint8_t g) : group(g), port(DIRCON_BASE_PORT + g) { buildInstance(); }

  // Device name: prefix + one sensor name per slot (the connected one of the slot, else its first),
  // e.g. "DirCon HR 3A4F". A combined device (several slots) is always "DirCon Sensors 3A4F". Rebuilt before every announcement.
  void buildInstance() {
    instance = DEVICE_NAME_PREFIX;
    bool first = true;
    uint8_t slotsInGroup = 0;                    // more than one slot = combined device: fixed name
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++)
      for (uint8_t s = 0; s < NUM_SENSORS; s++)
        if (SENSORS[s].group == group && SENSORS[s].slot == sl) { slotsInGroup++; break; }
    if (slotsInGroup > 1) { instance += " Sensors"; first = false; }
    for (uint8_t sl = 0; sl < NUM_SLOTS && slotsInGroup <= 1; sl++) {
      int pick = -1;
      for (uint8_t s = 0; s < NUM_SENSORS; s++) {
        if (SENSORS[s].group != group || SENSORS[s].slot != sl) continue;
        if (pick < 0) pick = s;
        if (rt[s].connected) { pick = s; break; }
      }
      if (pick < 0) continue;
      instance += first ? " " : COMBINED_SEPARATOR;
      instance += SENSORS[pick].name;
      first = false;
    }
    if (cfgName.length()) { instance += " "; instance += cfgName; }       // own name replaces the suffix
#if USE_MAC_SUFFIX
    else { instance += " "; instance += macSuffix(); }
#endif
  }

  void begin() {
    server = new WiFiServer(port);
    server->begin();
    server->setNoDelay(true);
    // Not advertised yet: mdnsRefresh() announces this device once a sensor of it is connected.
  }

  // Bit s set = BLE sensor s belongs to this DirCon device and is connected to the bridge.
  // The device is announced and accepts apps only while this is not 0.
  uint8_t readyMask() {
    uint8_t m = 0;
    for (uint8_t s = 0; s < NUM_SENSORS; s++)
      if (SENSORS[s].group == group && rt[s].connected) m |= (1 << s);
    return m;
  }

  // Service a of sensor s is offered to apps: forwarded at all, and the sensor is connected now.
  bool visible(uint8_t s, uint8_t a) { return rt[s].exposed[a] && rt[s].connected; }

  uint8_t liveMask = 0;     // readyMask() as last seen by poll()

  // ---- mDNS: _wahoo-fitness-tnp._tcp with the TXT records the apps look for ----
  // Only each sensor's PRIMARY service goes into ble-service-uuids (battery is left out
  // on purpose, so the app classifies the device by its HR / CSC service).
  void advertise() {
    buildInstance();
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    m[5] += group + 1;                       // unique pseudo MAC per device
    char mac[18], serial[32], uuids[64] = "";
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(serial, sizeof(serial), "BRIDGE-%02X%02X%02X-%u", m[3], m[4], m[5], group);
    for (uint8_t s = 0; s < NUM_SENSORS; s++) {
      if (SENSORS[s].group != group || !rt[s].exposed[0] || !rt[s].connected) continue;
      char t[12];
      snprintf(t, sizeof(t), "%s0x%04X", uuids[0] ? "," : "", SENSORS[s].services[0].uuid);
      strlcat(uuids, t, sizeof(uuids));
    }
    mdns_txt_item_t txt[3] = {
      { "mac-address",       mac },
      { "serial-number",     serial },
      { "ble-service-uuids", uuids },
    };
    esp_err_t err = mdns_service_add(instance.c_str(), "_wahoo-fitness-tnp", "_tcp", port, txt, 3);
    (void)err;
    LOG("[DirCon] '%s' port %u uuids %s -> %s\n", instance.c_str(), port, uuids,
        err == ESP_OK ? "advertised" : esp_err_to_name(err));
  }

  // ---- lookup helpers (only sensors of this group, only exposed services) ----
  // DirCon read/notify requests carry the characteristic UUID only, so characteristic
  // UUIDs must be unique within one DirCon device (buildGroups() guarantees it for services).
  bool findChar(const uint8_t* uuid, int& s, int& si, int& c) {
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      if (SENSORS[i].group != group) continue;
      for (uint8_t a = 0; a < SENSORS[i].numServices; a++) {
        if (!visible(i, a)) continue;
        for (uint8_t k = 0; k < SENSORS[i].services[a].numChars; k++)
          if (uuidMatches(uuid, SENSORS[i].services[a].chars[k].uuid)) { s = i; si = a; c = k; return true; }
      }
    }
    return false;
  }

  // ---- request handling ----
  void handle(Client& cl, uint8_t id, uint8_t seq, const uint8_t* p, size_t n) {
    uint8_t pl[128];
    size_t  pn = 0;
    uint8_t rc = RC_OK;

    switch (id) {
      case DC_DISCOVER_SERVICES:
        for (uint8_t s = 0; s < NUM_SENSORS; s++) {
          if (SENSORS[s].group != group) continue;
          for (uint8_t a = 0; a < SENSORS[s].numServices; a++) {
            if (!visible(s, a)) continue;
            uuid128(SENSORS[s].services[a].uuid, pl + pn); pn += 16;
          }
        }
        break;

      case DC_DISCOVER_CHARACTERISTICS: {
        int fs = -1, fa = -1;
        if (n >= 16)
          for (uint8_t s = 0; s < NUM_SENSORS; s++)
            if (SENSORS[s].group == group)
              for (uint8_t a = 0; a < SENSORS[s].numServices; a++)
                if (visible(s, a) && uuidMatches(p, SENSORS[s].services[a].uuid)) { fs = s; fa = a; }
        if (fs < 0) { rc = RC_SERVICE_NOT_FOUND; break; }
        memcpy(pl, p, 16); pn = 16;                       // echo service UUID
        const ServiceDef& sd = SENSORS[fs].services[fa];
        for (uint8_t k = 0; k < sd.numChars; k++) {
          uuid128(sd.chars[k].uuid, pl + pn); pn += 16;
          pl[pn++] = sd.chars[k].props & 0x07;            // strip internal flags
        }
        break;
      }

      case DC_READ_CHARACTERISTIC: {
        int s, a, c;
        if (n < 16 || !findChar(p, s, a, c)) { rc = RC_CHAR_NOT_FOUND; break; }
        if (!(SENSORS[s].services[a].chars[c].props & PROP_READ)) { rc = RC_OP_NOT_SUPPORTED; break; }
        memcpy(pl, p, 16); pn = 16;
        bool ok;
        portENTER_CRITICAL(&cacheMux);
        ok = rt[s].cache[a][c].valid;
        if (ok) { memcpy(pl + 16, rt[s].cache[a][c].data, rt[s].cache[a][c].len); pn += rt[s].cache[a][c].len; }
        portEXIT_CRITICAL(&cacheMux);
        if (!ok) {
          // Not read from the sensor yet (e.g. its first battery read failed and is being retried):
          // hold the reply for a few seconds instead of answering with an error. servePending()
          // sends it as soon as the value is there, or an error after BATTERY_READ_WAIT_MS.
          if (parkRead(cl, seq, s, a, c)) {
            LOG("[DirCon] %s: read 0x%04X held until the sensor has been read\n", instance.c_str(),
                SENSORS[s].services[a].chars[c].uuid);
            return;
          }
          rc = RC_UNEXPECTED; pn = 0;
        }
        break;
      }

      case DC_WRITE_CHARACTERISTIC:       // HR/CSC/battery need no writes; extend here for control points
        rc = RC_OP_NOT_SUPPORTED;
        break;

      case DC_ENABLE_NOTIFICATIONS: {
        int s, a, c;
        if (n < 17 || !findChar(p, s, a, c)) { rc = RC_CHAR_NOT_FOUND; break; }
        if (!(SENSORS[s].services[a].chars[c].props & PROP_NOTIFY)) { rc = RC_OP_NOT_SUPPORTED; break; }
        if (p[16]) cl.subs[s][a] |=  (1 << c);
        else       cl.subs[s][a] &= ~(1 << c);
        memcpy(pl, p, 16); pn = 16;
        break;
      }

      default:
        rc = RC_UNKNOWN_MSG;
    }
    LOG("[DirCon] %s: request 0x%02X -> reply code %u (%u bytes)\n", instance.c_str(), id, rc, (unsigned)pn);
    sendMsg(cl.sock, id, seq, rc, rc == RC_OK ? pl : nullptr, rc == RC_OK ? pn : 0);
  }

  // ---- held-back read replies ----
  bool parkRead(Client& cl, uint8_t seq, int s, int a, int c) {
    for (auto& pr : cl.pending) {
      if (pr.active) continue;
      pr.active = true; pr.seq = seq; pr.s = s; pr.a = a; pr.c = c;
      pr.deadline = millis() + BATTERY_READ_WAIT_MS;
      return true;
    }
    return false;
  }

  void servePending(Client& cl) {
    for (auto& pr : cl.pending) {
      if (!pr.active) continue;
      CharCache& cc = rt[pr.s].cache[pr.a][pr.c];
      bool have = cc.valid;
      bool late = (int32_t)(millis() - pr.deadline) >= 0;
      if (!have && !late) continue;
      uint8_t pl[16 + MAX_VALUE];
      size_t  pn = 0;
      uuid128(SENSORS[pr.s].services[pr.a].chars[pr.c].uuid, pl);
      if (have) {
        portENTER_CRITICAL(&cacheMux);
        pn = cc.len;
        memcpy(pl + 16, cc.data, pn);
        portEXIT_CRITICAL(&cacheMux);
        pn += 16;
      }
      LOG("[DirCon] %s: held read 0x%04X answered (%s)\n", instance.c_str(),
          SENSORS[pr.s].services[pr.a].chars[pr.c].uuid, have ? "value" : "timeout");
      sendMsg(cl.sock, DC_READ_CHARACTERISTIC, pr.seq, have ? RC_OK : RC_UNEXPECTED,
              have ? pl : nullptr, have ? pn : 0);
      pr.active = false;
    }
  }

  // ---- forward a BLE notification (or a battery change) to subscribed TCP clients ----
  void notify(uint8_t s, uint8_t a, uint8_t c, const uint8_t* data, size_t len) {
    if (SENSORS[s].group != group || !rt[s].exposed[a]) return;
    uint8_t pl[16 + MAX_VALUE];
    uuid128(SENSORS[s].services[a].chars[c].uuid, pl);
    memcpy(pl + 16, data, len);
    for (auto& cl : clients)
      if (cl.active && cl.sock.connected() && ((cl.subs[s][a] >> c) & 1))
        sendMsg(cl.sock, DC_NOTIFICATION, 0, RC_OK, pl, 16 + len);
  }

  // ---- TCP housekeeping, call often from loop() ----
  void drop(Client& cl) {
    cl.sock.stop();
    cl.active = false;
    cl.len = 0;
    memset(cl.subs, 0, sizeof(cl.subs));
    for (auto& pr : cl.pending) pr.active = false;
  }

  void poll() {
    WiFiClient nc = server->accept();
    uint8_t ready = readyMask();
    if (ready != liveMask) {                    // a sensor of this device connected or dropped out:
      liveMask = ready;                         // apps must reconnect to see the changed services
      for (auto& cl : clients) if (cl.active) { LOG("[DirCon] %s: sensors changed, closing client\n", instance.c_str()); drop(cl); }
    }
    if (!ready) {                               // no sensor connected: no service for apps
      if (nc) nc.stop();
      return;
    }
    if (nc) {
      Client* slot = nullptr;
      for (uint8_t k = 0; k < cfgMaxClients; k++) if (!clients[k].active) { slot = &clients[k]; break; }
      if (!slot) {                              // full: evict the oldest
        slot = &clients[0];
        for (uint8_t k = 1; k < cfgMaxClients; k++) if (clients[k].since < slot->since) slot = &clients[k];
        drop(*slot);
      }
      slot->sock = nc;
      slot->sock.setNoDelay(true);
      enableKeepAlive(slot->sock);
      slot->active = true;
      slot->since = millis();
      slot->len = 0;
      memset(slot->subs, 0, sizeof(slot->subs));
      for (auto& pr : slot->pending) pr.active = false;
      LOG("[DirCon] %s: client connected\n", instance.c_str());
    }

    for (auto& cl : clients) {
      if (!cl.active) continue;
      if (!cl.sock.connected()) { LOG("[DirCon] %s: client left\n", instance.c_str()); drop(cl); continue; }

      int avail = cl.sock.available();
      if (avail > 0) {
        size_t room = sizeof(cl.buf) - cl.len;
        int r = cl.sock.read(cl.buf + cl.len, min((size_t)avail, room));
        if (r > 0) cl.len += r;
      }
      while (cl.len >= 6) {
        size_t plen = ((size_t)cl.buf[4] << 8) | cl.buf[5];
        if (plen > sizeof(cl.buf) - 6) { drop(cl); break; }   // oversized: unsupported
        size_t total = 6 + plen;
        if (cl.len < total) break;
        handle(cl, cl.buf[1], cl.buf[2], cl.buf + 6, plen);
        memmove(cl.buf, cl.buf + total, cl.len - total);
        cl.len -= total;
      }
      servePending(cl);
    }
  }

  int clientCount() { int n = 0; for (auto& cl : clients) n += cl.active; return n; }
};

static DirconDevice* groups[MAX_GROUPS] = {nullptr};
static uint8_t numGroups = 0;

// A DirCon device is announced over mDNS while at least one of its BLE sensors is connected, with
// the services of the connected sensors in the TXT record. When that set changes for any group,
// all services are removed (goodbye packets) and the groups that have a sensor are announced again.
static void mdnsRefresh() {
  static uint8_t announced[MAX_GROUPS] = {0};           // sensor mask per group as last announced
  uint8_t now[MAX_GROUPS] = {0};
  bool changed = false;
  for (uint8_t g = 0; g < numGroups; g++) {
    if (groups[g]) now[g] = groups[g]->readyMask();
    if (now[g] != announced[g]) changed = true;
  }
  if (!changed) return;
  mdns_service_remove_all();
  for (uint8_t g = 0; g < numGroups; g++) {
    announced[g] = now[g];
    if (now[g]) groups[g]->advertise();
  }
}

static void buildGroups() {
  for (uint8_t s = 0; s < NUM_SENSORS; s++) {
    if (SENSORS[s].group >= MAX_GROUPS) { LOG("[CFG] sensor %s: group out of range\n", SENSORS[s].name); continue; }
    if (SENSORS[s].group + 1 > numGroups) numGroups = SENSORS[s].group + 1;
  }
  for (uint8_t g = 0; g < numGroups; g++) {
    uint8_t slotMask = 0;
    for (uint8_t s = 0; s < NUM_SENSORS; s++) if (SENSORS[s].group == g) slotMask |= (1 << SENSORS[s].slot);
    if (!slotMask) continue;                                // empty group numbers are skipped
    uint8_t count = 0;                                      // number of slots (alternatives count once)
    for (uint8_t k = 0; k < 8; k++) count += (slotMask >> k) & 1;

    // Several sensors in one DirCon device: a single battery service cannot say which sensor
    // it belongs to, so no battery info is forwarded for this device at all.
    if (count > 1) {
      for (uint8_t s = 0; s < NUM_SENSORS; s++) {
        if (SENSORS[s].group != g) continue;
        for (uint8_t a = 0; a < SENSORS[s].numServices; a++)
          if (SENSORS[s].services[a].uuid == SVC_BATTERY_UUID) {
            rt[s].exposed[a] = false;
            LOG("[CFG] group %u: battery of %s not forwarded (combined device)\n", g, SENSORS[s].name);
          }
      }
    }

    // A service UUID can appear only once per DirCon device: the first sensor that owns it
    // wins, later duplicates are hidden. Alternatives in the same slot never conflict: only one of
    // them is connected (and so visible) at a time.
    uint16_t seen[MAX_SENSORS * MAX_SERVICES];
    uint8_t  seenSlot[MAX_SENSORS * MAX_SERVICES];
    uint8_t  nseen = 0;
    for (uint8_t s = 0; s < NUM_SENSORS; s++) {
      if (SENSORS[s].group != g) continue;
      for (uint8_t a = 0; a < SENSORS[s].numServices; a++) {
        if (!rt[s].exposed[a]) continue;
        uint16_t u = SENSORS[s].services[a].uuid;
        bool dup = false;
        for (uint8_t k = 0; k < nseen; k++) dup |= (seen[k] == u && seenSlot[k] != SENSORS[s].slot);
        if (dup) {
          rt[s].exposed[a] = false;
          LOG("[CFG] group %u: service 0x%04X of %s hidden (already exposed by another sensor in the group)\n",
              g, u, SENSORS[s].name);
        } else {
          seen[nseen] = u; seenSlot[nseen] = SENSORS[s].slot; nseen++;
        }
      }
    }
    groups[g] = new DirconDevice(g);
  }
}

// ============================================================================
// Status LEDs
//   WIFI    : connected to the Wi-Fi network
//   DIRCON  : at least one app is connected to one of the DirCon devices
//   BLE HR  : the heart rate sensor is connected over BLE
//   BLE CAD : the cadence sensor or power meter (slot 1) is connected over BLE
// A sensor LED goes off briefly every 3 s while that sensor's battery is below 25 %.
// At power-on all four flash briefly. In pairing mode BLE HR / BLE CAD blink until their sensor
// has been found, then stay on (see runPairing()).
// ============================================================================
constexpr uint8_t SENSOR_HR = 0;                      // row number in SENSORS[]
constexpr uint8_t LED_WIFI = 0, LED_DIRCON = 1, LED_BLE_HR = 2, LED_BLE_CAD = 3;   // index in LED_PINS[]
static const int8_t LED_PINS[4] = { LED_WIFI_GPIO, LED_DIRCON_GPIO, LED_BLE_HR_GPIO, LED_BLE_CAD_GPIO };

static void ledWrite(int8_t pin, bool on) {
  if (pin < 0) return;
  digitalWrite(pin, (on != (LED_ACTIVE_LOW != 0)) ? HIGH : LOW);
}

// Battery level (0..100) of a connected sensor, read in this connection; -1 when not known (yet).
static int batteryPercent(uint8_t i) {
  int level = -1;
  const SensorDef& d = SENSORS[i];
  for (uint8_t si = 0; si < d.numServices; si++) {
    if (d.services[si].uuid != SVC_BATTERY_UUID) continue;
    for (uint8_t ci = 0; ci < d.services[si].numChars; ci++) {
      if (d.services[si].chars[ci].uuid != 0x2A19) continue;
      portENTER_CRITICAL(&cacheMux);
      const CharCache& cc = rt[i].cache[si][ci];
      if (cc.valid && cc.settled && cc.len >= 1) level = cc.data[0];
      portEXIT_CRITICAL(&cacheMux);
    }
  }
  return level > 100 ? -1 : level;
}

static void ledsBegin() {
  for (int8_t pin : LED_PINS) if (pin >= 0) { pinMode(pin, OUTPUT); ledWrite(pin, false); }
  for (int8_t pin : LED_PINS) ledWrite(pin, true);    // short self-test so the wiring can be checked
  delay(400);
  for (int8_t pin : LED_PINS) ledWrite(pin, false);
}

static bool anyAppConnected() {
  for (uint8_t g = 0; g < numGroups; g++)
    if (groups[g] && groups[g]->clientCount() > 0) return true;
  return false;
}

// Low battery (below BATT_LOW_PERCENT) of the connected sensor of a slot; unknown level = not low.
constexpr int      BATT_LOW_PERCENT = 25;
constexpr uint32_t BATT_DIP_PERIOD_MS = 3000;   // a low battery turns the sensor LED off once per period ...
constexpr uint32_t BATT_DIP_LEN_MS    = 200;    // ... for this long
static bool slotBatteryLow(uint8_t slot) {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    if (SENSORS[i].slot != slot || !rt[i].connected) continue;
    int b = batteryPercent(i);
    return b >= 0 && b < BATT_LOW_PERCENT;
  }
  return false;
}

static void ledsUpdate() {
  bool state[4] = {
    WiFi.status() == WL_CONNECTED,
    anyAppConnected(),
    rt[SENSOR_HR].connected.load(),
    slotConnected(1),                                 // cadence sensor or power meter
  };
  // Low battery: the sensor LED is briefly turned off every 3 seconds.
  if ((millis() % BATT_DIP_PERIOD_MS) < BATT_DIP_LEN_MS) {
    if (state[LED_BLE_HR]  && slotBatteryLow(0)) state[LED_BLE_HR]  = false;
    if (state[LED_BLE_CAD] && slotBatteryLow(1)) state[LED_BLE_CAD] = false;
  }
  static int8_t last[4] = {-1, -1, -1, -1};           // only touch a pin when its state changes
  for (uint8_t i = 0; i < 4; i++)
    if (last[i] != (int8_t)state[i]) { last[i] = (int8_t)state[i]; ledWrite(LED_PINS[i], state[i]); }
}

// ============================================================================
// Pairing (stored sensor addresses) + pairing mode
// ============================================================================
static Preferences prefs;
static const char* NVS_NAMESPACE = "dircon";

// Start from the compile-time filters, then let a paired address (NVS) override them.
static void loadFilters() {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    rt[i].filterName = SENSORS[i].namePrefix;
    rt[i].filterAddr = SENSORS[i].address;
  }
  bool stored[MAX_SENSORS] = {false};
  prefs.begin(NVS_NAMESPACE, false);
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    String key = String("a_") + SENSORS[i].name;
    String addr = prefs.getString(key.c_str(), "");
    if (addr.length()) {
      rt[i].filterAddr = addr.c_str();
      rt[i].filterName = "";            // the paired address identifies the sensor; ignore the name filter
      stored[i] = true;
      LOG("[PAIR] %s uses paired address %s\n", SENSORS[i].name, addr.c_str());
    }
  }
  prefs.end();

  // A slot with a paired sensor uses only that sensor: its alternatives (e.g. the cadence sensor
  // when a power meter was paired) are switched off, so they cannot grab some other device.
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    rt[i].disabled = false;
    if (stored[i]) continue;
    for (uint8_t k = 0; k < NUM_SENSORS; k++)
      if (k != i && stored[k] && SENSORS[k].slot == SENSORS[i].slot) rt[i].disabled = true;
    if (rt[i].disabled) LOG("[PAIR] %s not used (slot taken by a paired sensor)\n", SENSORS[i].name);
  }
}

struct PairResult {
  std::atomic<bool> found{false};
  std::string       addr;               // written once by the scan callback, before 'found' is set
};
static PairResult pairRes[MAX_SENSORS];

static bool pairSlotFound(uint8_t slot) {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) if (SENSORS[i].slot == slot && pairRes[i].found) return true;
  return false;
}

class PairScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (dev->getRSSI() < PAIR_MIN_RSSI) return;
    if (isSmartTrainer(dev)) return;
    std::string addr = dev->getAddress().toString();
    for (uint8_t i = 0; i < NUM_SENSORS; i++)
      if (pairRes[i].found && pairRes[i].addr == addr) return;   // already taken by a sensor
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      if (pairSlotFound(SENSORS[i].slot)) continue;              // first sensor found in a slot wins
      if (!dev->isAdvertisingService(NimBLEUUID(SENSORS[i].services[0].uuid))) continue;
      pairRes[i].addr = addr;
      pairRes[i].found = true;
      LOG("[PAIR] %s found: %s (%s)\n", SENSORS[i].name, addr.c_str(), dev->getName().c_str());
      break;
    }
  }
} pairCallbacks;

// Software-debounced button. btnPressEvent() returns true once per press (on the debounced
// transition to pressed). Button is active low (pin -> button -> GND).
static bool     btnStable = false, btnLastRaw = false;
static uint32_t btnChangedAt = 0;

static bool btnPressEvent() {
  bool raw = digitalRead(PAIR_BUTTON_GPIO) == LOW;
  uint32_t now = millis();
  if (raw != btnLastRaw) { btnLastRaw = raw; btnChangedAt = now; }
  if (raw != btnStable && now - btnChangedAt >= PAIR_DEBOUNCE_MS) {
    btnStable = raw;
    return raw;
  }
  return false;
}

// True when the button on this pin is held (stable for PAIR_DEBOUNCE_MS) at power-on.
static bool buttonHeldAtBoot(int pin) {
  pinMode(pin, INPUT_PULLUP);
  delay(10);                                         // let the pull-up settle
  uint32_t t0 = millis();
  while (millis() - t0 < PAIR_DEBOUNCE_MS) {
    if (digitalRead(pin) != LOW) return false;
    delay(1);
  }
  return true;
}

static void runPairing() {
  LOG("[PAIR] pairing mode: press the button again to finish\n");
  for (auto& p : pairRes) { p.found = false; p.addr.clear(); }

  // Starting pairing forgets every stored sensor. Ending pairing without having found a sensor
  // therefore leaves the bridge in the un-filtered "connect to any sensor" state.
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  LOG("[PAIR] stored sensors cleared\n");

  NimBLEDevice::init("dircon-bridge");
  bleInited = true;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&pairCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(160);
  scan->setWindow(150);                              // radio is free: Wi-Fi is not running yet
  scan->clearResults();
  scan->start(0, false, true);                       // 0 = scan until stopped

  // The button is still held down from power-on: the first events are its release, then a new press.
  btnStable = btnLastRaw = true;
  btnChangedAt = millis();

  uint32_t allFoundAt = 0;                           // when the last sensor was found (0 = not yet)
  for (;;) {
    uint8_t nFound = 0;
    // BLE HR / BLE CAD LED: blinks until its sensor has been found, then stays on. (Regular boot
    // only flashes all LEDs together once, so this is clearly different.)
    bool blink = ((millis() / PAIR_BLINK_MS) & 1) != 0;
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
      bool f = pairSlotFound(sl);
      if (f) nFound++;
      ledWrite(LED_PINS[LED_BLE_HR + sl], f ? true : blink);   // slot 0 = BLE HR LED, slot 1 = BLE CAD LED
    }
    if (btnPressEvent()) break;
    // Both slots filled (HR + cadence sensor or power meter): finish by itself after a short pause,
    // so the user sees both LEDs light up.
    if (nFound == NUM_SLOTS) {
      if (!allFoundAt) allFoundAt = millis() ? millis() : 1;
      if (millis() - allFoundAt >= PAIR_DONE_HOLD_MS) { LOG("[PAIR] both sensors found, finishing\n"); break; }
    }
    if (!scan->isScanning()) scan->start(0, false, true);
    delay(10);
  }

  scan->stop();
  scan->clearResults();

  // Store what was found; a sensor that was not found keeps its previous setting.
  prefs.begin(NVS_NAMESPACE, false);
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    if (!pairRes[i].found) continue;
    String key = String("a_") + SENSORS[i].name;
    prefs.putString(key.c_str(), pairRes[i].addr.c_str());
    LOG("[PAIR] stored %s = %s\n", SENSORS[i].name, pairRes[i].addr.c_str());
  }
  prefs.end();

  for (uint8_t i = 0; i < 4; i++) ledWrite(LED_PINS[i], false);   // all LEDs off before normal start
  LOG("[PAIR] done, starting normal operation\n");
}

// ============================================================================
// Wi-Fi credentials + Wi-Fi setup mode (own access point with a captive web page)
// ============================================================================
// Setup mode is started at power-on when (a) no network is stored yet, or (b) the AP button is
// held. The bridge then does nothing else (no BLE, no DirCon, no pairing): it opens an access point
// "<DEVICE_NAME_PREFIX>-Setup-xxxx" (open, no password) and a web page on http://192.168.4.1/ (a phone
// normally opens it by itself as a "sign in" page). Saving restarts the bridge into normal operation.
static const char* SETTINGS_NVS_NAMESPACE = "settings";   // sensor layout, device name, network mode, maintenance flag
static const char* WIFI_NVS_NAMESPACE = "wifi";   // own namespace: pairing wipes only the "dircon" one
static String wifiSsid, wifiPass;

// True when a network name is stored (the password may be empty: open network).
static bool loadWifiCreds() {
  Preferences wp;
  wp.begin(WIFI_NVS_NAMESPACE, false);
  wifiSsid = wp.getString("ssid", "");
  wifiPass = wp.getString("pass", "");
  wp.end();
  return wifiSsid.length() > 0;
}

static void saveWifiCreds(const String& ssid, const String& pass) {
  Preferences wp;
  wp.begin(WIFI_NVS_NAMESPACE, false);
  wp.putString("ssid", ssid);
  wp.putString("pass", pass);
  wp.end();
}

struct WifiNet { String ssid; int rssi; bool secure; };
static std::vector<WifiNet> wifiNets;

// Wi-Fi LED blinks as long as the setup mode is active (a network must be entered).
static void blinkWifiLed() { ledWrite(LED_PINS[LED_WIFI], ((millis() / 500) & 1) != 0); }

// Takes the result of a finished scan (n networks, or a negative value if it failed): strongest first,
// one entry per name. The ESP32-S3 has a 2.4 GHz radio only, so the list contains no 5 GHz networks.
static void collectWifiScan(int n) {
  wifiNets.clear();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;                           // hidden network: use "Other network"
    int  rssi = WiFi.RSSI(i);
    bool sec  = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    bool dup = false;
    for (auto& w : wifiNets) if (w.ssid == s) { dup = true; if (rssi > w.rssi) { w.rssi = rssi; w.secure = sec; } }
    if (!dup && wifiNets.size() < 25) wifiNets.push_back({s, rssi, sec});
  }
  WiFi.scanDelete();
  std::sort(wifiNets.begin(), wifiNets.end(), [](const WifiNet& a, const WifiNet& b) { return a.rssi > b.rssi; });
}

// Blocking scan (used by the refresh button).
static void scanWifiNetworks() { collectWifiScan(WiFi.scanNetworks()); }

static String htmlEsc(const String& in) {
  String o;
  o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '"':  o += "&quot;"; break;
      case '\'': o += "&#39;";  break;
      default:   o += c;
    }
  }
  return o;
}

static const char PAGE_HEAD[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>BLE - DirCon Bridge</title><style>
body{margin:0;font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#f2f4f7;color:#1b1f24}
header{background:#12355b;color:#fff;padding:18px 16px}
header h1{margin:0;font-size:19px;line-height:1.3}
header p{margin:4px 0 0;font-size:16px;opacity:.9}
main{padding:16px;max-width:480px;margin:0 auto}
.note{background:#fff4d6;border-left:4px solid #e0a800;padding:10px 12px;border-radius:4px;font-size:14px;margin-bottom:12px}
.err{background:#fde2e1;border-left:4px solid #c0392b;padding:10px 12px;border-radius:4px;font-size:14px;margin-bottom:12px}
label{display:block;margin:16px 0 6px;font-weight:600;font-size:14px}
label.chk{font-weight:400;margin:10px 0 0}
select,input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:12px;font-size:16px;border:1px solid #b8c0cc;border-radius:6px;background:#fff}
.btn{display:block;width:100%;box-sizing:border-box;text-align:center;padding:14px;font-size:16px;border-radius:6px;border:0;margin-top:16px;text-decoration:none;cursor:pointer}
.sec{background:#e3e8ef;color:#12355b}
.pri{background:#12355b;color:#fff;font-weight:600}
.go{background:#1e8449;color:#fff;font-weight:600}
.btn:disabled{opacity:.55;cursor:not-allowed}
.card{background:#dfe4ea;border-radius:10px;padding:2px 14px 16px;margin:18px 0}
.card h2{margin:14px 0 4px}
.card .sec{background:#fff}
.card td{border-bottom-color:#c3cbd6}
.card hr{border:0;border-top:1px solid #c3cbd6;margin:20px 0 0}
.sub{font-weight:600;font-size:15px;margin:14px 0 4px}
table{width:100%;border-collapse:collapse;font-size:15px}
td{padding:7px 0;border-bottom:1px solid #dde2e9;vertical-align:top}
td+td{text-align:right;padding-left:12px}
h2{font-size:16px;margin:22px 0 4px}
.ok{color:#1e8449;font-weight:600}
.no{color:#6b7684}
.bg{color:#1e8449;font-weight:600}
.bm{color:#d35400;font-weight:600}
.br{color:#c0392b;font-weight:600}
.dng{background:#c0392b;color:#fff;font-weight:600}
.hint{font-size:13px;color:#6b7684;margin:4px 0 0}
footer{text-align:center;color:#6b7684;font-size:13px;padding:8px 16px 24px}
footer .mf{font-size:15px;font-weight:600;color:#4a5563;margin-bottom:2px}
</style></head><body>)HTML";

static String pageHeader(const char* sub = "Wi-Fi setup") {
  String p;
  p.reserve(2600);
  p += PAGE_HEAD;
  p += "<header><h1>BLE - DirCon Bridge</h1><p>";
  p += sub;
  p += "</p></header><main>";
  return p;
}

static String pageFooter() {
  return String("<footer><div class=mf>") + htmlEsc(MANUFACTURER_NAME) + "</div><div>Firmware " +
         htmlEsc(FIRMWARE_VERSION) + "</div></footer>";
}

static String wifiPage(const String& error) {
  String p = pageHeader();
  p += "<div class=note>This device can only connect to <b>2.4&nbsp;GHz</b> Wi-Fi networks.</div>";
  if (error.length()) { p += "<div class=err>"; p += htmlEsc(error); p += "</div>"; }
  p += "<form method=POST action=/save><label for=s>Network</label><select id=s name=ssid onchange=t()>";
  for (auto& w : wifiNets) {
    String e = htmlEsc(w.ssid);
    p += "<option value=\""; p += e; p += "\">"; p += e;
    p += w.secure ? " (" : " (open, ";
    p += w.rssi; p += " dBm)</option>";
  }
  p += "<option value=__manual__>Other network (type the name)...</option></select>";
  p += "<div id=m><label for=n>Network name</label><input id=n type=text name=ssid_manual maxlength=32 "
       "autocapitalize=off autocorrect=off spellcheck=false></div>";
  p += "<label for=p>Password</label><input id=p type=password name=pass maxlength=63 autocomplete=off>";
  p += "<label class=chk><input type=checkbox onchange=\"document.getElementById('p').type=this.checked?'text':'password'\"> "
       "Show password</label>";
  p += "<a class='btn sec' href=/scan>Refresh network list</a>";
  p += "<button class='btn pri' type=submit>Save and start</button></form></main>";
  p += pageFooter();
  p += "<script>var s=document.getElementById('s'),m=document.getElementById('m');"
       "function t(){m.style.display=s.value=='__manual__'?'block':'none'}t();</script></body></html>";
  return p;
}

static WebServer* webServer = nullptr;
static DNSServer* dnsServer = nullptr;

static void sendPage(const String& error) {
  webServer->sendHeader("Cache-Control", "no-store");
  webServer->send(200, "text/html; charset=utf-8", wifiPage(error));
}

static void redirectToRoot() {                    // also answers the phones' "is there internet?" probes
  webServer->sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  webServer->send(302, "text/plain", "");
}

static void handleScan() {
  scanWifiNetworks();
  redirectToRoot();
}

static void handleSave() {
  String ssid = webServer->arg("ssid");
  if (ssid == "__manual__") { ssid = webServer->arg("ssid_manual"); ssid.trim(); }
  String pass = webServer->arg("pass");
  if (!ssid.length())                         { sendPage("Choose a network or type its name."); return; }
  if (ssid.length() > 32)                     { sendPage("The network name is too long (at most 32 characters)."); return; }
  if (pass.length() > 63)                     { sendPage("The password is too long (at most 63 characters)."); return; }
  if (pass.length() > 0 && pass.length() < 8) { sendPage("A Wi-Fi password has at least 8 characters (leave it empty for an open network)."); return; }

  saveWifiCreds(ssid, pass);
  {                                               // a new network usually has other addresses: back to DHCP
    Preferences sp;                               // (also the way out of a wrong fixed address)
    sp.begin(SETTINGS_NVS_NAMESPACE, false);
    sp.putBool("dhcp", true);
    sp.end();
  }
  LOG("[WiFiSetup] saved network '%s', restarting\n", ssid.c_str());

  String p = pageHeader();
  p += "<div class=note><b>Saved.</b> The bridge restarts now and connects to <b>";
  p += htmlEsc(ssid);
  p += "</b>. Reconnect your phone to your normal Wi-Fi network; you can close this page.</div></main>";
  p += pageFooter();
  p += "</body></html>";
  webServer->sendHeader("Cache-Control", "no-store");
  webServer->send(200, "text/html; charset=utf-8", p);
  delay(1500);                                    // let the page reach the phone
  ESP.restart();
}

// Never returns: ends with a restart after the settings were saved.
static void runWifiSetup() {
  LOG("[WiFiSetup] starting\n");
  for (uint8_t i = 0; i < 4; i++) ledWrite(LED_PINS[i], false);

  WiFi.mode(WIFI_STA);                            // scan first, before the access point is up
  WiFi.disconnect();
  delay(100);
  WiFi.scanNetworks(true);                        // asynchronous, so the Wi-Fi LED blinks from the start
  int found;
  while ((found = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) { blinkWifiLed(); delay(20); }
  collectWifiScan(found);

  WiFi.mode(WIFI_AP_STA);                         // STA stays on so the list can be refreshed
  String apName = String(DEVICE_NAME_PREFIX) + "-Setup-" + macSuffix();
  WiFi.softAP(apName.c_str());
  delay(200);
  LOG("[WiFiSetup] access point '%s' on %s\n", apName.c_str(), WiFi.softAPIP().toString().c_str());

  dnsServer = new DNSServer();
  dnsServer->start(53, "*", WiFi.softAPIP());     // every name resolves to the bridge: captive portal
  webServer = new WebServer(80);
  webServer->on("/", HTTP_GET, []() { sendPage(""); });
  webServer->on("/scan", HTTP_GET, handleScan);
  webServer->on("/save", HTTP_POST, handleSave);
  webServer->onNotFound(redirectToRoot);
  webServer->begin();

  for (;;) {
    dnsServer->processNextRequest();
    webServer->handleClient();
    blinkWifiLed();                               // Wi-Fi LED blinks: setup mode, a network must be entered
    delay(2);
  }
}

// ============================================================================
// Settings (NVS), status page and maintenance mode
// ============================================================================
static const uint32_t MAINT_IDLE_MS = 15UL * 60UL * 1000UL;   // maintenance mode ends itself after this idle time

static void loadSettings() {
  Preferences sp;
  sp.begin(SETTINGS_NVS_NAMESPACE, false);
  cfgGroup = sp.getUChar("group", SENSOR_GROUP);
  if (cfgGroup > 1) cfgGroup = SENSOR_GROUP;
  cfgName  = sp.getString("name", "");
  cfgMaxClients = sp.getUChar("maxcl", MAX_CLIENTS);
  if (cfgMaxClients < 2 || cfgMaxClients > MAX_CLIENTS_LIMIT) cfgMaxClients = MAX_CLIENTS;
  cfgDhcp  = sp.getBool("dhcp", true);
  if (!cfgIp.fromString(sp.getString("ip", ""))     || !cfgMask.fromString(sp.getString("mask", ""))) cfgDhcp = true;
  if (!cfgGw.fromString(sp.getString("gw", "")))    cfgGw = IPAddress(0, 0, 0, 0);
  if (!cfgDns.fromString(sp.getString("dns", ""))) cfgDns = IPAddress(0, 0, 0, 0);
  sp.end();
  for (uint8_t i = 0; i < NUM_SENSORS; i++)
    if (SENSORS[i].slot == 1) SENSORS[i].group = cfgGroup;      // HR (slot 0) always stays in group 0
}

static void saveSettings(uint8_t group, const String& name) {
  Preferences sp;
  sp.begin(SETTINGS_NVS_NAMESPACE, false);
  sp.putUChar("group", group);
  sp.putString("name", name);
  sp.putUChar("maxcl", cfgMaxClients);
  sp.putBool("dhcp", cfgDhcp);
  sp.putString("ip", cfgIp.toString());
  sp.putString("mask", cfgMask.toString());
  sp.putString("gw", cfgGw.toString());
  sp.putString("dns", cfgDns.toString());
  sp.end();
}

static uint32_t ipHost(const IPAddress& a) { return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3]; }

// Checks a fixed address; returns "" when fine, else the reason.
static String checkFixedIp(const IPAddress& ip, const IPAddress& mask, const IPAddress& gw, bool haveGw) {
  uint32_t i = ipHost(ip), m = ipHost(mask), hostBits = ~m;
  if (m == 0 || (hostBits & (hostBits + 1)) != 0 || hostBits < 3) return "the subnet mask is not valid (normally 255.255.255.0).";
  if (ip[0] == 0 || ip[0] == 127 || ip[0] >= 224)               return "the IP address is not a usable device address.";
  if ((i & hostBits) == 0 || (i & hostBits) == hostBits)        return "the IP address is the network or broadcast address of the subnet.";
  if (haveGw) {
    uint32_t g = ipHost(gw);
    if ((g & m) != (i & m))                                     return "the gateway is not in the same subnet as the IP address.";
    if (g == i)                                                 return "the gateway equals the IP address.";
  }
  return "";
}

static void setMaintFlag() {
  Preferences sp;
  sp.begin(SETTINGS_NVS_NAMESPACE, false);
  sp.putBool("maint", true);
  sp.end();
}

// True once after the status page asked for maintenance mode; the flag is cleared at once, so the
// next restart is a normal one.
static bool takeMaintFlag() {
  Preferences sp;
  sp.begin(SETTINGS_NVS_NAMESPACE, false);
  bool m = sp.getBool("maint", false);
  if (m) sp.remove("maint");
  sp.end();
  return m;
}

static void eraseNamespace(const char* ns) {
  Preferences p;
  p.begin(ns, false);
  p.clear();
  p.end();
}

// Wi-Fi network + paired sensors + settings. Afterwards the bridge starts the Wi-Fi setup again.
static void eraseAllSettings() {
  eraseNamespace(WIFI_NVS_NAMESPACE);
  eraseNamespace(NVS_NAMESPACE);
  eraseNamespace(SETTINGS_NVS_NAMESPACE);
}

// Blocks until connected (maxMs = 0: for ever, retrying); false when maxMs ran out.
static bool connectWifi(uint32_t maxMs) {
  WiFi.mode(WIFI_STA);
  if (!cfgDhcp) {                                         // fixed address; must be set before begin()
    if (!(cfgDns == IPAddress(0, 0, 0, 0))) WiFi.config(cfgIp, cfgGw, cfgMask, cfgDns);
    else                                 WiFi.config(cfgIp, cfgGw, cfgMask);
  }
  WiFi.setSleep(false);               // lower, steadier latency
  WiFi.setAutoReconnect(true);
  const char* wifiPassC = wifiPass.length() ? wifiPass.c_str() : nullptr;   // empty = open network
  WiFi.begin(wifiSsid.c_str(), wifiPassC);
  LOG("[WiFi] connecting to '%s'", wifiSsid.c_str());
  uint32_t tAll = millis(), t0 = tAll;
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    LOG(".");
    if (maxMs && millis() - tAll > maxMs) return false;
    if (millis() - t0 > 20000) { WiFi.disconnect(); WiFi.begin(wifiSsid.c_str(), wifiPassC); t0 = millis(); }
  }
  LOG("\n[WiFi] IP %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// mDNS host name: "dircon-bridge-<device name>" when a device name is set (lower case, anything but
// letters and digits becomes "-"), else "dircon-bridge-<MAC suffix>" (or plain "dircon-bridge").
static void beginMdnsHost() {
  String suffix;
  for (size_t i = 0; i < cfgName.length(); i++) {
    char c = cfgName[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (ok) suffix += c;
    else if (suffix.length() && suffix[suffix.length() - 1] != '-') suffix += '-';
  }
  while (suffix.length() && suffix[suffix.length() - 1] == '-') suffix.remove(suffix.length() - 1);
#if USE_MAC_SUFFIX
  if (!suffix.length()) { suffix = macSuffix(); suffix.toLowerCase(); }
#endif
  String host = MDNS_HOSTNAME;
  if (suffix.length()) host += "-" + suffix;
  MDNS.begin(host.c_str());
  LOG("[mDNS] host %s.local\n", host.c_str());
}

static void sendHtml(const String& body) {
  webServer->sendHeader("Cache-Control", "no-store");
  webServer->send(200, "text/html; charset=utf-8", body);
}

static void sendRedirectHome() {
  webServer->sendHeader("Location", "/", true);
  webServer->send(303, "text/plain", "");
}

// Page shown just before a restart; reloads itself after the bridge is back.
static void sendRestartPage(const String& message, const char* sub) {
  String p = pageHeader(sub);
  p += "<div class=note>" + message + "</div>";
  p += "<script>setTimeout(function(){location.href='/'},9000)</script></main>";
  p += pageFooter();
  p += "</body></html>";
  sendHtml(p);
}

// ---- passive status page (normal operation) ----------------------------------------------------
static String statusRow(const String& label, const String& value, const char* cls = "") {
  return "<tr><td>" + label + "</td><td class=\"" + cls + "\">" + value + "</td></tr>";
}

// One line per slot: the connected sensor of that slot, or "searching".
// The battery level follows the sensor name: green > 50 %, orange 25..50 %, red < 25 %.
static String slotStatus(uint8_t slot, const char* label) {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    if (SENSORS[i].slot != slot || !rt[i].connected) continue;
    String v = String(SENSORS[i].name) + ": " + htmlEsc(devNameC[i]);
    int b = batteryPercent(i);
    if (b >= 0) {
      const char* bc = b > 50 ? "bg" : (b >= 25 ? "bm" : "br");
      // Small battery icon (inline SVG, takes the text colour; fill width follows the level).
      v += String(" <span class=") + bc + ">(<svg width=18 height=10 viewBox='0 0 18 10' "
           "style='vertical-align:-1px'><rect x=.5 y=.5 width=14 height=9 rx=1 fill=none stroke=currentColor />"
           "<rect x=15 y=3 width=2 height=4 fill=currentColor /><rect x=2 y=2 width=" + String(b * 11 / 100.0f, 1) +
           " height=6 fill=currentColor /></svg> " + b + "%)</span>";
    }
    return statusRow(label, v, "ok");
  }
  return statusRow(label, "searching", "no");
}

// Line below a sensor: app connections to the DirCon device that serves this slot, actual/maximum.
static String slotConns(uint8_t slot) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    if (SENSORS[i].slot != slot) continue;
    uint8_t g = SENSORS[i].group;
    if (g < numGroups && groups[g]) n = groups[g]->clientCount();
    break;
  }
  return statusRow("&nbsp;&nbsp;App connections", String(n) + "/" + String(cfgMaxClients), n ? "ok" : "no");
}

static String statusPage() {
  uint32_t up = millis() / 1000;
  char ups[24];
  snprintf(ups, sizeof(ups), "%ud %02u:%02u:%02u", (unsigned)(up / 86400), (unsigned)(up / 3600 % 24),
           (unsigned)(up / 60 % 60), (unsigned)(up % 60));

  String p = pageHeader("Status");
  p += "<table>";
  p += statusRow("Wi-Fi", htmlEsc(wifiSsid) + " (" + String(WiFi.RSSI()) + " dBm)");
  p += statusRow("IP address", WiFi.localIP().toString() + (cfgDhcp ? " (DHCP)" : " (fixed)"));
  p += slotStatus(0, "Heart rate");
  p += slotConns(0);
  p += slotStatus(1, "Power / cadence");
  p += slotConns(1);
  p += statusRow("Layout", cfgGroup ? "Separate devices" : "Combined device");
  p += statusRow("Uptime", ups);
  p += "</table>";
  p += "<h2>Maintenance</h2><p class=hint>Stops the bridging. Then the settings can be changed. "
       "Restarting the bridge brings it back to normal operation.</p>";
  p += "<form method=post action=/maint onsubmit=\"return confirm('Stop bridging and enter maintenance mode?')\">"
       "<button class=\"btn sec\" type=submit>Enter maintenance mode</button></form>";
  p += "<script>setTimeout(function(){location.reload()},5000)</script></main>";
  p += pageFooter();
  p += "</body></html>";
  return p;
}

static void handleEnterMaint() {
  setMaintFlag();
  LOG("[WEB] maintenance mode requested, restarting\n");
  sendRestartPage("<b>Entering maintenance mode.</b> The bridge restarts; this page reloads by itself.", "Status");
  delay(1000);
  ESP.restart();
}

static void statusTask(void*) {
  for (;;) {
    webServer->handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Own task, so a slow or stalled web client can never delay loop() (DirCon forwarding).
static void startStatusServer() {
  webServer = new WebServer(80);
  webServer->on("/", HTTP_GET, []() { sendHtml(statusPage()); });
  webServer->on("/maint", HTTP_POST, handleEnterMaint);
  webServer->onNotFound([]() { sendRedirectHome(); });
  webServer->begin();
  xTaskCreate(statusTask, "status", 6144, nullptr, 1, nullptr);
}

// ---- maintenance mode -------------------------------------------------------------------------
static String   maintMsg;                 // one-time message shown on the next page
static uint32_t maintLast = 0;

// Stored (paired) address of one sensor row, "" when not paired.
static String storedAddr(uint8_t i) {
  Preferences pp;
  pp.begin(NVS_NAMESPACE, false);
  String a = pp.getString((String("a_") + SENSORS[i].name).c_str(), "");
  pp.end();
  return a;
}

static const char* const SLOT_LABEL[NUM_SLOTS] = { "Heart rate sensor", "Power meter / cadence sensor" };

// Per slot: what is connected right now, whether it is paired, and a Pair / Unpair button.
// (Maintenance mode keeps the BLE side running so the sensors can be seen; nothing is forwarded.)
static String sensorsSection() {
  String p = "<h2>Sensors</h2>";
  for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
    int con = -1, par = -1;
    String addr;
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      if (SENSORS[i].slot != sl) continue;
      if (rt[i].connected) con = i;
      String a = storedAddr(i);
      if (a.length() && par < 0) { par = i; addr = a; }
    }
    p += String("<p><b>") + SLOT_LABEL[sl] + "</b></p><table>";
    if (con >= 0)
      p += statusRow("Connected", String(SENSORS[con].name) + ": " + htmlEsc(devNameC[con]), "ok");
    else
      p += statusRow("Connected", "searching", "no");
    p += statusRow("Paired", par >= 0 ? htmlEsc(addr) : String("no"), par >= 0 ? "ok" : "no");
    p += "</table>";
    if (par >= 0)
      p += String("<form method=post action=/pair onsubmit=\"return confirm('Unpair this sensor?')\">"
                  "<input type=hidden name=slot value=") + sl + "><input type=hidden name=act value=unpair>"
           "<button class=\"btn sec\" type=submit>Unpair</button></form>";
    else if (con >= 0)
      p += String("<form method=post action=/pair><input type=hidden name=slot value=") + sl +
           "><input type=hidden name=act value=pair><button class=\"btn sec\" type=submit>Pair this sensor</button></form>";
  }
  p += "<p class=hint>Paired: the bridge only connects to that sensor. Not paired: it takes the first one it finds. "
       "Changes are used after you leave the maintenance mode.</p><a class=\"btn sec\" href=\"/\">Refresh</a>";
  return p;
}

static String maintPage() {
  String p = pageHeader("Maintenance mode");
  p += "<div class=note>Bridging is stopped. Leave the maintenance mode when you are done.</div>";
  if (maintMsg.length()) { p += "<div class=note><b>" + maintMsg + "</b></div>"; maintMsg = ""; }

  // ---- DirCon setup
  p += "<div class=card><h2>DirCon setup</h2><form method=post action=/dircon>";
  p += "<label>Sensor layout</label><select name=group>";
  p += String("<option value=1") + (cfgGroup ? " selected" : "") + ">Separate devices (HR and cadence/power)</option>";
  p += String("<option value=0") + (cfgGroup ? "" : " selected") + ">One combined device (e.g. for Zwift)</option>";
  p += "</select><p class=hint>Combined: no battery level is shown in the app.</p>";
  p += "<label>Device name</label><input type=text name=name maxlength=20 value=\"" + htmlEsc(cfgName) + "\">";
  p += "<p class=hint>Appended to the DirCon device names and used in the web address. Empty = last 4 digits of the MAC address. "
       "Letters, digits, space, - and _ only.</p>";
  p += "<label>Maximum concurrent connections per sensor</label><select name=maxcl>";
  for (uint8_t n = 2; n <= MAX_CLIENTS_LIMIT; n++)
    p += String("<option value=") + n + (n == cfgMaxClients ? " selected" : "") + ">" + n + "</option>";
  p += "</select><p class=hint>Applies to each sensor separately: every sensor accepts this many app connections "
       "(with the combined layout, the one combined device accepts this many).</p>"
       "<div class=note style=\"margin-top:10px\"><b>Warning:</b> setting this to more than 2 can cause issues.</div>";
  p += "<button class=\"btn pri\" type=submit>Save DirCon setup</button></form></div>";

  // ---- Network setup
  p += "<div class=card><h2>Network setup</h2><form method=post action=/network>";
  p += "<label>IP address</label><select name=net>";
  p += String("<option value=dhcp") + (cfgDhcp ? " selected" : "") + ">Automatic (DHCP)</option>";
  p += String("<option value=fixed") + (cfgDhcp ? "" : " selected") + ">Fixed IP address</option></select>";
  p += "<label>Fixed IP address</label><input type=text name=ip inputmode=decimal placeholder=\"192.168.1.50\" value=\"" +
       String(cfgDhcp ? "" : htmlEsc(cfgIp.toString())) + "\">";
  p += "<label>Subnet mask</label><input type=text name=mask inputmode=decimal value=\"" +
       htmlEsc(cfgDhcp ? String("255.255.255.0") : cfgMask.toString()) + "\">";
  p += "<label>Gateway (optional)</label><input type=text name=gw inputmode=decimal placeholder=\"192.168.1.1\" value=\"" +
       String((cfgDhcp || cfgGw == IPAddress(0, 0, 0, 0)) ? "" : htmlEsc(cfgGw.toString())) + "\">";
  p += "<label>DNS server (optional)</label><input type=text name=dns inputmode=decimal placeholder=\"192.168.1.1\" value=\"" +
       String((cfgDhcp || cfgDns == IPAddress(0, 0, 0, 0)) ? "" : htmlEsc(cfgDns.toString())) + "\">";
  p += "<div class=note style=\"margin-top:10px\"><b>Fixed address:</b> choose an address <b>outside the range your router's DHCP "
       "server hands out</b> and not used by another device (see the DHCP settings of your router), in the same subnet as your network. "
       "The four fields are ignored when &quot;Automatic (DHCP)&quot; is selected. If the bridge cannot be reached afterwards, hold the "
       "Wi-Fi setup button while connecting power and save the network again: that returns the bridge to DHCP.</div>";
  p += "<button class=\"btn pri\" type=submit>Save network setup</button></form></div>";

  // ---- Sensors (unchanged content)
  p += "<div class=card>" + sensorsSection() + "</div>";

  // ---- System: firmware update + factory reset
  p += "<div class=card><h2>System</h2><div class=sub>Firmware update</div><p class=hint>Currently " FIRMWARE_VERSION ". Choose the firmware file (.bin) you received from "
       + htmlEsc(MANUFACTURER_NAME) + ". It is written next to the running firmware, so a failed or interrupted upload leaves the current firmware untouched. "
       "Keep the power connected; the bridge restarts by itself when the upload is done. Your settings are kept.</p>"
       "<form method=post action=/update enctype=multipart/form-data "
       "onsubmit=\"var b=this.querySelector('button');b.disabled=true;b.textContent='Uploading... do not disconnect power'\">"
       "<input type=file id=fw name=firmware accept=\".bin\" hidden onchange=\""
       "var f=this.files[0],b=document.getElementById('fwb');"
       "document.getElementById('fwl').textContent=f?f.name:'Choose file';"
       "b.disabled=!f;b.className=f?'btn pri':'btn sec'\">"
       "<label for=fw id=fwl class=\"btn sec\" style=\"font-weight:400\">Choose file</label>"
       "<button id=fwb class=\"btn sec\" type=submit disabled>Upload and install</button></form>"
       "<hr><div class=sub>Factory reset</div><form method=post action=/factory "
       "onsubmit=\"return confirm('Erase the Wi-Fi network, paired sensors and settings?')\">"
       "<button class=\"btn dng\" type=submit>Factory reset</button></form>"
       "<p class=hint>Erases the Wi-Fi network, the paired sensors and all settings. The bridge then "
       "starts the Wi-Fi setup.</p></div>";

  // ---- leave
  p += "<form method=post action=/restart><button class=\"btn go\" type=submit>Leave maintenance mode</button></form></main>";
  p += pageFooter();
  p += "</body></html>";
  return p;
}

static void handleDirconSave() {
  maintLast = millis();
  uint8_t g = webServer->arg("group") == "0" ? 0 : 1;
  String raw = webServer->arg("name"), name;
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' || c == '-' || c == '_')
      name += c;
  }
  name.trim();
  if (name.length() > 20) name = name.substring(0, 20);
  int mc = webServer->arg("maxcl").toInt();
  if (mc < 2 || mc > MAX_CLIENTS_LIMIT) mc = cfgMaxClients;
  cfgGroup = g;
  cfgName = name;
  cfgMaxClients = (uint8_t)mc;
  saveSettings(g, name);                          // the network values in cfg* are saved unchanged
  maintMsg = "DirCon setup saved. It is used after you leave the maintenance mode.";
  sendRedirectHome();
}

static void handleNetworkSave() {
  maintLast = millis();
  bool dhcp = webServer->arg("net") != "fixed";
  IPAddress ip, mask, gw(0, 0, 0, 0);
  if (!dhcp) {
    String sIp = webServer->arg("ip"), sMask = webServer->arg("mask"), sGw = webServer->arg("gw"), sDns = webServer->arg("dns");
    sIp.trim(); sMask.trim(); sGw.trim(); sDns.trim();
    IPAddress dns(0, 0, 0, 0);
    bool haveGw = sGw.length() > 0;
    String err;
    if (!ip.fromString(sIp))                       err = "the IP address is not valid (four numbers, for example 192.168.1.50).";
    else if (!mask.fromString(sMask))              err = "the subnet mask is not valid (for example 255.255.255.0).";
    else if (haveGw && !gw.fromString(sGw))        err = "the gateway is not valid (for example 192.168.1.1).";
    else if (sDns.length() && !dns.fromString(sDns)) err = "the DNS server is not valid (for example 192.168.1.1).";
    else                                           err = checkFixedIp(ip, mask, gw, haveGw);
    if (err.length()) { maintMsg = "Network setup not saved: " + err; sendRedirectHome(); return; }
    cfgIp = ip; cfgMask = mask; cfgGw = gw; cfgDns = dns;
  }
  cfgDhcp = dhcp;
  saveSettings(cfgGroup, cfgName);                // the DirCon values in cfg* are saved unchanged
  maintMsg = "Network setup saved. It is used after you leave the maintenance mode.";
  sendRedirectHome();
}

static void handlePair() {
  maintLast = millis();
  int sl = webServer->arg("slot").toInt();
  bool pair = webServer->arg("act") == "pair";
  if (sl < 0 || sl >= NUM_SLOTS) { sendRedirectHome(); return; }
  Preferences pp;
  pp.begin(NVS_NAMESPACE, false);
  for (uint8_t i = 0; i < NUM_SENSORS; i++)                       // the slot holds one paired sensor at most
    if (SENSORS[i].slot == sl) pp.remove((String("a_") + SENSORS[i].name).c_str());
  maintMsg = "Sensor unpaired.";
  if (pair) {
    maintMsg = "Nothing connected to pair.";
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      if (SENSORS[i].slot != sl || !rt[i].connected) continue;
      String addr = rt[i].foundAddr.c_str();
      pp.putString((String("a_") + SENSORS[i].name).c_str(), addr);
      maintMsg = "Sensor paired.";
      break;
    }
  }
  pp.end();
  sendRedirectHome();
}

// ---- firmware upload ---------------------------------------------------------------------------
static String updateError;
static bool   updateStarted = false;
static bool   updateDone = false;

// Called by WebServer for every chunk of the uploaded file.
static void handleFirmwareUpload() {
  maintLast = millis();
  HTTPUpload& up = webServer->upload();
  if (up.status == UPLOAD_FILE_START) {
    updateError = ""; updateDone = false; updateStarted = false;
    LOG("[OTA] receiving %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) updateError = Update.errorString();
    else updateStarted = true;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (updateStarted && Update.write(up.buf, up.currentSize) != up.currentSize) {
      updateError = Update.errorString();
      Update.abort();
      updateStarted = false;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (updateStarted) {
      if (Update.end(true)) updateDone = true;        // true: also accepts a size that was not known in advance
      else updateError = Update.errorString();
      updateStarted = false;
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (updateStarted) Update.abort();
    updateStarted = false;
    updateError = "the upload was interrupted";
  }
}

static void handleFirmwareDone() {
  maintLast = millis();
  if (!updateDone) {
    String p = pageHeader("Maintenance mode");
    p += "<div class=err><b>Firmware update failed:</b> " +
         htmlEsc(updateError.length() ? updateError : String("no file received")) +
         ".<br>The current firmware is unchanged. Check that you chose the firmware .bin you "
         "received for this bridge, then try again.</div>"
         "<a class=\"btn sec\" href=\"/\">Back</a></main>";
    p += pageFooter();
    p += "</body></html>";
    sendHtml(p);
    return;
  }
  LOG("[OTA] done, restarting\n");
  sendRestartPage("<b>Firmware installed.</b> The bridge restarts with the new firmware; this page reloads by itself.", "Maintenance mode");
  delay(1000);
  ESP.restart();
}

static void handleFactory() {
  eraseAllSettings();
  LOG("[WEB] factory reset, restarting\n");
  sendRestartPage("<b>Factory reset done.</b> The bridge restarts into Wi-Fi setup: connect your phone to the "
                  "network <b>" + htmlEsc(String(DEVICE_NAME_PREFIX) + "-Setup-" + macSuffix()) + "</b>. "
                  "If the sign-in page does not appear, open <b>http://192.168.4.1</b> in your browser.", "Maintenance mode");
  delay(1000);
  ESP.restart();
}

static void handleRestart() {
  sendRestartPage("<b>Restarting</b> into normal operation.", "Maintenance mode");
  delay(1000);
  ESP.restart();
}

// Never returns. No BLE, no DirCon: only Wi-Fi + this page. Ends with a restart.
static void runMaintenance() {
  LOG("[MAINT] starting\n");
  if (!connectWifi(60000)) ESP.restart();          // network gone: back to normal operation
  beginMdnsHost();
  ledWrite(LED_PINS[LED_WIFI], true);
  bleBegin();                                      // only to show / pair the sensors; nothing is forwarded
  webServer = new WebServer(80);
  webServer->on("/", HTTP_GET, []() { maintLast = millis(); sendHtml(maintPage()); });
  webServer->on("/dircon", HTTP_POST, handleDirconSave);
  webServer->on("/network", HTTP_POST, handleNetworkSave);
  webServer->on("/pair", HTTP_POST, handlePair);
  webServer->on("/factory", HTTP_POST, handleFactory);
  webServer->on("/update", HTTP_POST, handleFirmwareDone, handleFirmwareUpload);
  webServer->on("/restart", HTTP_POST, handleRestart);
  webServer->onNotFound([]() { sendRedirectHome(); });
  webServer->begin();
  maintLast = millis();
  for (;;) {
    webServer->handleClient();
    if (millis() - maintLast > MAINT_IDLE_MS) ESP.restart();
    delay(2);
  }
}

// ============================================================================
// Factory reset by both buttons at power-on
// ============================================================================
// Called when both buttons were held at power-on (after the LED self-test). Sequence, all with both
// buttons kept pressed:
//   1. 10 s wait (LEDs off)
//   2. all 4 LEDs on
//   3. every 3 s one LED goes off, from right to left on the front: DirCon, Cadence/Power, HR, Wi-Fi
//   4. all 4 LEDs flash, then true is returned (the caller erases the settings and restarts)
// Releasing either button at any moment aborts: all LEDs off, false is returned, normal start-up goes on.
constexpr uint32_t FACTORY_WAIT_MS = 10000;
constexpr uint32_t FACTORY_STEP_MS = 3000;

static bool bothButtonsHeldFor(uint32_t ms) {
  uint32_t t0 = millis(), lastLow = t0;
  while (millis() - t0 < ms) {
    // a button counts as released only after it read "not pressed" for the debounce time
    if (digitalRead(PAIR_BUTTON_GPIO) == LOW && digitalRead(WIFI_AP_BUTTON_GPIO) == LOW) lastLow = millis();
    if (millis() - lastLow >= PAIR_DEBOUNCE_MS) return false;
    delay(1);
  }
  return true;
}

static void allLeds(bool on) { for (int8_t pin : LED_PINS) if (pin >= 0) ledWrite(pin, on); }

static bool factoryResetGesture() {
  LOG("[RESET] both buttons held: factory reset in %lu s unless released\n", (unsigned long)(FACTORY_WAIT_MS / 1000));
  allLeds(false);
  if (!bothButtonsHeldFor(FACTORY_WAIT_MS)) { LOG("[RESET] aborted\n"); return false; }
  allLeds(true);
  const uint8_t rightToLeft[4] = { LED_DIRCON, LED_BLE_CAD, LED_BLE_HR, LED_WIFI };   // front: Wi-Fi, HR, Cad/Pwr, DirCon
  for (uint8_t k = 0; k < 4; k++) {
    if (!bothButtonsHeldFor(FACTORY_STEP_MS)) { allLeds(false); LOG("[RESET] aborted\n"); return false; }
    ledWrite(LED_PINS[rightToLeft[k]], false);
  }
  for (uint8_t n = 0; n < 3; n++) { allLeds(true); delay(150); allLeds(false); delay(150); }
  LOG("[RESET] erasing all settings\n");
  return true;
}

// ============================================================================
// Arduino entry points
// ============================================================================
void setup() {
#if DEBUG_SERIAL
  Serial.begin(115200);
  #if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);          // never block when no serial monitor is attached
  #endif
  delay(300);
  Serial.printf("[BOOT] firmware %s\n", FIRMWARE_VERSION);
#endif

  ledsBegin();

  // Buttons at power-on: pairing button = pairing mode (then normal operation); AP button = Wi-Fi
  // setup; both together = factory reset (10 s hold + LED countdown, release at any time to abort).
  bool pairBtn  = buttonHeldAtBoot(PAIR_BUTTON_GPIO);
  bool apBtn    = buttonHeldAtBoot(WIFI_AP_BUTTON_GPIO);
  bool haveWifi = loadWifiCreds();
  bool maint    = takeMaintFlag();    // set by the status page; cleared at once
  if (pairBtn && apBtn) {
    if (factoryResetGesture()) {        // completed: erase everything, restart into Wi-Fi setup
      eraseAllSettings();
      ESP.restart();
    }
    pairBtn = apBtn = false;            // aborted: carry on as if no button was pressed
  }
  if (pairBtn && !apBtn) runPairing();
  // Wi-Fi setup: asked for with the AP button, or no network stored yet (then whatever the buttons say).
  // A stored network that cannot be reached does NOT start the setup: the bridge keeps trying it.
  if ((apBtn && !pairBtn) || !haveWifi) runWifiSetup();
  loadSettings();                     // sensor layout + device name
  loadFilters();                      // compile-time filters, overridden by paired addresses

  notifyQueue = xQueueCreate(32, sizeof(NotifyEvent));
  buildGroups();
  seedDefaults();
  if (maint) runMaintenance();        // never returns (BLE + Wi-Fi + settings page, no DirCon)

  // 1) Network first. A DirCon device is announced later (mdnsRefresh) once a sensor of it is connected.
  connectWifi(0);
  beginMdnsHost();
  startStatusServer();                // passive status page, own task
  for (uint8_t g = 0; g < numGroups; g++) if (groups[g]) groups[g]->begin();

  // 2) Then look for the sensors in the background; they are bridged as soon as they connect.
  bleBegin();
}

void loop() {
  // Wakes the moment a BLE value is queued, then forwards everything that is waiting.
  NotifyEvent e;
  if (xQueueReceive(notifyQueue, &e, pdMS_TO_TICKS(2)) == pdTRUE) {
    do {
      DirconDevice* d = groups[SENSORS[e.sensor].group];
      if (d) d->notify(e.sensor, e.svc, e.ch, e.data, e.len);
    } while (xQueueReceive(notifyQueue, &e, 0) == pdTRUE);
  }
  for (uint8_t g = 0; g < numGroups; g++) if (groups[g]) groups[g]->poll();
  mdnsRefresh();
  ledsUpdate();

#if DEBUG_SERIAL
  static uint32_t last = 0;
  if (millis() - last > 10000) {
    last = millis();
    LOG("[STAT] wifi=%d", WiFi.status() == WL_CONNECTED);
    for (uint8_t i = 0; i < NUM_SENSORS; i++) LOG(" %s=%d", SENSORS[i].name, (int)rt[i].connected);
    for (uint8_t g = 0; g < numGroups; g++) if (groups[g]) LOG(" tcp[g%u]=%d", g, groups[g]->clientCount());
    LOG("\n");
  }
#endif
}