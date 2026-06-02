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
