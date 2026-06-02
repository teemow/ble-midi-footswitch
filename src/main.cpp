#include <Arduino.h>

// WiFi + OTA (Phase 0). WiFi is best-effort and must never block the live
// BLE-MIDI path: we try saved credentials with a short timeout, and only open
// the captive portal when SW0 (bank-up) is held at boot. OTA + mDNS come up
// only when WiFi connected.
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>

// bluetooth midi
#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>

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

int bank = 1;

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
void drawBank();
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
void startConfigPortal();
void maybeSetupWiFi(bool startPortal);

void setup() {
  Serial.begin(115200);

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

  BLEMIDI.setHandleConnected([]() {
    Serial.println("---------CONNECTED---------");
    isConnected = true;
    
    drawSmile();
    blinkLEDBlue(50);
    vTaskDelay(50/portTICK_PERIOD_MS);
    blinkLEDBlue(50);
    vTaskDelay(850/portTICK_PERIOD_MS);
    drawBank();
  });

  BLEMIDI.setHandleDisconnected([]() {
    Serial.println("---------NOT CONNECTED---------");
    isConnected = false;

    drawFrown();
    blinkLEDRed(50);
    vTaskDelay(50/portTICK_PERIOD_MS);
    blinkLEDRed(50);
    vTaskDelay(850/portTICK_PERIOD_MS);
    drawBank();
  });

  MIDI.setHandleNoteOn([](byte channel, byte note, byte velocity) {
    Serial.print("NoteON: CH: ");
    Serial.print(channel);
    Serial.print(" | ");
    Serial.print(note);
    Serial.print(", ");
    Serial.println(velocity);
    digitalWrite(LED_BUILTIN, LOW);
  });
  
  MIDI.setHandleNoteOff([](byte channel, byte note, byte velocity) {
    digitalWrite(LED_BUILTIN, HIGH);
  });

  //See FreeRTOS for more multitask info
  xTaskCreatePinnedToCore(
    readCB,  
    "MIDI-READ",
    3000,
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

  drawBank();
}

void loop() {

  // Service OTA when on WiFi (non-blocking until an update actually streams).
  if (wifiReady) {
    ArduinoOTA.handle();
  }

  // upper red is bank up
  if (buttonPressed(0)) {
    bank++;
    if (bank > 9) {
      bank = 9;
    }
    digitalWrite(led_pins[0], HIGH);
    drawBank();
    digitalWrite(led_pins[0], LOW);
  }

  // lower red is bank down
  if (buttonPressed(1)) {
    bank--;
    if (bank < 1) {
      bank = 1;
    }
    digitalWrite(led_pins[1], HIGH);
    drawBank();
    vTaskDelay(250/portTICK_PERIOD_MS);
    digitalWrite(led_pins[1], LOW);
  }

  // all others send midi cc
  for (int i = 2; i < 6; i++) {
    if (buttonPressed(i)) {
      sendControlChange(58 + i + (bank - 1) * 4, led_pins[i]);
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
    drawBank();
  }
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

void drawBank() {
  matrix.clear();
  matrix.drawChar(1, 1, bank + 48, 1, 0, 1);
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
