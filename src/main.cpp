#include <Arduino.h>

// WiFi + OTA (Phase 0). WiFi is best-effort and must never block the live
// BLE-MIDI path: we try saved credentials with a short timeout, and only open
// the captive portal when SW0 (bank-up) is held at boot. OTA + mDNS come up
// only when WiFi connected.
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// bluetooth midi
#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>

// Phase 1: on-device compiled-scene store + replay (see include/scene.h).
#include "scene.h"

// LED pins (red, red, green, green, blue, blue)
const int led_pins[6] = {26, 25, 14, 27, 13, 12};

// Button pins. NOTE: btn_pins[2]=GPIO0, [3]=GPIO4, [4]=GPIO15, [5]=GPIO2 are
// boot-strapping pins; GPIO0 especially must be HIGH at boot. Only btn_pins[0]
// (GPIO16) is a safe pin to sample at boot, so it is the portal trigger below.
const int btn_pins[6] = {16, 17, 0, 4, 15, 2};

// Connect to first server found
BLEMIDI_CREATE_INSTANCE("ThreeFoot", MIDI);

#ifndef LED_BUILTIN
#define LED_BUILTIN 5 //modify for match with your board
#endif

// --- WiFi / OTA configuration ---------------------------------------------
// Passwords are kept out of this public repo: copy include/secrets.h.example to
// include/secrets.h (gitignored) and set them there. The fallback defaults
// below keep the build working without it; change them before exposing the
// device on a network you do not control.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "changeme-ota"   // OTA upload auth (override in secrets.h)
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "changeme-ap"     // captive portal passphrase (override in secrets.h)
#endif

static const char *OTA_HOSTNAME = "three";      // -> three.local for espota
static const char *AP_SSID = "THREE";            // captive portal SSID
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;

bool wifiReady = false;
bool otaInProgress = false;

// Continuos Read function (See FreeRTOS multitasks)
void readCB(void *parameter);

bool isConnected = false;

int btn_states[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
int btn_last_states[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};

// the following variables are unsigned longs because the time, measured in
// milliseconds, will quickly become a bigger number than can be stored in an int.
unsigned long btn_last_debounce_times[6] = {0, 0, 0, 0, 0, 0};
unsigned long debounceDelay = 30;

// Scene store + the currently selected scene (index into the store). The old
// "bank" concept is now a scene cursor: SW0/SW1 step it and replay; inbound MIDI
// from AUM selects + replays a scene by its trigger.
scenes::Store sceneStore;
int currentScene = 0;

// The store is read from the MIDI task (inbound triggers) and the loop task
// (foot nav + the Phase 2 HTTP push that mutates it). A recursive mutex
// serialises every access so a push can never reallocate the scene vector while
// a replay holds a Scene*. Recursive so selectScene()->replaySelected()->draw
// can each lock without deadlocking. Held briefly except during replay (which
// only happens at design time / between songs, when no push is in flight).
SemaphoreHandle_t sceneMux = nullptr;
inline void lockScenes()   { if (sceneMux) xSemaphoreTakeRecursive(sceneMux, portMAX_DELAY); }
inline void unlockScenes() { if (sceneMux) xSemaphoreGiveRecursive(sceneMux); }

// Scene push + introspection over WiFi (Phase 2). Design-time only: the
// mcp-midi-controller compiles a scene and POSTs it here; live shows are
// BLE-only. Plain HTTP on the trusted home network, same trust model as OTA.
WebServer httpServer(80);

// Phase 3: OSC/UDP to a mixer (the X32) as part of scene replay. Each OSC event
// in a compiled scene carries its own UDP target (host/port baked from the
// device binding), so this one socket fans out to whatever the scene addresses.
// Sends are best-effort and only happen when WiFi is up; the BLE-MIDI live path
// is never gated on it.
WiFiUDP oscUdp;

// Fixed transport CCs for the action switches (SW2..SW5), sent to AUM on ch 1
// val 127. These are decoupled from scene selection (SW3 stop/rewind, SW4
// record, SW5 play; SW2 free). Adjust to match the AUM MIDI mapping. Indices
// 0/1 are unused (those switches are scene nav).
const int transport_cc[6] = {-1, -1, 60, 61, 62, 63};

// 8x8 led matrix. The data/clock pins (D7=GPIO23, D5=GPIO18) are provided as
// build flags in platformio.ini so the WEMOS_Matrix_GFX library compiles too.
#define matrix_row 8
#define matrix_col 8

#include <Adafruit_GFX.h>
#include <WEMOS_Matrix_GFX.h>

MLED matrix(7);

static const uint8_t PROGMEM
  heart_bmp[] = 
  { B00000000,
    B01100110,
    B10011001,
    B10000001,
    B01000010,
    B00100100,
    B00011000,
    B00000000 },
  smile_bmp[] =
  { B00000000,
    B01100110,
    B01100110,
    B00000000,
    B00000000,
    B01000010,
    B00111100,
    B00000000 },
  wink_bmp[] =
  { B00000000,
    B01100000,
    B01100110,
    B00000000,
    B00000000,
    B01000010,
    B00111100,
    B00000000 },
  frown_bmp[] =
  { B00000000,
    B00000000,
    B01100110,
    B00000000,
    B00000000,
    B00111100,
    B01000010,
    B00000000 };

// Forward declarations
void clearMatrix();
void drawScene();
void replayScene(const scenes::Scene &sc);
void sendOsc(const scenes::Event &ev);
void replaySelected();
void selectScene(int index, bool replay);
void drawSmile();
void drawWink();
void drawFrown();
void drawHeart();
void drawChar(char c);
bool buttonPressed(int i);
void sendControlChange(int note, byte led);
void blinkLEDRed(int ms);
void blinkLEDGreen(int ms);
void blinkLEDBlue(int ms);
void blinkAllLEDs(int ms);
void setupOTA();
void setupHTTP();
void startConfigPortal();
void maybeSetupWiFi(bool startPortal);

void setup() {
  Serial.begin(115200);

  // Created before the scene store / inbound handlers so every store access is
  // serialisable from the very first event.
  sceneMux = xSemaphoreCreateRecursiveMutex();

  clearMatrix();

  // initialize button pins
  for (int i = 0; i < 6; i++) {
    pinMode(btn_pins[i], INPUT_PULLUP);
  }
  
  // initialize LED pins
  for (int i = 0; i < 6; i++) {
    pinMode(led_pins[i], OUTPUT);
  }

  // Hold SW0 (bank-up, GPIO16) at boot to open the WiFi captive portal.
  // Sampled before BLE/WiFi come up. Small settle delay for the pull-up.
  delay(50);
  bool portalRequested = (digitalRead(btn_pins[0]) == LOW);

  MIDI.begin(MIDI_CHANNEL_OMNI);

  // Request a larger ATT MTU than the 23-byte BLE default. The NimBLE backend
  // sends every BLE-MIDI packet as one notify(), so a packet cannot exceed
  // (MTU - 3) bytes. The SL-2 is driven over BLE with Roland DT1 SysEx, and a
  // single DT1 frame is up to ~19 bytes (e.g. a 4-nibble SYSTEM TEMPO write) ->
  // ~22 bytes on the wire once the BLE-MIDI header/timestamps are added, which
  // overflows the default 20-byte payload and silently truncates the SysEx.
  // 247 covers any single SL-2 DT1 frame; the central negotiates down if it
  // cannot support it. MIDI.begin() above already ran BLEDevice::init().
  NimBLEDevice::setMTU(247);

  BLEMIDI.setHandleConnected([]() {
    Serial.println("---------CONNECTED---------");
    isConnected = true;
    
    drawSmile();
    blinkLEDBlue(50);
    vTaskDelay(50/portTICK_PERIOD_MS);
    blinkLEDBlue(50);
    vTaskDelay(850/portTICK_PERIOD_MS);
    drawScene();
  });

  BLEMIDI.setHandleDisconnected([]() {
    Serial.println("---------NOT CONNECTED---------");
    isConnected = false;

    drawFrown();
    blinkLEDRed(50);
    vTaskDelay(50/portTICK_PERIOD_MS);
    blinkLEDRed(50);
    vTaskDelay(850/portTICK_PERIOD_MS);
    drawScene();
  });

  // Inbound MIDI from AUM selects + replays a scene (the live in-song path:
  // AUM sends one message, "three" expands it into the scene's outgoing MIDI).
  MIDI.setHandleProgramChange([](byte channel, byte program) {
    lockScenes();
    int i = sceneStore.matchProgramChange(channel, program);
    if (i >= 0) selectScene(i, true);
    unlockScenes();
  });

  MIDI.setHandleControlChange([](byte channel, byte cc, byte value) {
    lockScenes();
    int i = sceneStore.matchControlChange(channel, cc, value);
    if (i >= 0) selectScene(i, true);
    unlockScenes();
  });

  MIDI.setHandleNoteOn([](byte channel, byte note, byte velocity) {
    digitalWrite(LED_BUILTIN, LOW);
    lockScenes();
    int i = sceneStore.matchNoteOn(channel, note);
    if (i >= 0) selectScene(i, true);
    unlockScenes();
  });

  MIDI.setHandleNoteOff([](byte channel, byte note, byte velocity) {
    digitalWrite(LED_BUILTIN, HIGH);
  });

  // Mount the scene store (LittleFS /scenes). Best-effort: a missing/empty store
  // just means no scenes to replay yet (the mcp server pushes them in Phase 2).
  lockScenes();
  sceneStore.begin();
  unlockScenes();

  //See FreeRTOS for more multitask info
  xTaskCreatePinnedToCore(
    readCB,
    "MIDI-READ",
    4096,   // headroom: inbound handlers now drive scene replay from this task
    NULL,
    1,
    NULL,
    1); //Core0 or Core1

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Greeting
  blinkLEDRed(100);
  blinkLEDGreen(100);
  blinkLEDBlue(100);

  drawSmile();
  delay(1000);
  drawWink();
  delay(1000);
  drawHeart();
  delay(1000);

  // WiFi + OTA. If the portal was requested, that path blocks here (by design)
  // until the user finishes provisioning; otherwise it is a short best-effort
  // connect and we fall through to BLE-only if it fails.
  maybeSetupWiFi(portalRequested);

  drawScene();
}

void loop() {

  // Service OTA + the scene-push HTTP server when on WiFi (both non-blocking
  // until an update/request actually streams).
  if (wifiReady) {
    ArduinoOTA.handle();
    httpServer.handleClient();
  }

  // upper red: next scene (foot fallback for selecting + replaying a scene).
  if (buttonPressed(0)) {
    digitalWrite(led_pins[0], HIGH);
    lockScenes();
    selectScene(currentScene + 1, true);
    unlockScenes();
    vTaskDelay(250/portTICK_PERIOD_MS);
    digitalWrite(led_pins[0], LOW);
  }

  // lower red: previous scene.
  if (buttonPressed(1)) {
    digitalWrite(led_pins[1], HIGH);
    lockScenes();
    selectScene(currentScene - 1, true);
    unlockScenes();
    vTaskDelay(250/portTICK_PERIOD_MS);
    digitalWrite(led_pins[1], LOW);
  }

  // action switches: fixed transport CCs to AUM (decoupled from scene select).
  for (int i = 2; i < 6; i++) {
    if (buttonPressed(i)) {
      sendControlChange(transport_cc[i], led_pins[i]);
    }
  }
}

// --- WiFi / OTA -------------------------------------------------------------

void maybeSetupWiFi(bool startPortal) {
  WiFi.mode(WIFI_STA);

  if (startPortal) {
    startConfigPortal();
    return;
  }

  // Best-effort connect using credentials saved by a previous portal run.
  drawChar('w');
  WiFi.begin();
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
    setupOTA();
    setupHTTP();
  } else {
    Serial.println("WiFi not connected (BLE-only). Hold SW0 at boot to configure.");
    WiFi.disconnect(true);   // free the radio for BLE if we are not using WiFi
  }
}

void startConfigPortal() {
  Serial.println("Starting WiFi config portal (AP THREE)...");
  drawChar('P');

  WiFiManager wm;
  wm.setConfigPortalTimeout(180); // give up after 3 min, fall back to BLE-only
  bool ok = wm.startConfigPortal(AP_SSID, AP_PASSWORD);

  if (ok && WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    Serial.print("WiFi configured + connected: ");
    Serial.println(WiFi.localIP());
    setupOTA();
    setupHTTP();
  } else {
    Serial.println("Config portal timed out; continuing BLE-only.");
    WiFi.disconnect(true);
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);   // registers mDNS three.local
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    drawChar('U');
  });
  ArduinoOTA.onEnd([]() {
    drawChar('D');
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    // Cheap heartbeat on the builtin LED during the flash.
    digitalWrite(LED_BUILTIN, (progress / 8192) % 2);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    drawChar('E');
  });

  ArduinoOTA.begin();
  Serial.println("OTA ready (hostname: three.local)");
}

// --- Scene push HTTP API (Phase 2) -----------------------------------------
//
// The mcp-midi-controller compiles a song's scene into the on-device schema
// (see data/scenes/README.md) and pushes it here. Endpoints:
//
//   GET    /            -> plain-text status (scene count + names)
//   GET    /scenes      -> JSON array of {id,name,bank,events} currently loaded
//   POST   /scenes      -> body is one compiled scene JSON; stored + hot-reloaded
//                          (?id=<stem> overrides the filename, else the body "id")
//   DELETE /scenes?id=  -> remove a stored scene
//
// Unauthenticated by design: same trust model as OTA (a private LAN), and the
// repo is public so no endpoint/credential is baked in here.

void handleHttpRoot() {
  String body = "three: BLE-MIDI scene player\n";
  lockScenes();
  body += "scenes: " + String(sceneStore.count()) + "\n";
  for (size_t i = 0; i < sceneStore.count(); i++) {
    const scenes::Scene* sc = sceneStore.at(i);
    if (sc) body += "  - " + sc->id + " (" + sc->name + ")\n";
  }
  unlockScenes();
  httpServer.send(200, "text/plain", body);
}

void handleHttpListScenes() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  lockScenes();
  for (size_t i = 0; i < sceneStore.count(); i++) {
    const scenes::Scene* sc = sceneStore.at(i);
    if (!sc) continue;
    JsonObject o = arr.add<JsonObject>();
    o["id"] = sc->id;
    o["name"] = sc->name;
    o["bank"] = sc->bank;
    o["events"] = sc->events.size();
  }
  unlockScenes();
  String out;
  serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

void handleHttpPostScene() {
  if (!httpServer.hasArg("plain") || httpServer.arg("plain").isEmpty()) {
    httpServer.send(400, "text/plain", "empty body; POST the scene JSON\n");
    return;
  }
  String id = httpServer.hasArg("id") ? httpServer.arg("id") : String();
  String body = httpServer.arg("plain");

  String err;
  lockScenes();
  bool ok = sceneStore.save(id, body, err);
  size_t total = sceneStore.count();
  unlockScenes();

  if (!ok) {
    httpServer.send(400, "text/plain", "scene rejected: " + err + "\n");
    return;
  }
  httpServer.send(200, "text/plain",
                  "stored; " + String(total) + " scene(s) loaded\n");
}

void handleHttpDeleteScene() {
  if (!httpServer.hasArg("id")) {
    httpServer.send(400, "text/plain", "missing ?id=<scene>\n");
    return;
  }
  String id = httpServer.arg("id");
  lockScenes();
  bool ok = sceneStore.remove(id);
  size_t total = sceneStore.count();
  unlockScenes();

  if (!ok) {
    httpServer.send(404, "text/plain", "no such scene: " + id + "\n");
    return;
  }
  httpServer.send(200, "text/plain",
                  "removed; " + String(total) + " scene(s) loaded\n");
}

void setupHTTP() {
  httpServer.on("/", HTTP_GET, handleHttpRoot);
  httpServer.on("/scenes", HTTP_GET, handleHttpListScenes);
  httpServer.on("/scenes", HTTP_POST, handleHttpPostScene);
  httpServer.on("/scenes", HTTP_DELETE, handleHttpDeleteScene);
  httpServer.begin();
  Serial.println("HTTP scene API ready on port 80 (POST /scenes)");
}

/**
 * This function is called by xTaskCreatePinnedToCore() to perform a multitask execution.
 * In this task, read() is called every millisecond (approx.).
 * read() function performs connection, reconnection and scan-BLE functions.
 * Call read() method repeatedly to perform a successfull connection with the server 
 * in case connection is lost.
*/
void readCB(void *parameter) {
  for (;;) {
    MIDI.read(); 
    vTaskDelay(1 / portTICK_PERIOD_MS); //Feed the watchdog of FreeRTOS.
  }
  vTaskDelay(1);
}

void sendControlChange(int note, byte led) {
  if (note < 0) return;  // unmapped switch
  if (isConnected) {
    MIDI.sendControlChange(note, 127, 1);
    digitalWrite(led, HIGH);
    vTaskDelay(250/portTICK_PERIOD_MS);
    digitalWrite(led, LOW);
  } else {
    drawFrown();

    blinkAllLEDs(10);
    vTaskDelay(10/portTICK_PERIOD_MS);
    blinkAllLEDs(10);

    vTaskDelay(500/portTICK_PERIOD_MS);
    drawScene();
  }
}

// --- Scene replay -----------------------------------------------------------

// Emit a compiled scene's ordered events over BLE-MIDI. The mcp server has
// already resolved ordering (PC before CC) and settle windows into per-event
// delays, so this is a faithful, dumb player.
void replayScene(const scenes::Scene &sc) {
  for (const scenes::Event &ev : sc.events) {
    switch (ev.type) {
      case scenes::EventType::CC:
        MIDI.sendControlChange(ev.data1, ev.data2, ev.channel);
        break;
      case scenes::EventType::ProgramChange:
        MIDI.sendProgramChange(ev.data1, ev.channel);
        break;
      case scenes::EventType::NoteOn:
        MIDI.sendNoteOn(ev.data1, ev.data2, ev.channel);
        break;
      case scenes::EventType::NoteOff:
        MIDI.sendNoteOff(ev.data1, ev.data2, ev.channel);
        break;
      case scenes::EventType::SysEx:
        if (!ev.sysex.empty()) {
          // bytes include the F0..F7 boundaries -> ArrayContainsBoundaries=true
          MIDI.sendSysEx(ev.sysex.size(), ev.sysex.data(), true);
        }
        break;
      case scenes::EventType::Osc:
        sendOsc(ev);
        break;
    }
    if (ev.delayMs > 0) {
      vTaskDelay(ev.delayMs / portTICK_PERIOD_MS);
    }
  }
}

// --- OSC/UDP (Phase 3) ------------------------------------------------------
//
// Minimal OSC 1.0 encoder: an address pattern, a comma-led type-tag string, and
// the args, each section null-terminated and zero-padded to a 4-byte boundary.
// Built into a heap vector so an over-long string arg can never overflow a
// stack buffer (replay is occasional, between songs).

namespace {

void oscAppendString(std::vector<uint8_t> &b, const char *s) {
  for (const char *p = s; *p; ++p) b.push_back(static_cast<uint8_t>(*p));
  b.push_back(0);
  while (b.size() % 4 != 0) b.push_back(0);
}

void oscAppendU32(std::vector<uint8_t> &b, uint32_t v) {
  b.push_back((v >> 24) & 0xFF);  // OSC is big-endian
  b.push_back((v >> 16) & 0xFF);
  b.push_back((v >> 8) & 0xFF);
  b.push_back(v & 0xFF);
}

}  // namespace

// Encode + send one OSC event over UDP. Best-effort: silently no-ops when WiFi
// is down or the target/address is missing, so a mixer event in a scene never
// blocks the BLE-MIDI replay of the same scene.
void sendOsc(const scenes::Event &ev) {
  if (!wifiReady) return;
  if (ev.oscAddr.isEmpty() || ev.oscHost.isEmpty()) return;

  std::vector<uint8_t> buf;
  oscAppendString(buf, ev.oscAddr.c_str());

  String tags = ",";
  for (const scenes::OscArg &a : ev.oscArgs) tags += a.tag;
  oscAppendString(buf, tags.c_str());

  for (const scenes::OscArg &a : ev.oscArgs) {
    switch (a.tag) {
      case 'f': {
        uint32_t bits;
        memcpy(&bits, &a.f, sizeof(bits));
        oscAppendU32(buf, bits);
        break;
      }
      case 's':
        oscAppendString(buf, a.s.c_str());
        break;
      case 'i':
      default:
        oscAppendU32(buf, static_cast<uint32_t>(a.i));
        break;
    }
  }

  if (oscUdp.beginPacket(ev.oscHost.c_str(), ev.oscPort) != 1) {
    Serial.print("osc: cannot resolve ");
    Serial.println(ev.oscHost);
    return;
  }
  oscUdp.write(buf.data(), buf.size());
  oscUdp.endPacket();

  Serial.print("osc: ");
  Serial.print(ev.oscAddr);
  Serial.print(" -> ");
  Serial.print(ev.oscHost);
  Serial.print(":");
  Serial.println(ev.oscPort);
}

void replaySelected() {
  const scenes::Scene *sc = sceneStore.at(currentScene);
  if (sc == nullptr) return;
  if (!isConnected) {
    // can't reach AUM; flash a frown like a failed transport press
    drawFrown();
    blinkAllLEDs(10);
    vTaskDelay(500/portTICK_PERIOD_MS);
    drawScene();
    return;
  }
  Serial.print("scene: replay ");
  Serial.println(sc->name);
  replayScene(*sc);
}

// Move the cursor to a scene (clamped) and optionally replay it. Triggered both
// by the foot (SW0/SW1) and by inbound MIDI from AUM.
void selectScene(int index, bool replay) {
  size_t n = sceneStore.count();
  if (n == 0) {
    drawScene();
    return;
  }
  if (index < 0) index = 0;
  if (index >= (int)n) index = (int)n - 1;
  currentScene = index;
  drawScene();
  if (replay) replaySelected();
}

bool buttonPressed(int i) {
  int reading = digitalRead(btn_pins[i]);

  // reset the debouncing timer
  if (reading != btn_last_states[i]) {
    btn_last_debounce_times[i] = millis();
  }
  btn_last_states[i] = reading;

  if ((millis() - btn_last_debounce_times[i]) > debounceDelay) {
    if (reading != btn_states[i]) {
      btn_states[i] = reading;

      if (btn_states[i] == LOW) {
        return true;
      }
    }
  }
  return false;
}

void blinkLEDRed1(int ms) {
  digitalWrite(led_pins[0], HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_pins[0], LOW);
}

void blinkLEDRed2(int ms) {
  digitalWrite(led_pins[1], HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_pins[1], LOW);
}

void blinkLEDGreen1(int ms) {
  digitalWrite(led_pins[2], HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_pins[2], LOW);
}

void blinkLEDGreen2(int ms) {
  digitalWrite(led_pins[3], HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_pins[3], LOW);
}

void blinkLEDBlue1(int ms) {
  digitalWrite(led_pins[4], HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_pins[4], LOW);
}

void blinkLEDBlue2(int ms) {
  digitalWrite(led_pins[5], HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_pins[5], LOW);
}

void blinkLEDBlue(int ms) {
  blinkLEDBlue1(ms);
  blinkLEDBlue2(ms);
}

void blinkLEDGreen(int ms) {
  blinkLEDGreen1(ms);
  blinkLEDGreen2(ms);
}

void blinkLEDRed(int ms) {
  blinkLEDRed1(ms);
  blinkLEDRed2(ms);
}

void blinkAllLEDs(int ms) {
  blinkLEDBlue(ms);
  blinkLEDGreen(ms);
  blinkLEDRed(ms);
}

void clearMatrix() {
  matrix.clear();
  matrix.writeDisplay();
}

// Show the current scene's display digit (1..9). A dash means the store is
// empty (no scenes pushed yet).
void drawScene() {
  matrix.clear();
  lockScenes();
  bool empty = sceneStore.empty();
  uint8_t d = empty ? 0 : sceneStore.displayDigit(currentScene);
  unlockScenes();
  if (empty) {
    matrix.drawChar(1, 1, '-', 1, 0, 1);
  } else {
    matrix.drawChar(1, 1, d + 48, 1, 0, 1);
  }
  matrix.writeDisplay();
}

void drawChar(char c) {
  matrix.clear();
  matrix.drawChar(1, 1, c, 1, 0, 1);
  matrix.writeDisplay();
}

void drawSmile() {
  matrix.clear();
  matrix.drawBitmap(0, 0, smile_bmp, 8, 8, LED_ON);
  matrix.writeDisplay();
}

void drawWink() {
  matrix.clear();
  matrix.drawBitmap(0, 0, wink_bmp, 8, 8, LED_ON);
  matrix.writeDisplay();
}

void drawFrown() {
  matrix.clear();
  matrix.drawBitmap(0, 0, frown_bmp, 8, 8, LED_ON);
  matrix.writeDisplay();
}

void drawHeart() {
  matrix.clear();
  matrix.drawBitmap(0, 0, heart_bmp, 8, 8, LED_ON);
  matrix.writeDisplay();
}
