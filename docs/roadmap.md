# "three" footswitch — firmware v2 roadmap

## Where this fits

Two tools, **disjoint roles**:

- **`mcp-midi-controller`** (runs on a laptop, design-time only): verify the full
  MIDI capability of every device in the rig and **author scenes for songs**.
  Then **compile and push those scenes into this footswitch**. Never in the live
  signal path.
- **This footswitch** (live): a **standalone scene player**. Stores scenes on
  device; a footswitch press replays a song's scene to the rig.

Live topology (no laptop, no mcp server): everything is connected to the
**iPad**. The footswitch emits the scene's MIDI (the iPad / AUM fans it out to
the pedals + plugins), and talks to the **X32 directly over WiFi/OSC**.

## Phases

### Phase 0 — Foundations (DONE)

- Migrated to **PlatformIO** (`platformio.ini`, sources in `src/`).
- Pinned the toolchain: `platform = espressif32@6.9.0` (Arduino core 2.0.x /
  ESP-IDF 4.4) because the vendored NimBLE 1.4.3 + BLE-MIDI backend need the
  NimBLE 1.x API. **Do not** move to the pioarduino 5x platform (core 3.x) until
  BLE-MIDI is migrated to NimBLE 2.x.
- Vendored `BLE-MIDI` and `NimBLE-Arduino` 1.4.3 into `lib/` (see `lib/README.md`
  for why — PlatformIO otherwise pulls NimBLE 2.x transitively and the link
  fails). Other libs (MIDI, Adafruit GFX, WEMOS Matrix, WiFiManager) are pinned
  in `lib_deps`.
- Added **WiFi + OTA** that coexist with BLE-MIDI:
  - Best-effort STA connect on boot using saved credentials (8 s timeout); if it
    fails, the device continues **BLE-only** (the live path is never blocked).
  - **Captive portal** (AP `THREE` / `THR33!!!`, via WiFiManager) only when
    **SW0 (bank-up, GPIO16) is held at boot** — so it never hangs in AP mode
    during a gig.
  - **ArduinoOTA** (mDNS `three.local`, password-protected). Build/flash OTA with
    `pio run -e ota -t upload`.
- **Verified end-to-end on hardware (2026-06-02):** USB flash + boot, BLE-MIDI
  advertising as `ThreeFoot` (`30:AE:A4:05:B4:AE`), captive-portal WiFi
  provisioning, auto-reconnect with saved creds, `three.local` mDNS, and an
  **OTA flash + reboot + rejoin**. RAM ~18%, Flash ~89% of the default app slot.
- **Legacy behaviour re-verified over a real BLE peer (2026-06-02):** the
  PlatformIO + WiFi/OTA migration preserved the original banked-CC sender and
  visuals. Bonded the device from a Linux laptop (BlueZ central → WirePlumber
  auto-bridges it into the ALSA sequencer as `ThreeFoot Bluetooth`), then
  `aseqdump`'d the port while pressing switches:
  - SW2–5 on bank 1 → CC 60/61/62/63; SW2 on bank 2 → CC 64. Matches
    `58 + i + (bank-1)*4`, channel 1, value 127, momentary (no value-0).
  - SW0/SW1 (bank up/down) emit no MIDI — bank navigation only.
  - Matrix: **smile** on BLE connect, **frown** on disconnect, bank digit
    tracks 1↔2. Each action switch lights its own LED on press.
  - Verification path (no custom tooling): `bluetoothctl connect <mac>` →
    `aseqdump -p <client>:0`. Same ALSA-seq data plane the mcp server uses.
- **OTA on this (multi-homed) dev host** needs two espota flags (already in the
  `ota` env): `--host_ip=192.168.2.75` (so the ESP32 connects back to the right
  interface, not WireGuard/docker) and `--host_port=3233` (pinned so the host
  firewall can scope an allow rule). The matching nftables rule lives in
  `demiurg/system/etc/nftables.conf` (TCP/3233 from `@trusted_v4`).

**Carry-over note:** flash is ~89% full on the default partition. Before Phase 1
grows the image, switch to a dual-OTA-friendly scheme with more app space, e.g.
`board_build.partitions = min_spiffs.csv` (≈1.9 MB per OTA slot).

### Phase 1 — Scene model + on-device store

- Define the **scene data model** (JSON): per scene, an ordered list of targets
  and messages (CC / program-change / SysEx) + OSC commands for the X32.
- **On-device store** on flash (LittleFS or NVS); survive power-cycle.
- **Replay engine** mirroring the mcp server's recall semantics: program-change
  **before** CC, per-device settle delay, additive vs exact.
- **Footswitch UX**: map banks/switches to songs/scenes; matrix + LED feedback.

### Phase 2 — Provisioning protocol (mcp server → footswitch)

- Primary: small **HTTP endpoint over WiFi** on the footswitch that accepts a
  scene bundle (the mcp server POSTs compiled scenes).
- Fallback: **BLE-MIDI SysEx** transfer for when WiFi is unavailable.
- mcp-midi-controller side: add an "export scene(s) to footswitch" capability.

### Phase 3 — X32 over WiFi/OSC

- Send **OSC/UDP** to the X32 from the footswitch as part of scene replay (mixer
  scene/mute/fader recall). Reuse the verified OSC address knowledge from the
  mcp project's X32 research.

### Phase 4 — Live routing + polish

- Confirm the live MIDI route: footswitch → iPad (BLE-MIDI) → AUM fan-out to the
  pedal WIDI dongles + plugins. Evaluate alternatives (footswitch DIN-out into
  the pedal chain; footswitch as its own BLE central to the pedal hub).
- Polish: richer matrix/LED feedback, scene names, low-battery/health, etc.

## Open design questions

- WiFi + BLE (NimBLE) coexistence headroom on the Lolin32 (RAM + radio sharing).
- Scene-push transport: WiFi/HTTP vs BLE-MIDI SysEx (or both).
- Whether to redesign the bank/CC layout (today: channel 1, CC 60–95, banked) to
  match the scene model rather than the legacy flat map.

## Build & flash quick reference

```bash
# USB build + flash
pio run -e lolin32 -t upload

# Serial monitor
pio device monitor -e lolin32

# OTA flash (device on WiFi, reachable as three.local)
pio run -e ota -t upload

# First-time / re-provision WiFi: hold SW0 (bank-up) while powering on,
# then join AP "THREE" (pass THR33!!!) and pick your network.
```
