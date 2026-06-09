# ble-midi-footswitch

An ESP32 (Wemos Lolin32) based BLE-MIDI footswitch controller (codename "three").

It is becoming a **standalone scene player**: scenes for songs are authored on a
laptop with [mcp-midi-controller](https://github.com/teemow/mcp-midi-controller)
and pushed into the device, which then replays them live (no laptop in the loop)
to the rig over BLE-MIDI and to a mixer over WiFi/OSC. See
[`docs/roadmap.md`](docs/roadmap.md) for the plan.

## Build (PlatformIO)

```bash
pio run -e lolin32 -t upload        # USB build + flash
pio device monitor -e lolin32       # serial monitor (115200)
pio run -e ota -t upload            # OTA flash (device on WiFi, three.local)
```

Toolchain is pinned in `platformio.ini` (`espressif32@6.9.0`, Arduino core
2.0.x). `BLE-MIDI` and `NimBLE-Arduino` 1.4.3 are vendored in `lib/` — see
`lib/README.md` for the (NimBLE 1.x vs 2.x) reason. Do not bump the platform to
the core-3.x line without migrating BLE-MIDI to the NimBLE 2.x API.

## WiFi / OTA

- On boot the device makes a best-effort WiFi connect with saved credentials; if
  that fails it runs **BLE-only** (the live path is never blocked).
- To set/change WiFi: **hold SW0 (bank-up) while powering on** to open the
  captive portal (AP `THREE`), then pick your network.
- OTA is `ArduinoOTA`, advertised as `three.local`, password-protected.
- The captive-portal and OTA passwords are kept out of this repo: copy
  `include/secrets.h.example` to `include/secrets.h` and set your own values
  (it is gitignored).

Material:

 * Aluminium box (1590DD)
 * Wemos Lolin32
 * Wemos LED Matrix 8x8
 * Wemos prototype shield
 * USB-B socket to micro USB
 * 2 MIDI sockets (5 pin)
 * 6 10mm LEDs
 * 6 10mm LED sockets
 * 6 momentary footswitches

## Photos

![](https://github.com/teemow/ble-midi-footswitch/blob/main/docs/photos/raw_case.jpg?raw=true)
![](https://github.com/teemow/ble-midi-footswitch/blob/main/docs/photos/raw_case_with_holes.jpg?raw=true)
![](https://github.com/teemow/ble-midi-footswitch/blob/main/docs/photos/raw_case_with_holes_back.jpg?raw=true)
![](https://github.com/teemow/ble-midi-footswitch/blob/main/docs/photos/testing_ble_with_a_switch.mp4?raw=true)
![](https://github.com/teemow/ble-midi-footswitch/blob/main/docs/photos/painted_case.jpg?raw=true)
![](https://github.com/teemow/ble-midi-footswitch/blob/main/docs/photos/fully_assembled.jpg?raw=true)
![](https://github.com/teemow/ble-midi-footswitch/blob/main/docs/photos/fully_assembled_back.jpg?raw=true)
![](https://github.com/teemow/ble-midi-footswitch/blob/main/docs/photos/testing_leds.mp4?raw=true)
![](https://github.com/teemow/ble-midi-footswitch/blob/main/docs/photos/testing_the_matrix.mp4?raw=true)

