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

// LED pins
const byte led_blue_1 = 13;
const byte led_blue_2 = 12;
const byte led_green_1 = 14;
const byte led_green_2 = 27;
const byte led_red_1 = 26;
const byte led_red_2 = 25;

// Button pins
const int btn_blue_1 = 15;
const int btn_blue_2 = 2;
const int btn_green_1 = 0;
const int btn_green_2 = 4;
const int btn_red_1 = 16;
const int btn_red_2 = 17;

// Connect to first server found
BLEMIDI_CREATE_INSTANCE("ThreeFoot", MIDI);

#ifndef LED_BUILTIN
#define LED_BUILTIN 5 //modify for match with your board
#endif

// Continuos Read function (See FreeRTOS multitasks)
void readCB(void *parameter);

bool isConnected = false;

int btn_state_blue_1;
int btn_state_blue_2;
int btn_state_green_1;
int btn_state_green_2;
int btn_state_red_1;
int btn_state_red_2;
int btn_last_state_blue_1;
int btn_last_state_blue_2;
int btn_last_state_green_1;
int btn_last_state_green_2;
int btn_last_state_red_1;
int btn_last_state_red_2;

int pixel = 0;

// 8x8 led matrix
#define D5 18 // SCK - Clock for 8x8 matrix
#define D7 23 // MOSI - Data for 8x8 matrix
#define matrix_row 8
#define matrix_col 8

#include <Adafruit_GFX.h>
#include <WEMOS_Matrix_GFX.h>

MLED matrix(7);

void drawEyes(boolean winkL, boolean winkR) {
  // left eye
  mled.dot(1,6, winkL ? 0:1 );
  mled.dot(2,6, winkL ? 0:1 );
  mled.dot(1,5);
  mled.dot(2,5);

  //right eye
  mled.dot(5,6, winkR ? 0:1);
  mled.dot(5,5);
  mled.dot(6,6, winkR ? 0:1);
  mled.dot(6,5);
}

  smile_bmp[] =
  { B00111100,
    B01000010,
    B10100101,
    B10000001,
    B10100101,
    B10011001,
    B01000010,
    B00111100 },

void drawHappyMouth() {
  mled.dot(1,2);
  mled.dot(2,1);
  mled.dot(3,1);
  mled.dot(4,1);
  mled.dot(5,1);
  mled.dot(6,2);
}

void drawAngryMouth() {
  mled.dot(1,1);
  mled.dot(2,2);
  mled.dot(3,2);
  mled.dot(4,2);
  mled.dot(5,2);
  mled.dot(6,1);
}

static const uint8_t PROGMEM
  smile_bmp[] =
  { B00111100,
    B01000010,
    B10100101,
    B10000001,
    B10100101,
    B10011001,
    B01000010,
    B00111100 },
  neutral_bmp[] =
  { B00111100,
    B01000010,
    B10100101,
    B10000001,
    B10111101,
    B10000001,
    B01000010,
    B00111100 },
  frown_bmp[] =
  { B00111100,
    B01000010,
    B10100101,
    B10000001,
    B10011001,
    B10100101,
    B01000010,
    B00111100 };

void setup() {
  Serial.begin(115200);

  clearMatrix();

  // initialize button pins
  pinMode(btn_blue_1, INPUT_PULLUP);
  pinMode(btn_blue_2, INPUT_PULLUP);
  pinMode(btn_green_1, INPUT_PULLUP);
  pinMode(btn_green_2, INPUT_PULLUP);
  pinMode(btn_red_1, INPUT_PULLUP);
  pinMode(btn_red_2, INPUT_PULLUP);
  
  // initialize LED pins
  pinMode(led_blue_1, OUTPUT);
  pinMode(led_blue_2, OUTPUT);
  pinMode(led_green_1, OUTPUT);
  pinMode(led_green_2, OUTPUT);
  pinMode(led_red_1, OUTPUT);
  pinMode(led_red_2, OUTPUT);

  MIDI.begin(MIDI_CHANNEL_OMNI);

  BLEMIDI.setHandleConnected([]() {
    Serial.println("---------CONNECTED---------");
    isConnected = true;
    
    drawSmileFace();

    blinkLEDBlue(50);
    vTaskDelay(50/portTICK_PERIOD_MS);
    blinkLEDBlue(50);

    vTaskDelay(850/portTICK_PERIOD_MS);

    clearMatrix();
  });

  BLEMIDI.setHandleDisconnected([]() {
    Serial.println("---------NOT CONNECTED---------");
    isConnected = false;

    drawFrownFace();
    
    blinkLEDRed(50);
    vTaskDelay(50/portTICK_PERIOD_MS);
    blinkLEDRed(50);

    vTaskDelay(850/portTICK_PERIOD_MS);
    
    clearMatrix();
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

  drawNeutralFace();
  delay(1000);

  // wink right
  drawSmileFace();
  delay(1000);

  clearMatrix();

  /*
  Server.on("/", rootPage);
  AutoConnectConfig config;
  config.apid = "THREE";
  config.psk = "THR33!!!";
  Portal.config(config);
  Portal.begin();
  */
}

void loop() {
  btn_last_state_blue_1 = btn_state_blue_1;
  btn_last_state_blue_2 = btn_state_blue_2;
  btn_last_state_green_1 = btn_state_green_1;
  btn_last_state_green_2 = btn_state_green_2;
  btn_last_state_red_1 = btn_state_red_1;
  btn_last_state_red_2 = btn_state_red_2;
  btn_state_blue_1 = digitalRead(btn_blue_1);
  btn_state_blue_2 = digitalRead(btn_blue_2);
  btn_state_green_1 = digitalRead(btn_green_1);
  btn_state_green_2 = digitalRead(btn_green_2);
  btn_state_red_1 = digitalRead(btn_red_1);
  btn_state_red_2 = digitalRead(btn_red_2);

  if (btn_last_state_blue_1 == 1 && btn_state_blue_1 == 0) {
    sendControlChange(60, led_blue_1);
  }
  if (btn_last_state_blue_2 == 1 && btn_state_blue_2 == 0) {
    sendControlChange(61, led_blue_2);
  }
  if (btn_last_state_green_1 == 1 && btn_state_green_1 == 0) {
    sendControlChange(62, led_green_1);
  }
  if (btn_last_state_green_2 == 1 && btn_state_green_2 == 0) {
    sendControlChange(63, led_green_2);
  }
  if (btn_last_state_red_1 == 1 && btn_state_red_1 == 0) {
    sendControlChange(64, led_red_1);
  }
  if (btn_last_state_red_2 == 1 && btn_state_red_2 == 0) {
    sendControlChange(65, led_red_2);
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
    drawFrownFace();

    blinkAllLEDs(10);
    vTaskDelay(10/portTICK_PERIOD_MS);
    blinkAllLEDs(10);

    vTaskDelay(500/portTICK_PERIOD_MS);
    clearMatrix();
  }
}

void blinkLEDBlue1(int ms) {
  digitalWrite(led_blue_1, HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_blue_1, LOW);
}

void blinkLEDBlue2(int ms) {
  digitalWrite(led_blue_2, HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_blue_2, LOW);
}

void blinkLEDGreen1(int ms) {
  digitalWrite(led_green_1, HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_green_1, LOW);
}

void blinkLEDGreen2(int ms) {
  digitalWrite(led_green_2, HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_green_2, LOW);
}

void blinkLEDRed1(int ms) {
  digitalWrite(led_red_1, HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_red_1, LOW);
}

void blinkLEDRed2(int ms) {
  digitalWrite(led_red_2, HIGH);
  vTaskDelay(ms/portTICK_PERIOD_MS);
  digitalWrite(led_red_2, LOW);
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

void drawSmileFace() {
  matrix.clear();
  matrix.drawBitmap(0, 0, smile_bmp, 8, 8, LED_ON);
  matrix.writeDisplay();
}

void drawNeutralFace() {
  matrix.clear();
  matrix.drawBitmap(0, 0, neutral_bmp, 8, 8, LED_ON);
  matrix.writeDisplay();
}

void drawFrownFace() {
  matrix.clear();
  matrix.drawBitmap(0, 0, frown_bmp, 8, 8, LED_ON);
  matrix.writeDisplay();
}

/*
void rootPage() {
  char content[] = "Three";
  Server.send(200, "text/plain", content);
}
*/
