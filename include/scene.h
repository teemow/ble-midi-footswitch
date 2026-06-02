// Phase 1 scene model + on-device store for the "three" footswitch.
//
// A scene is a *compiled* unit pushed by the mcp-midi-controller: an inbound
// trigger plus an already-ordered list of outgoing wire events. The footswitch
// is a faithful player -- it does NOT re-derive recall semantics. The mcp server
// resolves program-change-before-CC ordering, per-device settle delays, and
// additive/exact mode at compile time and bakes the result into `events`
// (settle/ordering encoded as the per-event `delay_ms` gap that follows a send).
//
// On device, scenes live as JSON files under /scenes on LittleFS. Live in-song
// switching is driven by inbound MIDI from AUM (a single message -> a scene);
// the bank-up/down footswitches navigate scenes by foot as a manual fallback.

#pragma once

#include <Arduino.h>
#include <vector>

namespace scenes {

// Wire event kinds the footswitch can emit. OSC (X32) arrives in Phase 3.
enum class EventType : uint8_t {
  CC,
  ProgramChange,
  NoteOn,
  NoteOff,
  SysEx,
};

// How AUM (or any peer) selects this scene over the inbound MIDI link.
enum class TriggerType : uint8_t {
  None,           // foot-only; never matched by inbound MIDI
  ProgramChange,  // match on PC number
  ControlChange,  // match on CC number (optionally a specific value)
  NoteOn,         // match on note number
};

// One outgoing message. `delayMs` is honoured *after* the send, which is how the
// compiler encodes settle windows and inter-message spacing.
struct Event {
  EventType type = EventType::CC;
  uint8_t channel = 1;             // 1..16
  uint8_t data1 = 0;               // cc number / program / note
  uint8_t data2 = 0;               // cc value / velocity (ignored for PC)
  std::vector<uint8_t> sysex;      // full bytes incl. F0..F7 (SysEx only)
  uint16_t delayMs = 0;            // wait after sending this event
};

// Inbound match spec.
struct Trigger {
  TriggerType type = TriggerType::None;
  uint8_t channel = 0;             // 0 = any channel
  uint8_t number = 0;              // program / cc number / note
  bool matchValue = false;         // CC only: also require a specific value
  uint8_t value = 0;
};

// A fully-loaded scene (kept in RAM; setlists are small).
struct Scene {
  String id;                       // file stem (e.g. "verse")
  String name;                     // human label
  uint8_t bank = 0;                // matrix display digit 1..9 (0 = list pos)
  Trigger trigger;
  std::vector<Event> events;
};

// On-device scene store backed by LittleFS (/scenes/*.json). All scenes are
// loaded into RAM at begin() so inbound triggering never blocks on flash I/O.
class Store {
 public:
  // Mounts LittleFS (formatting on first run), ensures /scenes exists, and loads
  // every scene file. Returns false only if the filesystem cannot be mounted.
  bool begin();

  // Re-scans /scenes and rebuilds the in-RAM set (assumes LittleFS is already
  // mounted by begin()). Used after a scene is pushed/removed over HTTP so the
  // change takes effect without a reboot.
  bool reload();

  // Validates and persists one pushed scene as /scenes/<id>.json (Phase 2 push),
  // then reloads the store. `id` is sanitised to [A-Za-z0-9_-]; if empty it
  // falls back to the scene's own "id" field. On failure returns false and sets
  // err. Mutates the in-RAM set, so callers must serialise it against replay.
  bool save(const String& id, const String& json, String& err);

  // Deletes /scenes/<id>.json and reloads. Returns false if the file is absent.
  bool remove(const String& id);

  size_t count() const { return scenes_.size(); }
  bool empty() const { return scenes_.empty(); }
  const Scene* at(size_t i) const;

  // Inbound-trigger lookup. Returns the scene index or -1 if nothing matches.
  int matchProgramChange(uint8_t channel, uint8_t program) const;
  int matchControlChange(uint8_t channel, uint8_t cc, uint8_t value) const;
  int matchNoteOn(uint8_t channel, uint8_t note) const;

  // Display digit (1..9) for the scene at index i: its `bank` if set, else the
  // 1-based list position clamped to 9.
  uint8_t displayDigit(size_t i) const;

 private:
  std::vector<Scene> scenes_;

  // Scans /scenes and rebuilds scenes_ (LittleFS must already be mounted).
  bool scanDir();
  bool loadFile(const String& path, Scene& out) const;
};

}  // namespace scenes
