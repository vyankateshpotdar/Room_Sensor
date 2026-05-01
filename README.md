# 🤖 DORM-OS v1.2

> **A smart hostel room assistant built on NodeMCU ESP8266 — monitors your room, controls your environment, and talks to you via Telegram with Grok AI.**

<p align="center">
  <img src="https://img.shields.io/badge/Platform-ESP8266-blue?logo=espressif" />
  <img src="https://img.shields.io/badge/AI-Grok%20(xAI)-black" />
  <img src="https://img.shields.io/badge/Notifications-Telegram-2CA5E0?logo=telegram" />
  <img src="https://img.shields.io/badge/Version-1.2-brightgreen" />
  <img src="https://img.shields.io/badge/Made%20at-MIT%20ADT%20Pune-orange" />
</p>

---

## 📸 Hardware

| NodeMCU ESP8266 | SSD1306 OLED 0.96" | DHT11 Sensor |
|:---:|:---:|:---:|
| ![NodeMCU](https://techiesms.com/wp-content/uploads/2019/09/NodeMCU.jpg) | ![OLED](https://m.media-amazon.com/images/I/51tPfIPzAKL._AC_SX679_.jpg) | ![DHT11](https://robu.in/wp-content/uploads/2015/07/dht11-module.jpg) |
| Brain of DORM-OS | 128×64 live display | Room temp & humidity |

> **Full BOM:** NodeMCU ESP8266 · SSD1306 I²C OLED · DHT11 · Red LED (D6/GPIO12) · Active Buzzer (D7/GPIO13)

---

## 5W1H — Everything You Need to Know

### 👤 WHO made this?
A hostel student at **MIT-ADT University, Pune** who was tired of not knowing if it was too hot to sleep, whether it would rain before class, or how many power cuts happened overnight — so JARVIS was born.

---

### 🤔 WHAT does it do?

DORM-OS is a **always-on room intelligence system** that runs 24/7 on a ₹300 NodeMCU. Here's what it packs:

#### 🌡️ Environment Monitoring
- Reads **room temperature & humidity** every 3 seconds via DHT11
- Tracks daily **max / min / average** temperature
- Computes **comfort index** and **heat index**
- Shows everything live on the OLED display

#### 🌤️ Live Weather (Open-Meteo API)
- Pulls **real outdoor weather** for Pune every 5 minutes — no API key needed
- Shows: condition, feels-like temp, rain %, UV index, wind speed, daily H/L
- **Auto-alerts** via Telegram for: rain >70%, UV >7, wind >40 km/h, storms, fog, heat stroke risk

#### 📱 Telegram Bot Control
Send commands from your phone, anywhere:

| Command | What it does |
|---------|-------------|
| `status` | Full room + weather + system report |
| `temp` | Quick temperature snapshot |
| `weather` | Detailed outdoor conditions |
| `alarm 7:30` | Set a wake-up alarm |
| `focus 25` | Start a 25-min Pomodoro timer |
| `sleep` / `wake` | Silence alerts for the night |
| `quiet` / `loud` | Toggle weather alerts |
| `note: buy milk` | Save a quick note (up to 5) |
| `report` | Today's room stats |
| `diagnostics` | System health check |
| `professional` / `chill` / `savage` | Switch JARVIS personality |
| *(anything else)* | Grok AI answers with room context |

#### 🧠 Grok AI (xAI)
- Any message that isn't a command goes to **Grok** with your room's current sensor data as context
- Grok knows your temperature, humidity, outdoor weather, and time of day before it answers
- Three personality modes: **chill**, **professional**, **savage**

#### 🔔 Smart Alerts (21 alert types)
DORM-OS watches for:
- Extreme heat / cold outside
- Room overheating (>32°C or >36°C)
- Night-time heat ("too hot to sleep")
- High humidity (>80%) — mold risk
- Low humidity (<20%) — dry air
- UV index danger (>7 and >10)
- Heavy rain, storms, fog
- Heat stroke / exhaustion risk (heat index)
- WiFi / sensor / API failures

Each alert has its own **cooldown** so you're never spammed.

#### 🔌 Power Cut Counter
- Uses **ESP8266 RTC memory** (survives power loss, not just reboots)
- Counts every hard power cut since midnight
- Reports count in Telegram and daily summary
- Auto-resets at 11 PM daily

#### ⏰ Alarm & Focus Timer
- Set alarms via Telegram — buzzer beeps + LED blinks + morning briefing sent to your phone
- Pomodoro-style focus timer with LED heartbeat blink; buzzes when done

#### 📊 Auto Reports
- **Daily at 11 PM** — room stats, outside summary, power cuts, sleep forecast
- **Weekly Sunday 9 AM** — weekly summary message

---

### 🕐 WHEN does it run?
All the time. DORM-OS is designed to run **24/7 on USB power** (phone charger works fine). It boots in ~3 seconds, connects to WiFi, and starts watching. Even if WiFi drops, it keeps reading sensors and showing data on the OLED.

---

### 📍 WHERE was it built?
**MIT-ADT University, Loni Kalbhor, Pune, Maharashtra** — hostel room, built on a breadboard during exam season.

The weather API is pre-configured for Pune (lat `18.4926`, lon `74.0255`). Change these two values in the code to use it anywhere.

---

### 💡 WHY build this?
Hostel life problems this solves:

| Problem | DORM-OS Solution |
|---------|----------------|
| Overslept — no alarm | Telegram alarm + buzzer |
| Rained, no umbrella | Rain alert >30 min before |
| Too hot to study | Room temp alert + fan reminder |
| Power cut tracking | RTC memory counter |
| Bored, need info | Ask JARVIS anything via Telegram |
| Forgotten tasks | In-device note storage |
| No weather app open | OLED shows weather 24/7 |

---

### 🛠️ HOW to set it up?

#### 1. Hardware Wiring

```
NodeMCU    →   Component
─────────────────────────
D2 (GPIO4) →   OLED SDA
D1 (GPIO5) →   OLED SCL
D4 (GPIO2) →   DHT11 DATA
D6 (GPIO12)→   Red LED (+ 220Ω to GND)
D7 (GPIO13)→   Buzzer +ve (GND to GND)
3.3V / GND →   All VCC / GND
```

#### 2. Arduino IDE Setup

Install these libraries via **Library Manager**:
- `Adafruit SSD1306`
- `Adafruit GFX`
- `DHT sensor library` (Adafruit)
- `ArduinoJson` (Benoit Blanchon)
- `NTPClient`
- `UniversalTelegramBot` (Brian Lough)
- `ESP8266WiFi` *(comes with ESP8266 board package)*

Board: **NodeMCU 1.0 (ESP-12E)** · Upload speed: 115200 · Flash: 4MB (FS:2MB)

#### 3. Configure `DORM_OS_v1_2.ino`

```cpp
// WiFi
const char* WIFI_SSID     = "YourWiFiName";
const char* WIFI_PASSWORD = "YourPassword";

// Telegram — get token from @BotFather, chat ID from @userinfobot
#define BOT_TOKEN  "your_bot_token"
#define CHAT_ID    "your_chat_id"

// Grok API — get key from console.x.ai
#define GROK_API_KEY  "xai-your-key-here"

// Location — change for your city
"?latitude=18.4926&longitude=74.0255"
```

#### 4. Flash & Go
Upload, open Serial Monitor at **115200 baud**, watch it connect and send the startup Telegram message. Done.

---

## 🔴 LED & Buzzer Signals

| Event | LED | Buzzer |
|-------|-----|--------|
| Telegram message sent OK | 2 quick blinks | — |
| Telegram send failed | 3 rapid blinks | — |
| **Internet / WiFi lost** | **Blinks for 60 seconds** | **3 beeps** |
| WiFi reconnected | Stops | — |
| Sensor failure | Continuous blink | — |
| Alarm ringing | Continuous blink | 3 beeps every 5s |
| Focus timer done | Stops | 3 beeps |
| Startup (online) | — | 3 short beeps |
| Startup (offline) | 3 blinks | 1 long beep |

---

## 📁 File Structure

```
DORM_OS_v1_2.ino   ← single-file Arduino sketch (all code here)
README.md          ← this file
```

---

## 🔄 Changelog

| Version | Changes |
|---------|---------|
| v1.0 | Initial release — DHT11, OLED, Telegram, Gemini AI |
| v1.1 | Switched AI to Grok · LED blinks replace Telegram beeps · RTC power cut counter |
| v1.2 | Internet lost → 3 buzzer beeps + 60-second red LED blink alert |

---

## ⚠️ Security Note
Before pushing to GitHub, **regenerate your Grok API key** at [console.x.ai](https://console.x.ai) and update your Telegram bot token. Never commit real credentials to a public repo — use a `secrets.h` file added to `.gitignore`.

---

## 📜 License
MIT — build it, break it, improve it.

---

<p align="center">Built with ☕ and sleep deprivation at MIT-ADT Pune</p>
