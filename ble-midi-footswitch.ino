#include <Arduino.h>

/*
#include <WiFi.h>
#include <WebServer.h>
#include <AutoConnect.h>

WebServer Server;
AutoConnect Portal(Server);
*/

// bluetooth midi
#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>

// LED pins (red, red, green, green, blue, blue)
const int led_pins[6] = {26, 25, 14, 27, 13, 12};

// Button pins
const int btn_pins[6] = {16, 17, 0, 4, 15, 2};

// Connect to first server found
BLEMIDI_CREATE_INSTANCE("ThreeFoot", MIDI);

#ifndef LED_BUILTIN
#define LED_BUILTIN 5 //modify for match with your board
#endif

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

// 8x8 led matrix
#define D5 18 // SCK - Clock for 8x8 matrix
#define D7 23 // MOSI - Data for 8x8 matrix
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
  drawBank();

  /*
  Server.on("/", rootPage);
  AutoConnectConfig config;
  config.apid = +"THREE";
  config.psk = "THR33!!!";
  Portal.config(config);
  Portal.begin();
  */
}

void loop() {

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
  
  //Portal.handleClient();
}

/**
 * This function is called by xTaskCreatePinnedToCore() to perform a multitask execution.
 * In this task, read() is called every millisecond (approx.).
 * read() function performs connection, reconnection and scan-BLE functions.
 * Call read() method repeatedly to perform a successfull connection with the server 
 * in case connection is lost.
*/
void readCB(void *parameter) {
  //  Serial.print("READ Task is started on core: ");
  //  Serial.println(xPortGetCoreID());
  for (;;) {
    MIDI.read(); 
    vTaskDelay(1 / portTICK_PERIOD_MS); //Feed the watchdog of FreeRTOS.
    //Serial.println(uxTaskGetStackHighWaterMark(NULL)); //Only for debug. You can see the watermark of the free resources assigned by the xTaskCreatePinnedToCore() function.
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

/*
void rootPage() {
  char content[] = "Three";
  Server.send(200, "text/plain", content);
}
*/
