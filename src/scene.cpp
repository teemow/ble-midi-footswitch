#include "scene.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace scenes {

namespace {

constexpr const char* kScenesDir = "/scenes";

EventType parseEventType(const char* s) {
  if (!s) return EventType::CC;
  if (!strcmp(s, "cc") || !strcmp(s, "control_change")) return EventType::CC;
  if (!strcmp(s, "pc") || !strcmp(s, "program_change")) return EventType::ProgramChange;
  if (!strcmp(s, "note_on")) return EventType::NoteOn;
  if (!strcmp(s, "note_off")) return EventType::NoteOff;
  if (!strcmp(s, "sysex")) return EventType::SysEx;
  return EventType::CC;
}

TriggerType parseTriggerType(const char* s) {
  if (!s) return TriggerType::None;
  if (!strcmp(s, "pc") || !strcmp(s, "program_change")) return TriggerType::ProgramChange;
  if (!strcmp(s, "cc") || !strcmp(s, "control_change")) return TriggerType::ControlChange;
  if (!strcmp(s, "note_on")) return TriggerType::NoteOn;
  return TriggerType::None;
}

uint8_t clampByte(int v) {
  if (v < 0) return 0;
  if (v > 127) return 127;
  return static_cast<uint8_t>(v);
}

uint8_t clampChannel(int v) {
  if (v < 1) return 1;
  if (v > 16) return 16;
  return static_cast<uint8_t>(v);
}

// Reduce an arbitrary id to a safe flash filename stem ([A-Za-z0-9_-]); any
// other rune becomes '_'. Empty input yields "scene".
String sanitizeId(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    out += ok ? c : '_';
  }
  if (out.isEmpty()) out = "scene";
  return out;
}

// Some scene authors number the controller field "controller" (mirrors the mcp
// server's CC control), others "number"/"data1". Accept any of them.
int firstInt(JsonObjectConst o, std::initializer_list<const char*> keys, int dflt) {
  for (const char* k : keys) {
    if (o[k].is<int>()) return o[k].as<int>();
  }
  return dflt;
}

}  // namespace

const Scene* Store::at(size_t i) const {
  if (i >= scenes_.size()) return nullptr;
  return &scenes_[i];
}

uint8_t Store::displayDigit(size_t i) const {
  if (i >= scenes_.size()) return 0;
  uint8_t b = scenes_[i].bank;
  if (b >= 1 && b <= 9) return b;
  size_t pos = i + 1;
  return pos > 9 ? 9 : static_cast<uint8_t>(pos);
}

int Store::matchProgramChange(uint8_t channel, uint8_t program) const {
  for (size_t i = 0; i < scenes_.size(); i++) {
    const Trigger& t = scenes_[i].trigger;
    if (t.type != TriggerType::ProgramChange) continue;
    if (t.channel != 0 && t.channel != channel) continue;
    if (t.number == program) return static_cast<int>(i);
  }
  return -1;
}

int Store::matchControlChange(uint8_t channel, uint8_t cc, uint8_t value) const {
  for (size_t i = 0; i < scenes_.size(); i++) {
    const Trigger& t = scenes_[i].trigger;
    if (t.type != TriggerType::ControlChange) continue;
    if (t.channel != 0 && t.channel != channel) continue;
    if (t.number != cc) continue;
    if (t.matchValue && t.value != value) continue;
    return static_cast<int>(i);
  }
  return -1;
}

int Store::matchNoteOn(uint8_t channel, uint8_t note) const {
  for (size_t i = 0; i < scenes_.size(); i++) {
    const Trigger& t = scenes_[i].trigger;
    if (t.type != TriggerType::NoteOn) continue;
    if (t.channel != 0 && t.channel != channel) continue;
    if (t.number == note) return static_cast<int>(i);
  }
  return -1;
}

bool Store::loadFile(const String& path, Scene& out) const {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.print("scene: cannot open ");
    Serial.println(path);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("scene: parse error in ");
    Serial.print(path);
    Serial.print(": ");
    Serial.println(err.c_str());
    return false;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();

  // id defaults to the file stem ("/scenes/verse.json" -> "verse").
  String stem = path;
  int slash = stem.lastIndexOf('/');
  if (slash >= 0) stem = stem.substring(slash + 1);
  if (stem.endsWith(".json")) stem = stem.substring(0, stem.length() - 5);

  out.id = root["id"].is<const char*>() ? String(root["id"].as<const char*>()) : stem;
  out.name = root["name"].is<const char*>() ? String(root["name"].as<const char*>()) : out.id;
  out.bank = static_cast<uint8_t>(firstInt(root, {"bank"}, 0));

  out.trigger = Trigger{};
  JsonObjectConst trig = root["trigger"];
  if (!trig.isNull()) {
    out.trigger.type = parseTriggerType(trig["type"].as<const char*>());
    out.trigger.channel = static_cast<uint8_t>(firstInt(trig, {"channel"}, 0));
    out.trigger.number = clampByte(firstInt(trig, {"number", "program", "controller", "note"}, 0));
    if (trig["value"].is<int>()) {
      out.trigger.matchValue = true;
      out.trigger.value = clampByte(trig["value"].as<int>());
    }
  }

  out.events.clear();
  for (JsonObjectConst e : root["events"].as<JsonArrayConst>()) {
    Event ev;
    ev.type = parseEventType(e["type"].as<const char*>());
    ev.channel = clampChannel(firstInt(e, {"channel"}, 1));
    ev.delayMs = static_cast<uint16_t>(firstInt(e, {"delay_ms", "delayMs"}, 0));

    switch (ev.type) {
      case EventType::CC:
        ev.data1 = clampByte(firstInt(e, {"controller", "cc", "number", "data1"}, 0));
        ev.data2 = clampByte(firstInt(e, {"value", "data2"}, 0));
        break;
      case EventType::ProgramChange:
        ev.data1 = clampByte(firstInt(e, {"program", "number", "data1"}, 0));
        break;
      case EventType::NoteOn:
      case EventType::NoteOff:
        ev.data1 = clampByte(firstInt(e, {"note", "number", "data1"}, 0));
        ev.data2 = clampByte(firstInt(e, {"velocity", "value", "data2"}, 0));
        break;
      case EventType::SysEx:
        for (JsonVariantConst b : e["bytes"].as<JsonArrayConst>()) {
          ev.sysex.push_back(static_cast<uint8_t>(b.as<int>() & 0xFF));
        }
        break;
    }
    out.events.push_back(std::move(ev));
  }
  return true;
}

bool Store::scanDir() {
  scenes_.clear();

  File dir = LittleFS.open(kScenesDir);
  if (!dir || !dir.isDirectory()) {
    Serial.println("scene: /scenes is not a directory");
    return true;
  }

  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    String name = entry.name();  // may be bare name or full path depending on core
    entry.close();
    if (!name.endsWith(".json")) continue;

    String path = name.startsWith("/") ? name : (String(kScenesDir) + "/" + name);
    Scene sc;
    if (loadFile(path, sc)) {
      scenes_.push_back(std::move(sc));
    }
  }

  Serial.print("scene: loaded ");
  Serial.print(scenes_.size());
  Serial.println(" scene(s)");
  return true;
}

bool Store::begin() {
  scenes_.clear();

  if (!LittleFS.begin(true /* formatOnFail */)) {
    Serial.println("scene: LittleFS mount failed");
    return false;
  }

  if (!LittleFS.exists(kScenesDir)) {
    LittleFS.mkdir(kScenesDir);
    Serial.println("scene: created /scenes (empty store)");
    return true;
  }

  return scanDir();
}

bool Store::reload() {
  if (!LittleFS.exists(kScenesDir)) {
    LittleFS.mkdir(kScenesDir);
  }
  return scanDir();
}

bool Store::save(const String& id, const String& json, String& err) {
  // Validate before touching flash: must parse and carry an events array, so a
  // malformed push can never leave a corrupt file that breaks the next boot.
  JsonDocument doc;
  DeserializationError perr = deserializeJson(doc, json);
  if (perr) {
    err = String("parse error: ") + perr.c_str();
    return false;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    err = "body is not a JSON object";
    return false;
  }
  if (!root["events"].is<JsonArrayConst>()) {
    err = "missing events array";
    return false;
  }

  String stem = id;
  if (stem.isEmpty() && root["id"].is<const char*>()) {
    stem = root["id"].as<const char*>();
  }
  stem = sanitizeId(stem);

  String path = String(kScenesDir) + "/" + stem + ".json";
  if (!LittleFS.exists(kScenesDir)) {
    LittleFS.mkdir(kScenesDir);
  }
  File f = LittleFS.open(path, "w");
  if (!f) {
    err = String("cannot open ") + path + " for write";
    return false;
  }
  size_t n = f.print(json);
  f.close();
  if (n != json.length()) {
    err = "short write to flash";
    return false;
  }

  Serial.print("scene: saved ");
  Serial.println(path);
  return reload();
}

bool Store::remove(const String& id) {
  String path = String(kScenesDir) + "/" + sanitizeId(id) + ".json";
  if (!LittleFS.exists(path)) {
    return false;
  }
  LittleFS.remove(path);
  Serial.print("scene: removed ");
  Serial.println(path);
  reload();
  return true;
}

}  // namespace scenes
