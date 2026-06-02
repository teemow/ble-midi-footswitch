# On-device scene store (`/scenes` on LittleFS)

Each `*.json` file here is one **compiled scene**. The firmware loads them all at
boot. They can be flashed with `pio run -t uploadfs` (uploads this whole `data/`
directory to the device's LittleFS partition), or pushed live over WiFi by the
`mcp-midi-controller` server (Phase 2, see below).

## Scene push HTTP API (Phase 2)

When on WiFi, `three` serves a small HTTP API on port 80 so the mcp server can
push compiled scenes without a re-flash (same trust model as OTA — a private
LAN, unauthenticated):

| Method | Path | Body / args | Effect |
|--------|------|-------------|--------|
| `GET` | `/` | — | plain-text status (scene count + names) |
| `GET` | `/scenes` | — | JSON array of `{id,name,bank,events}` loaded |
| `POST` | `/scenes` | one scene JSON (this schema); `?id=<stem>` overrides the filename, else the body `id` | validate → write `/scenes/<id>.json` → hot-reload |
| `DELETE` | `/scenes?id=<stem>` | — | remove a stored scene + reload |

A push is validated (must parse and carry an `events` array) before any flash
write, so a bad push can never corrupt the store. The mcp side resolves all
recall semantics (ordering/settle/additive-exact) at compile time and emits this
exact schema; `three` stays a faithful player.

`POST /scenes` **requires `Content-Type: application/json`**. The ESP32
`WebServer` only exposes a request body as the `plain` arg when it is *not*
form-encoded, so a default `curl --data` (which sends
`application/x-www-form-urlencoded`) is rejected with `400 "empty body"`. Use:

```
curl -X POST -H 'Content-Type: application/json' \
     --data-binary @example-verse.json http://three.local/scenes
```

## Scene schema

```json
{
  "id": "verse",                  // optional; defaults to the file name stem
  "name": "Verse",               // shown in logs
  "bank": 1,                      // matrix display digit 1..9 (0 = list position)
  "trigger": {                    // how AUM selects this scene over inbound MIDI
    "type": "program_change",     // program_change | control_change | note_on
    "channel": 1,                 // 0 = match any channel
    "number": 0,                  // PC number / CC number / note
    "value": 127                  // control_change only: also require this value
  },
  "events": [                     // outgoing wire events, already ordered
    { "type": "program_change", "channel": 2, "program": 12, "delay_ms": 80 },
    { "type": "cc",             "channel": 2, "controller": 28, "value": 127 },
    { "type": "cc",             "channel": 3, "controller": 17, "value": 64 },
    { "type": "osc", "osc_addr": "/ch/01/mix/fader", "osc_types": "f",
      "osc_args": [0.75], "host": "192.168.1.50", "port": 10023 }
  ]
}
```

### Notes

- **The list is a faithful player.** The mcp server resolves recall semantics
  (program-change before CC, additive vs exact) at compile time and bakes them
  into the `events` order. A per-event `delay_ms` is honoured *after* the send
  and is how device **settle** windows are expressed.
- **Triggering is inbound-driven.** AUM sends a single MIDI message; the matching
  scene's `events` are emitted back over the BLE-MIDI link (AUM fans them out to
  the rig). SW0/SW1 also step through scenes by foot as a manual fallback.
- Event types: `cc` (`controller`, `value`), `program_change` (`program`),
  `note_on`/`note_off` (`note`, `velocity`), `sysex` (`bytes`: full array
  including the `0xF0`/`0xF7` boundaries), and `osc` (see below). `channel` is
  1..16 (MIDI events only; `osc` ignores it).
- Unknown keys (e.g. a per-event `comment`) are ignored by the parser, so they
  are safe to use for documenting raw SysEx byte arrays in place.

### OSC events (Phase 3 — the X32 over WiFi)

An `osc` event sends an OSC/UDP message to a mixer (the Behringer X32) as part of
the same scene replay that drives the pedals over BLE-MIDI. The MIDI events still
go to AUM over BLE; the `osc` events go straight to the mixer over WiFi.

| Field | Meaning |
|-------|---------|
| `osc_addr` | the OSC address pattern, e.g. `/ch/01/mix/fader`, `/-action/goscene` |
| `osc_types` | one tag char **per arg**: `f` = float32, `i` = int32, `s` = string |
| `osc_args` | the args, in order (matched positionally to `osc_types`) |
| `host` | the mixer's IP/hostname (baked from the device's mcp binding) |
| `port` | UDP port (X32 = `10023`; defaults to `10023` if omitted) |

`osc_types` is required because JSON numbers cannot distinguish a float `1.0`
from an int `1`, and the X32 cares (a fader is float `0.0..1.0`, `mix/on` is int
`0/1`). The mcp server's `export_scene_to_footswitch` bakes the type-tag, the
args, and the UDP target (from the X32 binding's `host:port` endpoint) so the
scene is self-describing — the footswitch needs no rig config of its own.

OSC send is **best-effort and gated on WiFi**: if the device booted BLE-only
(no WiFi), `osc` events are silently skipped and the BLE-MIDI events in the same
scene still play. So driving the X32 live requires the footswitch to be on WiFi
*and* BLE at once (the ESP32 time-slices the shared 2.4 GHz radio).

Common X32 addresses (full map: `mcp-midi-controller/docs/research/x32.md`):

| What | `osc_addr` | `osc_types` | `osc_args` |
|------|-----------|-------------|------------|
| Channel 1 fader (0.0–1.0) | `/ch/01/mix/fader` | `f` | `[0.75]` |
| Channel 1 on/off | `/ch/01/mix/on` | `i` | `[1]` |
| Recall console scene N | `/-action/goscene` | `i` | `[3]` |
| Scribble-strip name | `/ch/01/config/name` | `s` | `["Bass"]` |

## Driving the Boss SL-2 over BLE (Roland DT1 SysEx)

`example-sl2.json` is the reference. The SL-2 has **no Program Change** and the
manual calls slicer Type/pattern "front-panel only", but it parses Roland
**DT1 SysEx** on its TRS MIDI IN — reachable over this footswitch's BLE-MIDI link
through the WIDI hub with **no laptop**. So a scene can recall a stored preset and
override pattern/type/tempo live. (RQ1 *readback* still needs USB; over BLE we only
write absolute values.)

DT1 frame (Boss SL-2, model id `00 00 00 00 1D`, default device id `0x10`):

```
F0 41 10 00 00 00 00 1D 12 <a3 a2 a1 a0> <data...> <sum> F7
                        └ DT1 (write)        └ Roland checksum
sum = (0x80 - ((Σ address+data bytes) & 0x7F)) & 0x7F
```

DT1 is **device-id based and channel-independent** — only the channel-voice CCs
below use the SL-2's MIDI **receive channel** (set on the pedal via its power-on
knob combo; the example uses channel 1 — change it to match your SL-2).

Useful addresses (full map in the mcp-midi-controller repo, `docs/research/sl-2.md`):

| What | Address | Data | Example frame (decimal `bytes`) |
|------|---------|------|---------------------------------|
| Recall stored preset n into temp (`PATCH_SELECT`) | `0x7F000100` | `00 <n>` | n=7 → `[240,65,16,0,0,0,0,29,18,127,0,1,0,0,7,121,247]` |
| SLICER(1) PATTERN (0–50) | `0x20001000` | `<p>` | p=10 → `[240,65,16,0,0,0,0,29,18,32,0,16,0,10,70,247]` |
| SLICER(1) FX_TYPE (0–6) | `0x20001002` | `<t>` | t=5 → `[240,65,16,0,0,0,0,29,18,32,0,16,2,5,73,247]` |
| SYSTEM TEMPO (40.0–300.0 BPM) | `0x10000000` | BPM×10 as `INTEGER4x4` (4 bytes, one nibble each) | 120.0 BPM (1200=0x04B0 → `00 04 0B 00`) → `[240,65,16,0,0,0,0,29,18,16,0,0,0,0,4,11,0,97,247]` |

Channel-voice CCs (use the SL-2 receive channel):

| Control | CC# | Notes |
|---------|-----|-------|
| EFFECTS ON/OFF | 80 | `127` = on, `0` = off |
| EXP (assignable) | 16 | Drives whatever SYSTEM `EXP_FUNC` points at. When `EXP_FUNC=2` (TEMPO), `BPM = 40 + value/127*260` — a precise single-message tempo. |
| TAP TEMPO | 81 | Rate-from-interval; **unreliable over BLE** (timing collapses). Prefer CC#16 or a DT1 TEMPO write. |

A single DT1 TEMPO frame is 19 bytes (≈22 on the BLE wire with the BLE-MIDI
header/timestamps), which overflows the default 23-byte ATT MTU. The firmware
calls `NimBLEDevice::setMTU(247)` at boot so these frames are sent in one packet
rather than being truncated.
