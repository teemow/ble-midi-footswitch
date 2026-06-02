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

### Phase 1 — Scene model + on-device store (IN PROGRESS)

**Done (build- and hardware-verified):**

- **Partition scheme switched** to `board_build.partitions = min_spiffs.csv` +
  `board_build.filesystem = littlefs`. The app slot grew from ~1.3 MB to ~1.9 MB,
  so the (now larger) image sits at ~62% instead of ~89%, and a ~190 KB
  LittleFS data partition holds the scene store. Dual-OTA preserved.
- **Compiled-scene model + JSON parser** (`include/scene.h`, `src/scene.cpp`),
  parsed with **ArduinoJson 7**. A scene = an **inbound trigger** + an
  **already-ordered list of outgoing wire events**. The footswitch is a faithful
  *player*: the mcp server resolves recall semantics (PC-before-CC, additive vs
  exact) at **compile time** and bakes them into the event order; per-device
  **settle** is expressed as a per-event `delay_ms` honoured after each send.
- **On-device store** on **LittleFS** (`/scenes/*.json`), loaded into RAM at
  boot. Schema + example in `data/scenes/` (flash with `pio run -t uploadfs`).
- **Replay engine** (`replayScene`) emits the ordered events over BLE-MIDI
  (`cc` / `program_change` / `note_on` / `note_off` / `sysex`).
- **UX / triggering** (refined with the user — see below):
  - **Inbound MIDI from AUM selects + replays a scene** (the live in-song path):
    AUM sends a single message (Program Change is the natural "single signal";
    CC and Note also supported), `three` matches it against scene triggers and
    expands it into the scene's outgoing MIDI. No button press needed in-song.
  - **Banks are now scenes:** SW0/SW1 step the scene cursor and replay (manual
    foot fallback). Matrix shows the scene's display digit (`-` when empty).
  - **SW2–SW5 keep fixed transport CCs to AUM** (decoupled from scene select):
    `transport_cc[] = {-, -, 60, 61, 62, 63}`, ch 1, val 127 — SW3 stop/rewind,
    SW4 record, SW5 play, SW2 free. **Verify/adjust these CC numbers against the
    actual AUM mapping on hardware.**

**Hardware-verified (2026-06-02):** flashed over USB (`-t upload` + `-t uploadfs`).
Serial boot log shows `scene: loaded 1 scene(s)`, WiFi + OTA up. Bonded as a BLE
central, sent **Program Change ch 1 #0**, and `aseqdump`'d `ThreeFoot`: it emitted
exactly the example scene's events in order — `PC ch2 #12`, `CC ch2 #28=127`,
`CC ch3 #17=64` (aseqdump prints 0-based wire channels; values/ordering exact).
The inbound-trigger → store lookup → replay path works end to end.

**Physical-switch checks (2026-06-02, feet on the box):** with the device bonded
to a laptop and `aseqdump`'d, every switch was pressed and captured:
- **SW0 / SW1** (scene nav) both select + replay the scene — emitted the example
  scene's `PC ch2 #12`, `CC ch2 #28=127`, `CC ch3 #17=64` in order. With a single
  scene loaded they clamp to it and re-replay (matrix stays on its digit), as
  designed.
- **SW2–SW5** sent fixed transport CCs **60 / 61 / 62 / 63** (ch 1, val 127),
  matching `transport_cc[]`.
- SW0/SW1 now share the same post-replay LED hold (250 ms) so the foot-nav LED is
  visible. (Note: on *this* unit the SW0 upper-red LED is a dead LED / bad joint —
  switch + MIDI verified working; cosmetic LED repair tracked in the private rig
  notes, not a firmware issue.)

**Remaining for Phase 1 (next):**

- **Confirm the transport CC numbers (60–63) match the live AUM mapping** — needs
  the iPad/AUM (only the *sending* of those CCs is verified so far).
- Tune the inbound-handler behaviour while a scene is mid-replay (currently the
  replay blocks the MIDI-read task; fine for short scenes).
- Decide scene-cursor display for >9 scenes (single matrix digit today).

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

# Upload the scene store (data/scenes/*.json) to the device's LittleFS partition
pio run -e lolin32 -t uploadfs

# First-time / re-provision WiFi: hold SW0 (bank-up) while powering on,
# then join AP "THREE" (pass THR33!!!) and pick your network.
```
