# Heat Stress Guardian

A wearable that watches a worker's heat stress and warns them before it becomes
dangerous. Built on an Arduino UNO R4 WiFi, with a cloud AI that explains each
alert in plain English and a live supervisor dashboard.

```
 Sensors ──► Arduino (C++) ──► Firebase Realtime Database ◄── AI service (Python, GitHub Actions)
                                         │
                                         ▼
                              Dashboard (HTML, GitHub Pages)
```

## Folder layout

| Folder | Part | What it does |
| --- | --- | --- |
| `backend/arduino/HeatStressGuardian/` | Backend: device | Reads all sensors, works out the risk level, drives the buzzer, LED and OLED, uploads readings to Firebase every 5 s |
| `backend/ai-service/` | Backend: cloud | Every 5 minutes, reads the latest readings, asks Gemini (or Claude) to explain the risk, writes the answer back to Firebase |
| `backend/firebase/` | Backend: database | Security rules: only signed-in users can read or write |
| `frontend/` | Frontend | The supervisor dashboard: live status, AI explanation, sensor cards, trend chart, map, alert history |
| `.github/workflows/` | Automation | `heat-ai.yml` runs the AI service; `dashboard.yml` publishes the dashboard |

## Hardware

| Part | Pin |
| --- | --- |
| DHT11 (air temperature, humidity) | D2 |
| PIR HC-SR501 (motion) | D3 |
| LED (through 220 ohm) | D7 |
| Buzzer | D8 |
| LM35 (contact temperature) | A0 |
| GPS GY-NEO6MV2 (TX pin) | D0 (Serial1, 9600 baud) |
| MAX30102 heart rate (I2C 0x57) | SDA, SCL |
| MLX90614 IR skin temperature (I2C 0x5A) | SDA, SCL |
| SSD1306 OLED (I2C 0x3C) | SDA, SCL |

Risk levels: Safe below 32 °C heat index, Caution from 32 °C, Danger from 41 °C
or heart rate above 120 BPM.

## 1. Arduino (backend: device)

1. Install Arduino IDE 2 and the board package **Arduino UNO R4 Boards**.
2. Install these libraries from the Library Manager:
   DHT sensor library (Adafruit), Adafruit Unified Sensor, Adafruit SSD1306,
   Adafruit GFX, SparkFun MAX3010x Pulse and Proximity Sensor Library,
   Adafruit MLX90614, TinyGPSPlus, FirebaseClient (Mobizt).
3. In `backend/arduino/HeatStressGuardian/`, copy `secrets.example.h`,
   rename the copy to `secrets.h`, and fill in the Wi-Fi and Firebase values.
   `secrets.h` is in `.gitignore`, so it never goes to GitHub.
4. Open `HeatStressGuardian.ino`, pick the board and port, click Upload.
5. Open the Serial Monitor at 115200 baud to see the readings.

## 2. Firebase (backend: database)

1. Realtime Database > Rules: paste `backend/firebase/database.rules.json` and Publish.
2. Authentication > Users: add one user for the device and one for the supervisor.

## 3. AI service (backend: cloud)

In GitHub: **Settings > Secrets and variables > Actions**

| Secret | Value |
| --- | --- |
| FIREBASE_API_KEY | same as API_KEY in secrets.h |
| FIREBASE_EMAIL | same as USER_EMAIL in secrets.h |
| FIREBASE_PASSWORD | same as USER_PASSWORD in secrets.h |
| DATABASE_URL | same as DATABASE_URL in secrets.h |
| GEMINI_API_KEY | free key from aistudio.google.com |

Optional variable: `GEMINI_MODEL` = `gemini-2.5-flash`.

Then **Actions > Backend - Heat Stress AI > Run workflow** to test it. After
that it runs every 5 minutes by itself.

Test on a laptop without any keys: `python ai_service.py --demo`

## 4. Dashboard (frontend)

1. In `frontend/index.html`, fill in `firebaseConfig` (Firebase console >
   Project settings > General > Your apps > Web app).
2. GitHub: **Settings > Pages > Source: GitHub Actions**.
3. Push to `main`. The dashboard goes live at
   `https://YOUR-GITHUB-NAME.github.io/heat-stress-guardian/`
4. Sign in with the supervisor user.
5. Add `?demo`, `?demo=safe` or `?demo=danger` to the address to see sample data.
   Add `?theme=dark` or `?theme=industrial` to switch the look.

## Safety note

This is a student project. The thresholds are not medical advice. The buzzer
and LED on the device stay the main alert, so the worker is warned even
without Wi-Fi.
