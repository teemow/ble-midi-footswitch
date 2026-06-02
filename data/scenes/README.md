# On-device scene store (`/scenes` on LittleFS)

Each `*.json` file here is one **compiled scene**. The firmware loads them all at
boot. In Phase 2 the `mcp-midi-controller` server pushes these files over WiFi;
for now they can be flashed with `pio run -t uploadfs` (uploads this whole
`data/` directory to the device's LittleFS partition).

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
    { "type": "cc",             "channel": 3, "controller": 17, "value": 64 }
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
  including the `0xF0`/`0xF7` boundaries). `channel` is 1..16.
- Unknown keys (e.g. a per-event `comment`) are ignored by the parser, so they
  are safe to use for documenting raw SysEx byte arrays in place.

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
