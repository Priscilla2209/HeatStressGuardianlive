// =====================================================
//  HEAT STRESS GUARDIAN  -  Phase 2 (GPS + IR + LED)
//  Board: Arduino UNO R4 WiFi
//
//  New in this version:
//   1. GPS (GY-NEO6MV2): location, satellites, sent to Firebase
//   2. IR temperature (MLX90614 / GY-906): skin or surface temperature
//   3. Alert LED on D7: blinks on CAUTION, stays on for DANGER
//   4. Raw pulse-sensor value in the Serial Monitor, to help testing
//
//  Extra libraries to install (Library Manager):
//   - TinyGPSPlus  (by Mikal Hart)
//   - Adafruit MLX90614 Library  (by Adafruit)
//
//  Wiring for the new parts:
//   GPS:      VCC -> 5V, GND -> GND, TX -> D0 (RX). Leave GPS RX empty.
//   MLX90614: VIN -> 5V, GND -> GND, SDA -> SDA row, SCL -> SCL row
//   LED:      D7 -> 220 ohm resistor -> long leg, short leg -> GND
// =====================================================

#include <Arduino.h>

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <WiFiS3.h>
#include <WiFiSSLClient.h>
#include <FirebaseClient.h>

#include <Wire.h>
#include <DHT.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MLX90614.h>
#include <TinyGPSPlus.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD, API_KEY, USER_EMAIL, USER_PASSWORD, DATABASE_URL

// -------- DEVICE --------

#define DEVICE_ID "worker01"

// -------- PINS --------

#define DHT_PIN 2
#define DHT_TYPE DHT11
#define PIR_PIN 3
#define LED_PIN 7
#define BUZZER_PIN 8
#define LM35_PIN A0
// GPS uses Serial1: its TX goes to D0 (RX)

// -------- ALERT THRESHOLDS (demonstration values, not medical advice) --------

const float HI_CAUTION = 32.0;   // heat index in C: caution starts here
const float HI_DANGER  = 41.0;   // heat index in C: danger starts here
const int   HR_HIGH    = 120;    // BPM: high heart rate in hot conditions raises the level

// -------- TIMING (milliseconds) --------

const unsigned long DHT_INTERVAL        = 2000;
const unsigned long DISPLAY_INTERVAL    = 500;
const unsigned long SERIAL_INTERVAL     = 1000;
const unsigned long UPLOAD_INTERVAL     = 5000;
const unsigned long HISTORY_INTERVAL    = 30000;
const unsigned long WIFI_RETRY_INTERVAL = 30000;
const unsigned long GPS_STALE_MS        = 5000;   // no new GPS fix for 5 s = lost

// -------- OBJECTS --------

DHT dht(DHT_PIN, DHT_TYPE);
MAX30105 particleSensor;
// The MLX90614 only supports 100 kHz I2C, so the screen also uses 100 kHz.
Adafruit_SSD1306 display(128, 64, &Wire, -1, 100000UL, 100000UL);
Adafruit_MLX90614 mlx;
TinyGPSPlus gps;

WiFiSSLClient ssl_client;

using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD);

FirebaseApp app;
RealtimeDatabase Database;

// -------- VARIABLES --------

float humidity = 0;
float airTemperature = 0;
float heatIndex = 0;
float lm35Temperature = 0;
float irObjectTemp = 0;          // MLX90614: temperature of what it points at
float irAmbientTemp = 0;         // MLX90614: temperature of the sensor itself

int motion = 0;
int heartRate = 0;
int riskLevel = 0;               // 0 = safe, 1 = caution, 2 = danger
long lastIR = 0;                 // raw pulse-sensor value, for testing

bool fingerDetected = false;
bool maxSensorReady = false;
bool oledReady = false;
bool irReady = false;
bool firebaseStarted = false;

// GPS
bool gpsFix = false;
double latitude = 0;
double longitude = 0;
int satellites = 0;

// Heart rate: the sensor gives 100 readings per second (400 / 4),
// so each reading is 10 ms apart. Beats are timed by counting readings.
const unsigned long SAMPLE_PERIOD_MS = 10;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte rateCount = 0;
unsigned long sampleCount = 0;
unsigned long lastBeatSample = 0;

unsigned long lastDht = 0;
unsigned long lastDisplay = 0;
unsigned long lastSerial = 0;
unsigned long lastUpload = 0;
unsigned long lastHistory = 0;
unsigned long lastWifiRetry = 0;

// -------- FIREBASE CALLBACK --------

void processData(AsyncResult &aResult) {

  if (aResult.isError()) {
    Serial.print("Firebase error: ");
    Serial.println(aResult.error().message());
  }
}

// -------- WIFI --------

void connectWiFi() {

  Serial.print("Connecting to WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startTime < 15000) {
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed! Alerts still work offline.");
  }
}

// -------- FIREBASE --------

void initializeFirebase() {

  Serial.println("Initializing Firebase...");

  initializeApp(aClient, app, getAuth(user_auth),
                processData, "authTask");

  app.getApp<RealtimeDatabase>(Database);

  Database.url(DATABASE_URL);

  firebaseStarted = true;
}

// Retry Wi-Fi in the background if the hotspot dropped.
void keepWiFiAlive() {

  if (WiFi.status() == WL_CONNECTED) {

    if (!firebaseStarted) initializeFirebase();
    return;
  }

  if (millis() - lastWifiRetry < WIFI_RETRY_INTERVAL) return;

  lastWifiRetry = millis();

  Serial.println("WiFi lost. Trying to reconnect...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// -------- GPS (runs on every loop, reads whatever has arrived) --------

void readGPS() {

  while (Serial1.available()) {
    gps.encode(Serial1.read());
  }

  // A fix counts only if the location is valid and recent.
  gpsFix = gps.location.isValid() && gps.location.age() < GPS_STALE_MS;

  if (gpsFix) {
    latitude = gps.location.lat();
    longitude = gps.location.lng();
  }

  satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
}

// -------- HEART RATE (runs on every loop, must stay fast) --------

void resetPulse() {

  fingerDetected = false;
  heartRate = 0;
  rateCount = 0;
  rateSpot = 0;
}

void readPulse() {

  if (!maxSensorReady) return;

  particleSensor.check();

  while (particleSensor.available()) {

    long irValue = particleSensor.getFIFOIR();
    particleSensor.nextSample();

    lastIR = irValue;
    sampleCount++;

    if (irValue < 50000) {
      resetPulse();
      lastBeatSample = sampleCount;
      continue;
    }

    fingerDetected = true;

    if (checkForBeat(irValue)) {

      unsigned long beatInterval =
        (sampleCount - lastBeatSample) * SAMPLE_PERIOD_MS;

      lastBeatSample = sampleCount;

      if (beatInterval > 0) {

        float newBPM = 60000.0 / beatInterval;

        if (newBPM > 40 && newBPM < 200) {

          rates[rateSpot] = (byte)newBPM;
          rateSpot = (rateSpot + 1) % RATE_SIZE;
          if (rateCount < RATE_SIZE) rateCount++;

          int total = 0;
          for (byte i = 0; i < rateCount; i++) total += rates[i];

          heartRate = total / rateCount;
        }
      }
    }

    // Finger is on but no beat for 3 seconds: clear the old value.
    if (sampleCount - lastBeatSample > 300) {
      heartRate = 0;
      rateCount = 0;
      rateSpot = 0;
    }
  }
}

// -------- SLOW SENSORS (every 2 seconds) --------

void readEnvironment() {

  // DHT11: air temperature and humidity
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (!isnan(h)) humidity = h;
  if (!isnan(t)) airTemperature = t;

  // Heat index combines temperature and humidity into one "feels like" value
  heatIndex = dht.computeHeatIndex(airTemperature, humidity, false);

  // LM35: average of 10 readings for a steadier value
  long total = 0;
  for (int i = 0; i < 10; i++) total += analogRead(LM35_PIN);

  float voltage = (total / 10.0) * (5000.0 / 1023.0);
  lm35Temperature = voltage / 10.0;

  // MLX90614: IR temperature
  if (irReady) {
    float obj = mlx.readObjectTempC();
    float amb = mlx.readAmbientTempC();
    if (!isnan(obj)) irObjectTemp = obj;
    if (!isnan(amb)) irAmbientTemp = amb;
  }

  // PIR
  motion = digitalRead(PIR_PIN);
}

// -------- RISK LEVEL --------

void evaluateRisk() {

  int level = 0;

  if (heatIndex >= HI_DANGER)       level = 2;
  else if (heatIndex >= HI_CAUTION) level = 1;

  // Hot conditions plus a high heart rate is treated as danger.
  if (level == 1 && fingerDetected && heartRate >= HR_HIGH) level = 2;

  riskLevel = level;
}

const char *riskText() {

  if (riskLevel == 2) return "DANGER";
  if (riskLevel == 1) return "CAUTION";
  return "SAFE";
}

// -------- BUZZER AND LED --------
// Safe: off.  Caution: short beep and blink every 2 seconds.  Danger: on.

void updateAlerts() {

  bool on;

  if (riskLevel == 2)      on = true;
  else if (riskLevel == 1) on = (millis() % 2000) < 200;
  else                     on = false;

  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

// -------- OLED DISPLAY (8 lines of 8 pixels) --------

void showDisplay() {

  if (!oledReady) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("HEAT STRESS GUARDIAN");

  display.setCursor(0, 9);
  display.print("Air ");
  display.print(airTemperature, 1);
  display.print("C  Hum ");
  display.print(humidity, 0);
  display.print("%");

  display.setCursor(0, 18);
  display.print("Heat index: ");
  display.print(heatIndex, 1);
  display.print("C");

  display.setCursor(0, 27);
  display.print("LM35 ");
  display.print(lm35Temperature, 1);
  display.print(" IR ");
  if (irReady) display.print(irObjectTemp, 1);
  else         display.print("--");

  display.setCursor(0, 36);
  display.print("Heart: ");
  if (!fingerDetected)      display.print("No finger");
  else if (heartRate == 0)  display.print("Reading...");
  else {
    display.print(heartRate);
    display.print(" BPM");
  }

  display.setCursor(0, 45);
  display.print("GPS: ");
  if (gpsFix) {
    display.print(latitude, 3);
    display.print(",");
    display.print(longitude, 3);
  } else {
    display.print("searching ");
    display.print(satellites);
    display.print(" sat");
  }

  display.setCursor(0, 55);
  display.print("Status: ");
  display.print(riskText());

  display.display();
}

// -------- SERIAL MONITOR (one line per second) --------

void showSerialData() {

  Serial.print("Air ");
  Serial.print(airTemperature, 1);
  Serial.print("C | Hum ");
  Serial.print(humidity, 0);
  Serial.print("% | HeatIdx ");
  Serial.print(heatIndex, 1);
  Serial.print("C | LM35 ");
  Serial.print(lm35Temperature, 1);
  Serial.print("C | IR ");
  if (irReady) { Serial.print(irObjectTemp, 1); Serial.print("C"); }
  else         Serial.print("off");
  Serial.print(" | Motion ");
  Serial.print(motion);
  Serial.print(" | Pulse raw ");
  Serial.print(maxSensorReady ? String(lastIR) : String("off"));
  Serial.print(" | Finger ");
  Serial.print(fingerDetected ? "yes" : "no");
  Serial.print(" | HR ");
  Serial.print(heartRate);
  Serial.print(" | GPS ");
  if (gpsFix) {
    Serial.print(latitude, 6);
    Serial.print(",");
    Serial.print(longitude, 6);
  } else {
    Serial.print("no fix");
  }
  Serial.print(" (");
  Serial.print(satellites);
  Serial.print(" sat) | ");
  Serial.print(riskText());
  Serial.print(" | WiFi ");
  Serial.print(WiFi.status() == WL_CONNECTED ? "ok" : "off");
  Serial.print(" | Firebase ");
  Serial.println(firebaseStarted && app.ready() ? "ready" : "not ready");
}

// -------- FIREBASE UPLOAD --------

String buildJson() {

  String json = "{";
  json += "\"deviceId\":\"" + String(DEVICE_ID) + "\",";
  json += "\"temperature\":" + String(airTemperature, 2) + ",";
  json += "\"humidity\":" + String(humidity, 2) + ",";
  json += "\"heatIndex\":" + String(heatIndex, 2) + ",";
  json += "\"lm35Temperature\":" + String(lm35Temperature, 2) + ",";
  if (irReady) {
    json += "\"irObjectTemp\":" + String(irObjectTemp, 2) + ",";
    json += "\"irAmbientTemp\":" + String(irAmbientTemp, 2) + ",";
  }
  json += "\"motion\":" + String(motion) + ",";
  json += "\"heartRate\":" + String(heartRate) + ",";
  json += "\"fingerDetected\":" + String(fingerDetected ? "true" : "false") + ",";
  json += "\"gpsFix\":" + String(gpsFix ? "true" : "false") + ",";
  json += "\"satellites\":" + String(satellites) + ",";
  if (gpsFix) {
    json += "\"latitude\":" + String(latitude, 6) + ",";
    json += "\"longitude\":" + String(longitude, 6) + ",";
  }
  json += "\"riskLevel\":" + String(riskLevel) + ",";
  json += "\"status\":\"" + String(riskText()) + "\",";
  json += "\"buzzer\":" + String(riskLevel > 0 ? "true" : "false") + ",";
  json += "\"timestamp\":{\".sv\":\"timestamp\"}";   // filled in by Firebase
  json += "}";

  return json;
}

void uploadFirebase() {

  if (WiFi.status() != WL_CONNECTED) return;
  if (!firebaseStarted || !app.ready()) return;

  String payload = buildJson();
  object_t json(payload);

  // Latest reading (overwritten each time, for the live dashboard)
  Database.set<object_t>(
    aClient,
    "/HeatStressGuardian/latest",
    json,
    processData,
    "latestUpload"
  );

  // History (a new record every 30 seconds, for trends)
  if (millis() - lastHistory >= HISTORY_INTERVAL) {

    lastHistory = millis();

    Database.push<object_t>(
      aClient,
      "/HeatStressGuardian/history",
      json,
      processData,
      "historyUpload"
    );
  }
}

// -------- SETUP --------

void setup() {

  Serial.begin(115200);
  delay(1500);

  Serial1.begin(9600);   // GPS (NEO-6M default speed)

  Wire.begin();

  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  analogReadResolution(10);

  dht.begin();

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not detected!");
    oledReady = false;
  } else {
    Serial.println("OLED detected!");
    oledReady = true;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 20);
    display.println("Heat Stress Guardian");
    display.setCursor(0, 36);
    display.println("Starting...");
    display.display();
  }

  // MAX30102
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {   // 100 kHz, shared with the MLX90614
    Serial.println("MAX30102 not detected!");
    maxSensorReady = false;
  } else {
    Serial.println("MAX30102 detected!");
    maxSensorReady = true;

    // brightness, average of 4, red + IR, 400 samples/s, pulse width, range
    // 400 / 4 = 100 readings per second
    particleSensor.setup(0x1F, 4, 2, 400, 411, 4096);
  }

  // MLX90614
  if (!mlx.begin()) {
    Serial.println("MLX90614 IR sensor not detected!");
    irReady = false;
  } else {
    Serial.println("MLX90614 IR sensor detected!");
    irReady = true;
  }

  Wire.setClock(100000);   // keep the shared I2C line at 100 kHz

  Serial.println("GPS started on D0 (RX). A first fix can take 1 to 5 minutes outdoors.");

  // WIFI AND FIREBASE
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    initializeFirebase();
  }
}

// -------- MAIN LOOP --------
// No delay() here. Each job runs on its own timer so the
// heart rate sensor and GPS are read without gaps.

void loop() {

  unsigned long now = millis();

  if (firebaseStarted) app.loop();

  readPulse();
  readGPS();

  if (now - lastDht >= DHT_INTERVAL) {
    lastDht = now;
    readEnvironment();
  }

  evaluateRisk();
  updateAlerts();

  if (now - lastDisplay >= DISPLAY_INTERVAL) {
    lastDisplay = now;
    showDisplay();
  }

  if (now - lastSerial >= SERIAL_INTERVAL) {
    lastSerial = now;
    showSerialData();
  }

  if (now - lastUpload >= UPLOAD_INTERVAL) {
    lastUpload = now;
    uploadFirebase();
  }

  keepWiFiAlive();
}
