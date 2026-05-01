// ════════════════════════════════════════════════════════
//  DORM-OS v1.0 — MIT ADT Pune
//  Hardware: NodeMCU ESP8266 + SSD1306 OLED 0.96"
//           + DHT11 + Red LED (D6/GPIO12) + Buzzer (D7/GPIO13)
//
//  Features:
//   • Jarvis AI via Telegram (Gemini 2.5 Flash)
//   • Weather via Open-Meteo API
//   • Smart alarm system (set via Telegram)
//   • Daily report at 11PM + Weekly report Sunday
//   • 2 OLED screens: Room + Outside
//   • Red LED alert blink engine
//   • Active-HIGH buzzer (idle LOW)
// ════════════════════════════════════════════════════════

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUDP.h>
#include <UniversalTelegramBot.h>

// ── Display ──────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── DHT11 ────────────────────────────────────────────────
#define DHTPIN   2    // D4
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// ── Hardware ─────────────────────────────────────────────
#define RED_LED   12   // D6
#define BUZZER    13   // D7 — active-HIGH, idle LOW

// ── WiFi credentials ─────────────────────────────────────
const char* WIFI_SSID     = "Manet Hostel@103";
const char* WIFI_PASSWORD = "MitHostel@103";

// ── Telegram ─────────────────────────────────────────────
#define BOT_TOKEN  "769ytFsBk"
#define CHAT_ID    "3"
WiFiClientSecure securedClient;
UniversalTelegramBot bot(BOT_TOKEN, securedClient);

// ── Gemini API ───────────────────────────────────────────
// Get free key at: aistudio.google.com
#define GEMINI_API_KEY  "AIzaSyDLuc3xX9w-H1hJz28L4f7w"
const char* GEMINI_HOST = "generativelanguage.googleapis.com";

// ── Open-Meteo API ───────────────────────────────────────
const char* WEATHER_URL =
  "http://api.open-meteo.com/v1/forecast"
  "?latitude=18.4926&longitude=74.0255"
  "&current_weather=true"
  "&hourly=temperature_2m,relativehumidity_2m,"
  "apparent_temperature,precipitation_probability,uv_index"
  "&daily=temperature_2m_max,temperature_2m_min,uv_index_max"
  "&timezone=Asia%2FKolkata"
  "&forecast_days=1";

// ── NTP (IST = UTC+5:30) ─────────────────────────────────
WiFiUDP   ntpUDP;
NTPClient ntp(ntpUDP, "pool.ntp.org", 19800);

// ════════════════════════════════════════════════════════
//  DATA STRUCTURES
// ════════════════════════════════════════════════════════
struct OutsideData {
  float  temp       = 0;
  float  feelsLike  = 0;
  float  high       = 0;
  float  low        = 0;
  float  wind       = 0;
  int    humidity   = 0;
  int    rainChance = 0;
  int    code       = 0;
  float  uvMax      = 0;
  String condition  = "---";
  bool   ready      = false;
};

struct RoomData {
  float temp     = 0;
  float hum      = 0;
  float maxTemp  = -99;
  float minTemp  = 99;
  float sumTemp  = 0;
  int   readings = 0;
};

struct AlarmData {
  bool   set      = false;
  int    hour     = 0;
  int    minute   = 0;
  bool   ringing  = false;
  unsigned long ringStartMs = 0;
};

OutsideData out;
RoomData    room;
AlarmData   alarm;

// ── Notes (stored in RAM, up to 5) ──────────────────────
#define MAX_NOTES 5
String notes[MAX_NOTES];
int noteCount = 0;

// ── Daily tracking ───────────────────────────────────────
int   studyMinutes    = 0;
int   powerCuts       = 0;
bool  reportSentToday = false;
int   lastReportDay   = -1;
bool  weeklyReportSentThisWeek = false;
int   lastWeeklyDay   = -1;

// ── Jarvis mode ──────────────────────────────────────────
bool   sleepMode  = false;
bool   quietMode  = false;
bool   focusMode  = false;
int    focusMins  = 25;
unsigned long focusStartMs = 0;

String jarvisPersonality = "chill"; // chill / professional / savage

// ════════════════════════════════════════════════════════
//  TIMERS
// ════════════════════════════════════════════════════════
unsigned long lastSensorMs   = 0;
unsigned long lastFetchMs    = 0;
unsigned long lastScreenMs   = 0;
unsigned long lastBotCheckMs = 0;
unsigned long fetchedAtMs    = 0;
int           currentScreen  = 0;

#define SENSOR_MS    3000UL
#define FETCH_MS     300000UL
#define SCREEN_MS    8000UL
#define BOT_CHECK_MS 2000UL
#define NUM_SCREENS  2

// ════════════════════════════════════════════════════════
//  BUZZER (active-HIGH, idle LOW)
// ════════════════════════════════════════════════════════
void beep(int count, int onMs = 80, int offMs = 80) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(onMs);
    digitalWrite(BUZZER, LOW);
    if (i < count - 1) delay(offMs);
  }
  digitalWrite(BUZZER, LOW);
}

// ════════════════════════════════════════════════════════
//  RED LED BLINK ENGINE
// ════════════════════════════════════════════════════════
struct BlinkJob {
  int           pin;
  int           totalPairs;
  int           pairsDone;
  unsigned long periodMs;
  bool          continuous;
  bool          active;
  bool          ledOn;
  unsigned long lastToggleMs;
};
BlinkJob blk = {0,0,0,0,false,false,false,0};

void startBlink(int pin, int count, unsigned long period, bool continuous = false) {
  if (blk.active && blk.ledOn) digitalWrite(blk.pin, LOW);
  blk.pin          = pin;
  blk.totalPairs   = count;
  blk.pairsDone    = 0;
  blk.periodMs     = period;
  blk.continuous   = continuous;
  blk.active       = true;
  blk.ledOn        = false;
  blk.lastToggleMs = millis();
}

void stopBlink() {
  if (blk.active && blk.ledOn) digitalWrite(blk.pin, LOW);
  blk.active = false;
}

void updateBlink() {
  if (!blk.active) return;
  if (millis() - blk.lastToggleMs < blk.periodMs) return;
  blk.lastToggleMs = millis();
  blk.ledOn = !blk.ledOn;
  digitalWrite(blk.pin, blk.ledOn ? HIGH : LOW);
  if (!blk.ledOn) {
    blk.pairsDone++;
    if (!blk.continuous && blk.pairsDone >= blk.totalPairs) blk.active = false;
    if ( blk.continuous && blk.pairsDone >= blk.totalPairs) blk.pairsDone = 0;
  }
}

// ════════════════════════════════════════════════════════
//  ALERT IDs
// ════════════════════════════════════════════════════════
#define A_RAIN90     0
#define A_RAIN70     1
#define A_UV10       2
#define A_UV7        3
#define A_TEMP38     4
#define A_TEMP10     5
#define A_WIND60     6
#define A_WIND40     7
#define A_STORM      8
#define A_FOG        9
#define A_ROOM_HOT36 10
#define A_ROOM_HOT32 11
#define A_ROOM_RISE  12
#define A_HUM_HIGH   13
#define A_HUM_LOW    14
#define A_NIGHT_HOT  15
#define A_HI_45      16
#define A_HI_40      17
#define A_WIFI_LOST  18
#define A_SENSOR_FAIL 19
#define A_API_FAIL   20
#define NUM_ALERTS   21

const unsigned long CD[NUM_ALERTS] = {
  3600000UL, 10800000UL, 3600000UL, 10800000UL,
  10800000UL, 10800000UL, 3600000UL, 10800000UL,
  3600000UL, 10800000UL, 3600000UL, 7200000UL,
  1800000UL, 1800000UL, 1800000UL, 7200000UL,
  3600000UL, 7200000UL, 0UL, 0UL, 0UL
};

unsigned long alertSentAt[NUM_ALERTS];
bool          alertActive[NUM_ALERTS];

bool canAlert(int id) { return (millis() - alertSentAt[id]) >= CD[id]; }
void markAlert(int id) { alertSentAt[id] = millis(); alertActive[id] = true; }

// ════════════════════════════════════════════════════════
//  HELPERS
// ════════════════════════════════════════════════════════
String wmoShort(int code) {
  if (code == 0)                return "CLEAR SKY";
  if (code == 1)                return "MAINLY CLEAR";
  if (code == 2)                return "PARTLY CLOUDY";
  if (code == 3)                return "OVERCAST";
  if (code == 45 || code == 48) return "FOGGY";
  if (code >= 51 && code <= 55) return "DRIZZLE";
  if (code >= 61 && code <= 65) return "RAINY";
  if (code >= 71 && code <= 75) return "SNOWY";
  if (code >= 80 && code <= 82) return "SHOWERS";
  if (code >= 95 && code <= 99) return "STORM";
  return "UNKNOWN";
}

float heatIdx(float t, float h) {
  return t + 0.33f * (h / 100.0f * 6.105f *
         exp(17.27f * t / (237.7f + t))) - 4.0f;
}

int comfort(float t, float h) {
  if (t >= 20 && t <= 28 && h >= 40 && h <= 60) return 0;
  if (t > 34 || h > 80 || h < 20)               return 2;
  return 1;
}
const char* comfortStr(int c) {
  if (c == 0) return "GOOD";
  if (c == 1) return "FAIR";
  return "POOR";
}

String comfortEmoji(int c) {
  if (c == 0) return "✅";
  if (c == 1) return "⚠️";
  return "🔴";
}

// ════════════════════════════════════════════════════════
//  TELEGRAM SEND
// ════════════════════════════════════════════════════════
void tgSend(const String& msg) {
  if (WiFi.status() != WL_CONNECTED) {
    startBlink(RED_LED, 1, 300);
    return;
  }
  bool ok = bot.sendMessage(CHAT_ID, msg, "");
  if (ok) beep(1, 60);
  else    startBlink(RED_LED, 1, 200);
  Serial.println(ok ? "[TG] OK: " + msg : "[TG] FAIL");
}

// ════════════════════════════════════════════════════════
//  GEMINI AI
// ════════════════════════════════════════════════════════
String askGemini(String userMsg) {
  if (WiFi.status() != WL_CONNECTED) return "I'm offline right now.";
  if (String(GEMINI_API_KEY) == "YOUR_GEMINI_API_KEY_HERE") {
    return "Gemini API key not set. Add it in the code at GEMINI_API_KEY.";
  }

  // Build sensor context
  String context = "You are JARVIS, a smart hostel room assistant built on a NodeMCU ESP8266. ";
  context += "Personality: " + jarvisPersonality + ". ";
  context += "Current data: ";
  context += "Room " + String(room.temp, 1) + "°C, ";
  context += "Humidity " + String(room.hum, 0) + "%, ";
  context += "Comfort " + String(comfortStr(comfort(room.temp, room.hum))) + ". ";
  if (out.ready) {
    context += "Outside " + String(out.temp, 1) + "°C, ";
    context += out.condition + ", ";
    context += "Rain " + String(out.rainChance) + "%, ";
    context += "UV " + String(out.uvMax, 0) + ", ";
    context += "Wind " + String(out.wind, 0) + " km/h. ";
  }
  ntp.update();
  context += "Time: " + String(ntp.getHours()) + ":" + (ntp.getMinutes() < 10 ? "0" : "") + String(ntp.getMinutes()) + " IST. ";
  context += "Keep reply under 3 sentences, plain text, no markdown.";

  // Build JSON payload
  String payload = "{\"contents\":[{\"parts\":[{\"text\":\"";
  String combined = context + " User says: " + userMsg;
  combined.replace("\"", "'");
  payload += combined;
  payload += "\"}]}],\"generationConfig\":{\"maxOutputTokens\":150}}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://";
  url += GEMINI_HOST;
  url += "/v1beta/models/gemini-2.0-flash:generateContent?key=";
  url += GEMINI_API_KEY;

  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");
  https.setTimeout(15000);

  int code = https.POST(payload);
  String reply = "I couldn't reach Gemini right now.";

  if (code == 200) {
    String response = https.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, response);
    if (!err) {
      reply = doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
      reply.trim();
    }
  } else {
    Serial.printf("[Gemini] HTTP %d\n", code);
  }
  https.end();
  return reply;
}

// ════════════════════════════════════════════════════════
//  WIFI RECONNECT
// ════════════════════════════════════════════════════════
bool          wifiWasUp       = true;
unsigned long wifiLostAtMs    = 0;
unsigned long lastReconnectMs = 0;

void wifiLoop() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasUp) {
      wifiWasUp    = false;
      wifiLostAtMs = now;
      Serial.println("[WiFi] Lost");
      if (!alertActive[A_WIFI_LOST]) {
        tgSend("📡 DORM-OS offline — WiFi lost.");
        startBlink(RED_LED, 0, 600, true);
        markAlert(A_WIFI_LOST);
      }
    }
    if (now - lastReconnectMs >= 30000UL) {
      lastReconnectMs = now;
      if (now - wifiLostAtMs > 120000UL) {
        WiFi.disconnect(true); delay(500);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      } else {
        WiFi.reconnect();
      }
    }
  } else {
    if (!wifiWasUp) {
      wifiWasUp = true;
      stopBlink();
      beep(2, 100, 60);
      powerCuts++;
      if (alertActive[A_WIFI_LOST]) {
        tgSend("✅ DORM-OS back online — " + WiFi.localIP().toString());
        alertActive[A_WIFI_LOST] = false;
      }
    }
  }
}

// ════════════════════════════════════════════════════════
//  FETCH WEATHER
// ════════════════════════════════════════════════════════
int apiFails = 0;

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  http.begin(client, WEATHER_URL);
  http.setTimeout(12000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> filter;
    filter["current_weather"]["temperature"] = true;
    filter["current_weather"]["windspeed"]   = true;
    filter["current_weather"]["weathercode"] = true;
    filter["daily"]["temperature_2m_max"]    = true;
    filter["daily"]["temperature_2m_min"]    = true;
    filter["daily"]["uv_index_max"]          = true;
    filter["hourly"]["relativehumidity_2m"]  = true;
    filter["hourly"]["apparent_temperature"] = true;
    filter["hourly"]["precipitation_probability"] = true;

    DynamicJsonDocument doc(3072);
    DeserializationError err = deserializeJson(
        doc, payload, DeserializationOption::Filter(filter));

    if (!err) {
      out.temp      = doc["current_weather"]["temperature"].as<float>();
      out.wind      = doc["current_weather"]["windspeed"].as<float>();
      out.code      = doc["current_weather"]["weathercode"].as<int>();
      out.condition = wmoShort(out.code);
      out.high      = doc["daily"]["temperature_2m_max"][0].as<float>();
      out.low       = doc["daily"]["temperature_2m_min"][0].as<float>();
      out.uvMax     = doc["daily"]["uv_index_max"][0].as<float>();
      ntp.update();
      int h          = ntp.getHours();
      out.humidity   = doc["hourly"]["relativehumidity_2m"][h].as<int>();
      out.feelsLike  = doc["hourly"]["apparent_temperature"][h].as<float>();
      out.rainChance = doc["hourly"]["precipitation_probability"][h].as<int>();
      out.ready   = true;
      fetchedAtMs = millis();
      apiFails    = 0;
      alertActive[A_API_FAIL] = false;
    } else { apiFails++; }
  } else { apiFails++; }
  http.end();

  if (apiFails >= 3 && !alertActive[A_API_FAIL]) {
    tgSend("🌐 Weather API unreachable.");
    startBlink(RED_LED, 3, 300);
    markAlert(A_API_FAIL);
  }
}

// ════════════════════════════════════════════════════════
//  CHECK WEATHER ALERTS
// ════════════════════════════════════════════════════════
void checkAlerts() {
  if (!out.ready || quietMode || sleepMode) return;

  if (out.rainChance > 90 && canAlert(A_RAIN90)) {
    tgSend("🌧 Heavy rain " + String(out.rainChance) + "% — avoid travel.");
    startBlink(RED_LED, 3, 300); markAlert(A_RAIN90);
  } else if (out.rainChance > 70 && canAlert(A_RAIN70)) {
    tgSend("☔ Rain likely " + String(out.rainChance) + "% — carry umbrella.");
    startBlink(RED_LED, 2, 400); markAlert(A_RAIN70);
  }
  if (out.uvMax >= 10 && canAlert(A_UV10)) {
    tgSend("☠️ Extreme UV " + String(out.uvMax, 0) + " — stay indoors.");
    startBlink(RED_LED, 5, 100); markAlert(A_UV10);
  } else if (out.uvMax >= 7 && canAlert(A_UV7)) {
    tgSend("🔆 High UV " + String(out.uvMax, 0) + " — avoid 11AM-3PM.");
    startBlink(RED_LED, 3, 200); markAlert(A_UV7);
  }
  if (out.temp > 38 && canAlert(A_TEMP38)) {
    tgSend("🔥 Extreme heat " + String(out.temp, 1) + "°C outside.");
    startBlink(RED_LED, 5, 100); markAlert(A_TEMP38);
  }
  if (out.temp < 10 && canAlert(A_TEMP10)) {
    tgSend("🥶 Unusually cold " + String(out.temp, 1) + "°C outside.");
    startBlink(RED_LED, 2, 400); markAlert(A_TEMP10);
  }
  if (out.wind > 60 && canAlert(A_WIND60)) {
    tgSend("🌪️ Dangerous wind " + String(out.wind, 0) + " km/h.");
    startBlink(RED_LED, 5, 100); markAlert(A_WIND60);
  } else if (out.wind > 40 && canAlert(A_WIND40)) {
    tgSend("💨 Strong wind " + String(out.wind, 0) + " km/h.");
    startBlink(RED_LED, 3, 400); markAlert(A_WIND40);
  }
  if (out.code >= 95 && canAlert(A_STORM)) {
    tgSend("⛈️ Storm active — stay inside, close windows.");
    startBlink(RED_LED, 0, 500, true); markAlert(A_STORM);
  }
  if ((out.code == 45 || out.code == 48) && canAlert(A_FOG)) {
    tgSend("🌫️ Foggy — drive carefully.");
    startBlink(RED_LED, 2, 400); markAlert(A_FOG);
  }
  if (room.temp > 36 && canAlert(A_ROOM_HOT36)) {
    tgSend("🚨 Room dangerously hot — " + String(room.temp, 1) + "°C!");
    startBlink(RED_LED, 5, 100); markAlert(A_ROOM_HOT36);
  } else if (room.temp > 32 && canAlert(A_ROOM_HOT32)) {
    tgSend("🌡️ Room hot — " + String(room.temp, 1) + "°C. Turn on fan.");
    startBlink(RED_LED, 3, 400); markAlert(A_ROOM_HOT32);
  }
  if (room.hum > 80 && canAlert(A_HUM_HIGH)) {
    tgSend("💧 High humidity " + String(room.hum, 0) + "% — mold risk.");
    startBlink(RED_LED, 2, 400); markAlert(A_HUM_HIGH);
  }
  if (room.hum < 20 && canAlert(A_HUM_LOW)) {
    tgSend("🏜️ Air too dry — " + String(room.hum, 0) + "%.");
    markAlert(A_HUM_LOW);
  }
  ntp.update();
  int nowH = ntp.getHours();
  if ((nowH >= 22 || nowH < 6) && room.temp > 30 && canAlert(A_NIGHT_HOT)) {
    tgSend("😴 Too hot to sleep — room " + String(room.temp, 1) + "°C.");
    startBlink(RED_LED, 3, 400); markAlert(A_NIGHT_HOT);
  }
  float hi = heatIdx(out.temp, out.humidity);
  if (hi >= 45 && canAlert(A_HI_45)) {
    tgSend("🚨 Heat stroke danger — heat index " + String(hi, 0) + "°C!");
    startBlink(RED_LED, 5, 100); markAlert(A_HI_45);
  } else if (hi >= 40 && canAlert(A_HI_40)) {
    tgSend("🏥 Heat exhaustion risk — index " + String(hi, 0) + "°C.");
    startBlink(RED_LED, 4, 150); markAlert(A_HI_40);
  }
}

// ════════════════════════════════════════════════════════
//  DAILY REPORT
// ════════════════════════════════════════════════════════
void sendDailyReport() {
  ntp.update();
  int h = ntp.getHours();
  int d = ntp.getDay(); // 0=Sun

  // Daily at 11PM
  if (h == 23 && !reportSentToday) {
    String msg = "📊 *Daily Report*\n\n";
    msg += "🌡️ Room\n";
    if (room.readings > 0) {
      msg += "Max: " + String(room.maxTemp, 1) + "°C\n";
      msg += "Min: " + String(room.minTemp, 1) + "°C\n";
      msg += "Avg: " + String(room.sumTemp / room.readings, 1) + "°C\n";
      msg += "Humidity avg: " + String(room.hum, 0) + "%\n";
    }
    if (out.ready) {
      msg += "\n🌤️ Outside\n";
      msg += "High: " + String(out.high, 0) + "°C  Low: " + String(out.low, 0) + "°C\n";
      msg += "Condition: " + out.condition + "\n";
    }
    msg += "\n🔌 Power cuts today: " + String(powerCuts) + "\n";
    msg += "Uptime: " + String(millis() / 3600000UL) + " hrs\n";
    msg += "\n😴 Sleep forecast: ";
    if (room.temp > 30 || room.hum > 70)
      msg += "Poor — room too warm/humid";
    else
      msg += "Good — conditions ok";

    tgSend(msg);
    reportSentToday = true;
    lastReportDay   = d;

    // Reset daily stats
    room.maxTemp  = -99;
    room.minTemp  = 99;
    room.sumTemp  = 0;
    room.readings = 0;
    powerCuts     = 0;
  }

  if (h == 0 && reportSentToday && lastReportDay != d) {
    reportSentToday = false;
  }

  // Weekly report every Sunday at 9AM
  if (d == 0 && h == 9 && !weeklyReportSentThisWeek) {
    String msg = "📈 *Weekly Summary*\n\n";
    msg += "DORM-OS has been running " + String(millis() / 86400000UL) + "+ days.\n";
    msg += "Current room: " + String(room.temp, 1) + "°C, " + String(room.hum, 0) + "% hum\n";
    if (out.ready) msg += "Outside: " + String(out.temp, 1) + "°C — " + out.condition + "\n";
    msg += "\nHave a great week! 💪";
    tgSend(msg);
    weeklyReportSentThisWeek = true;
    lastWeeklyDay = d;
  }
  if (d != 0 && weeklyReportSentThisWeek) {
    weeklyReportSentThisWeek = false;
  }
}

// ════════════════════════════════════════════════════════
//  ALARM CHECK
// ════════════════════════════════════════════════════════
int sensorFails = 0;

void checkAlarm() {
  if (!alarm.set) return;
  ntp.update();
  int h = ntp.getHours();
  int m = ntp.getMinutes();

  if (!alarm.ringing && h == alarm.hour && m == alarm.minute) {
    alarm.ringing    = true;
    alarm.ringStartMs = millis();
    tgSend("⏰ WAKE UP! It's " + String(h) + ":" + (m < 10 ? "0" : "") + String(m) +
           "\nRoom: " + String(room.temp, 1) + "°C  " + String(room.hum, 0) + "% hum" +
           (out.ready ? "\nOutside: " + String(out.temp, 1) + "°C — " + out.condition : "") +
           "\n\nReply *stop* to silence.");
    startBlink(RED_LED, 0, 400, true);
  }

  if (alarm.ringing) {
    // Buzz every 5 seconds
    if ((millis() - alarm.ringStartMs) % 5000 < 200) {
      beep(3, 150, 80);
    }
    // Auto-stop after 5 minutes
    if (millis() - alarm.ringStartMs > 300000UL) {
      alarm.ringing = false;
      alarm.set     = false;
      stopBlink();
      tgSend("⏰ Alarm auto-stopped after 5 minutes.");
    }
  }
}

// ════════════════════════════════════════════════════════
//  FOCUS TIMER
// ════════════════════════════════════════════════════════
void checkFocusTimer() {
  if (!focusMode) return;
  unsigned long elapsed = (millis() - focusStartMs) / 60000UL;
  unsigned long remaining = focusMins - elapsed;

  if (elapsed >= (unsigned long)focusMins) {
    focusMode = false;
    stopBlink();
    beep(3, 200, 100);
    tgSend("🍅 Focus session done! " + String(focusMins) + " mins complete. Take a break.");
    studyMinutes += focusMins;
  }
}

// ════════════════════════════════════════════════════════
//  PROCESS TELEGRAM COMMANDS
// ════════════════════════════════════════════════════════
String buildStatusMsg() {
  ntp.update();
  String msg = "📍 *DORM-OS Status*\n";
  msg += "🕐 " + String(ntp.getHours()) + ":" + (ntp.getMinutes()<10?"0":"") + String(ntp.getMinutes()) + " IST\n\n";

  msg += "🏠 *Room*\n";
  msg += "Temp: " + String(room.temp, 1) + "°C\n";
  msg += "Humidity: " + String(room.hum, 0) + "%\n";
  msg += "Comfort: " + comfortEmoji(comfort(room.temp, room.hum)) + " " + comfortStr(comfort(room.temp, room.hum)) + "\n";
  msg += "Heat index: " + String(heatIdx(room.temp, room.hum), 1) + "°C\n";

  if (out.ready) {
    msg += "\n🌤️ *Outside*\n";
    msg += "Temp: " + String(out.temp, 1) + "°C (feels " + String(out.feelsLike, 0) + "°C)\n";
    msg += "Condition: " + out.condition + "\n";
    msg += "Rain: " + String(out.rainChance) + "%\n";
    msg += "UV: " + String(out.uvMax, 0) + "\n";
    msg += "Wind: " + String(out.wind, 0) + " km/h\n";
    msg += "H:" + String(out.high, 0) + "° / L:" + String(out.low, 0) + "°\n";
  } else {
    msg += "\n🌤️ Weather: fetching...\n";
  }

  msg += "\n⚙️ *System*\n";
  msg += "Uptime: " + String(millis()/3600000UL) + "h " + String((millis()%3600000UL)/60000UL) + "m\n";
  msg += "WiFi: " + WiFi.localIP().toString() + "\n";
  msg += "Mode: " + String(sleepMode?"Sleep":focusMode?"Focus":"Normal") + "\n";
  if (alarm.set) {
    msg += "Alarm: " + String(alarm.hour) + ":" + (alarm.minute<10?"0":"") + String(alarm.minute) + "\n";
  }
  return msg;
}

void handleCommand(String text) {
  text.trim();
  String lower = text;
  lower.toLowerCase();

  Serial.println("[CMD] " + text);
  beep(1, 40);

  // ── STOP ALARM ──────────────────────────────────────
  if (lower == "stop") {
    if (alarm.ringing) {
      alarm.ringing = false;
      alarm.set     = false;
      stopBlink();
      tgSend("✅ Alarm stopped. Good morning!");
    } else {
      tgSend("No alarm ringing right now.");
    }
    return;
  }

  // ── STATUS ───────────────────────────────────────────
  if (lower == "status" || lower == "s") {
    tgSend(buildStatusMsg()); return;
  }

  // ── TEMP ─────────────────────────────────────────────
  if (lower == "temp" || lower == "temperature") {
    tgSend("🌡️ Room: " + String(room.temp, 1) + "°C  |  Outside: " +
           (out.ready ? String(out.temp, 1) + "°C" : "---"));
    return;
  }

  // ── WEATHER ──────────────────────────────────────────
  if (lower == "weather" || lower == "w") {
    if (!out.ready) { tgSend("🌤️ Fetching weather..."); fetchWeather(); }
    if (out.ready) {
      String msg = "🌤️ *Outside Weather*\n";
      msg += out.condition + "\n";
      msg += "Temp: " + String(out.temp, 1) + "°C (feels " + String(out.feelsLike, 0) + "°C)\n";
      msg += "Rain: " + String(out.rainChance) + "% | UV: " + String(out.uvMax, 0) + "\n";
      msg += "Wind: " + String(out.wind, 0) + " km/h\n";
      msg += "H:" + String(out.high, 0) + "° / L:" + String(out.low, 0) + "°";
      tgSend(msg);
    }
    return;
  }

  // ── ALARM ────────────────────────────────────────────
  if (lower.startsWith("alarm ")) {
    String timeStr = text.substring(6);
    timeStr.trim();
    int colonIdx = timeStr.indexOf(':');
    if (colonIdx > 0) {
      alarm.hour   = timeStr.substring(0, colonIdx).toInt();
      alarm.minute = timeStr.substring(colonIdx + 1).toInt();
      alarm.set    = true;
      alarm.ringing = false;
      tgSend("⏰ Alarm set for " + String(alarm.hour) + ":" +
             (alarm.minute < 10 ? "0" : "") + String(alarm.minute));
    } else {
      tgSend("Format: alarm 7:30");
    }
    return;
  }

  // ── FOCUS TIMER ──────────────────────────────────────
  if (lower.startsWith("focus")) {
    String numStr = lower.substring(5);
    numStr.trim();
    focusMins = numStr.length() > 0 ? numStr.toInt() : 25;
    if (focusMins <= 0) focusMins = 25;
    focusMode    = true;
    focusStartMs = millis();
    startBlink(RED_LED, 0, 1500, true); // slow pulse during focus
    tgSend("🍅 Focus mode ON — " + String(focusMins) + " min session started. GO!");
    return;
  }

  // ── SLEEP MODE ───────────────────────────────────────
  if (lower == "sleep") {
    sleepMode = true; quietMode = true;
    stopBlink();
    ntp.update();
    tgSend("🌙 Sleep mode ON. Alerts silenced. Good night!\n(Send *wake* to disable)");
    return;
  }

  if (lower == "wake") {
    sleepMode = false; quietMode = false;
    ntp.update();
    String msg = "🌅 Good morning!\n";
    msg += "Room: " + String(room.temp, 1) + "°C  " + String(room.hum, 0) + "%\n";
    if (out.ready) msg += "Outside: " + String(out.temp, 1) + "°C — " + out.condition;
    tgSend(msg);
    return;
  }

  // ── QUIET MODE ───────────────────────────────────────
  if (lower == "quiet") {
    quietMode = true;
    tgSend("🔕 Quiet mode ON. Weather alerts silenced.");
    return;
  }
  if (lower == "loud" || lower == "unmute") {
    quietMode = false;
    tgSend("🔔 Alerts re-enabled.");
    return;
  }

  // ── FOCUS STOP ───────────────────────────────────────
  if (lower == "break" || lower == "stopfocus") {
    focusMode = false; stopBlink();
    tgSend("⏸ Focus stopped. Take a break!");
    return;
  }

  // ── REPORT ───────────────────────────────────────────
  if (lower == "report" || lower == "daily") {
    String msg = "📊 *Today So Far*\n";
    if (room.readings > 0) {
      msg += "Room max: " + String(room.maxTemp, 1) + "°C\n";
      msg += "Room min: " + String(room.minTemp, 1) + "°C\n";
    }
    if (out.ready) {
      msg += "Outside H:" + String(out.high, 0) + "° L:" + String(out.low, 0) + "°\n";
    }
    msg += "Uptime: " + String(millis()/3600000UL) + "h\n";
    msg += "Power cuts: " + String(powerCuts);
    tgSend(msg);
    return;
  }

  // ── NOTES ────────────────────────────────────────────
  if (lower.startsWith("note:") || lower.startsWith("note ")) {
    String noteText = text.substring(5);
    noteText.trim();
    if (noteCount < MAX_NOTES) {
      notes[noteCount++] = noteText;
      tgSend("📝 Noted: " + noteText);
    } else {
      tgSend("📝 Notes full (max 5). Use *clearnotes* first.");
    }
    return;
  }

  if (lower == "notes") {
    if (noteCount == 0) { tgSend("📝 No notes saved."); return; }
    String msg = "📝 *Your Notes*\n";
    for (int i = 0; i < noteCount; i++) {
      msg += String(i + 1) + ". " + notes[i] + "\n";
    }
    tgSend(msg);
    return;
  }

  if (lower == "clearnotes") {
    noteCount = 0;
    tgSend("📝 Notes cleared.");
    return;
  }

  // ── PERSONALITY ──────────────────────────────────────
  if (lower == "professional mode" || lower == "professional") {
    jarvisPersonality = "professional";
    tgSend("🎩 Professional mode activated.");
    return;
  }
  if (lower == "chill mode" || lower == "chill") {
    jarvisPersonality = "chill";
    tgSend("😎 Chill mode on bro.");
    return;
  }
  if (lower == "savage mode" || lower == "savage") {
    jarvisPersonality = "savage";
    tgSend("😈 Savage mode activated. Brace yourself.");
    return;
  }

  // ── DIAGNOSTICS ──────────────────────────────────────
  if (lower == "diagnostics" || lower == "diag") {
    String msg = "🔧 *System Diagnostics*\n";
    msg += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? "✅ " + WiFi.localIP().toString() : "❌ Offline") + "\n";
    msg += "Sensor: " + String(sensorFails == 0 ? "✅ OK" : "❌ " + String(sensorFails) + " fails") + "\n";
    msg += "Weather API: " + String(out.ready ? "✅ OK" : "❌ No data") + "\n";
    msg += "Uptime: " + String(millis()/3600000UL) + "h " + String((millis()%3600000UL)/60000UL) + "m\n";
    msg += "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    msg += "Personality: " + jarvisPersonality;
    tgSend(msg);
    return;
  }

  // ── HELP ─────────────────────────────────────────────
  if (lower == "help" || lower == "?") {
    String msg = "🤖 *DORM-OS Commands*\n\n";
    msg += "status / s — full room report\n";
    msg += "temp — current temperature\n";
    msg += "weather / w — outside weather\n";
    msg += "alarm 7:30 — set alarm\n";
    msg += "focus 25 — start pomodoro timer\n";
    msg += "break — stop focus timer\n";
    msg += "sleep — night mode on\n";
    msg += "wake — morning briefing\n";
    msg += "quiet / loud — toggle alerts\n";
    msg += "report — today's data\n";
    msg += "note: your note — save note\n";
    msg += "notes — show notes\n";
    msg += "clearnotes — delete all notes\n";
    msg += "professional/chill/savage — personality\n";
    msg += "diagnostics — system health\n";
    msg += "stop — stop alarm\n";
    msg += "\nAnything else → Jarvis AI answers 🧠";
    tgSend(msg);
    return;
  }

  // ── AI FALLBACK ──────────────────────────────────────
  tgSend("🤖 " + askGemini(text));
}

// ════════════════════════════════════════════════════════
//  SCREEN 0 — MY ROOM
// ════════════════════════════════════════════════════════
void drawScreen0() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  ntp.update();
  char timeBuf[6];
  sprintf(timeBuf, "%02d:%02d", ntp.getHours(), ntp.getMinutes());
  display.setTextSize(1);
  display.setCursor(2, 0);
  display.print(sleepMode ? "SLEEP" : focusMode ? "FOCUS" : "MY ROOM");
  display.setCursor(128 - (int)strlen(timeBuf) * 6, 0);
  display.print(timeBuf);
  display.drawLine(0, 9, 127, 9, WHITE);

  char tbuf[7];
  sprintf(tbuf, "%.1f", room.temp);
  int numW = (int)strlen(tbuf) * 12;
  int totW = numW + 12;
  int tx   = (128 - totW) / 2;
  display.setTextSize(2);
  display.setCursor(tx, 13);
  display.print(tbuf);
  display.setTextSize(1);
  display.print("\xF7""C");

  display.drawLine(0, 30, 127, 30, WHITE);

  display.setTextSize(1);
  display.setCursor(2, 34);
  display.print("HUM");
  char hbuf[6];
  sprintf(hbuf, "%d%%", (int)room.hum);
  display.setCursor(128 - (int)strlen(hbuf) * 6, 34);
  display.print(hbuf);

  display.setCursor(2, 43);
  display.print("COMFORT");
  const char* cs = comfortStr(comfort(room.temp, room.hum));
  display.setCursor(128 - (int)strlen(cs) * 6, 43);
  display.print(cs);

  display.drawLine(0, 52, 127, 52, WHITE);

  display.setCursor(2, 56);
  if (alarm.set) {
    char albuf[12];
    sprintf(albuf, "ALM %02d:%02d", alarm.hour, alarm.minute);
    display.print(albuf);
  } else if (out.ready) {
    char hibuf[7], lobuf[7];
    sprintf(hibuf, "H:%.0f\xF7", out.high);
    sprintf(lobuf, "L:%.0f\xF7", out.low);
    display.print(hibuf);
    display.setCursor(44, 56);
    display.print(lobuf);
  }
  char hibuf2[9];
  sprintf(hibuf2, "HI:%.0f\xF7""C", heatIdx(room.temp, room.hum));
  display.setCursor(128 - (int)strlen(hibuf2) * 6, 56);
  display.print(hibuf2);

  display.display();
}

// ════════════════════════════════════════════════════════
//  SCREEN 1 — OUTSIDE
// ════════════════════════════════════════════════════════
void drawScreen1() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  if (!out.ready) {
    display.setTextSize(1);
    display.setCursor(2, 0);  display.print("OUTSIDE");
    display.drawLine(0, 9, 127, 9, WHITE);
    display.setCursor(16, 26); display.print("FETCHING...");
    display.display();
    return;
  }

  display.setTextSize(1);
  display.setCursor(2, 4);
  display.print("OUTSIDE");

  char tempBuf[5];
  sprintf(tempBuf, "%.0f", out.temp);
  int tempW = (int)strlen(tempBuf) * 12 + 6;
  display.setTextSize(2);
  display.setCursor(128 - tempW, 0);
  display.print(tempBuf);
  display.setTextSize(1);
  display.print("\xF7");

  display.drawLine(0, 17, 127, 17, WHITE);

  display.setTextSize(1);
  String cond = out.condition;
  if (cond.length() > 12) cond = cond.substring(0, 12);
  display.setCursor(2, 20);
  display.print(cond);

  char flbuf[10];
  sprintf(flbuf, "FL:%.0f\xF7""C", out.feelsLike);
  display.setCursor(128 - (int)strlen(flbuf) * 6, 20);
  display.print(flbuf);

  display.drawLine(0, 30, 127, 30, WHITE);

  display.setCursor(2, 34); display.print("WIND");
  char wbuf[9];
  sprintf(wbuf, "%.0fKM/H", out.wind);
  display.setCursor(2 + 4*6 + 2, 34); display.print(wbuf);

  display.setCursor(68, 34); display.print("UV");
  char uvbuf[5];
  sprintf(uvbuf, "%.0f", out.uvMax);
  display.setCursor(68 + 2*6 + 2, 34); display.print(uvbuf);

  display.setCursor(2, 44); display.print("HUM");
  char hbuf[6];
  sprintf(hbuf, "%d%%", out.humidity);
  display.setCursor(2 + 4*6 + 2, 44); display.print(hbuf);

  display.setCursor(68, 44); display.print("RAIN");
  char rbuf[6];
  sprintf(rbuf, "%d%%", out.rainChance);
  display.setCursor(68 + 4*6 + 2, 44); display.print(rbuf);

  display.drawLine(0, 54, 127, 54, WHITE);

  char hibuf[8], lobuf[8];
  sprintf(hibuf, "H:%.0f\xF7""C", out.high);
  sprintf(lobuf, "L:%.0f\xF7""C", out.low);
  display.setCursor(2, 57); display.print(hibuf);
  display.setCursor(128 - (int)strlen(lobuf) * 6, 57); display.print(lobuf);

  display.display();
}

// ════════════════════════════════════════════════════════
//  STARTUP ANIMATION
// ════════════════════════════════════════════════════════
void startup() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(4, 4);   display.print("DORM-OS");
  display.setTextSize(1);
  display.setCursor(4, 24);  display.print("MIT-ADT  PUNE");
  display.setCursor(4, 34);  display.print("V1.0  by JARVIS");
  display.drawRect(14, 50, 100, 5, WHITE);
  display.display();
  for (int p = 1; p <= 98; p += 2) {
    display.fillRect(15, 51, p, 3, WHITE);
    display.display();
    delay(12);
  }
  delay(300);
  for (int y = 0; y < SCREEN_HEIGHT; y += 2) {
    display.fillRect(0, y, SCREEN_WIDTH, 2, BLACK);
    display.display();
    delay(3);
  }
}

// ════════════════════════════════════════════════════════
//  CONNECT WIFI
// ════════════════════════════════════════════════════════
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(WHITE);
  display.setCursor(2, 0);  display.print("DORM-OS");
  display.drawLine(0, 9, 127, 9, WHITE);
  display.setCursor(2, 13); display.print("CONNECTING...");
  display.setCursor(2, 23); display.print(WIFI_SSID);
  display.display();

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(300); tries++;
    display.fillRect(0, 35, 128, 8, BLACK);
    display.setCursor(2, 35);
    for (int d = 0; d < (tries % 4) + 1; d++) display.print(". ");
    display.display();
  }

  display.clearDisplay();
  display.setCursor(2, 0);
  if (WiFi.status() == WL_CONNECTED) {
    display.print("ONLINE");
    display.drawLine(0, 9, 127, 9, WHITE);
    display.setCursor(2, 13); display.print(WiFi.localIP().toString());
    display.setCursor(2, 23); display.print("DORM-OS READY");
    beep(3, 80, 60);
  } else {
    display.print("OFFLINE");
    display.drawLine(0, 9, 127, 9, WHITE);
    display.setCursor(2, 13); display.print("ROOM ONLY MODE");
    startBlink(RED_LED, 3, 300);
    beep(1, 500);
  }
  display.display();
  delay(1200);
}

// ════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n[DORM-OS V1.0]");

  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER,  OUTPUT);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER,  LOW);   // idle = LOW (active-HIGH buzzer)

  // Self-test
  digitalWrite(RED_LED, HIGH); delay(150);
  digitalWrite(RED_LED, LOW);  delay(80);
  beep(1, 100);

  memset(alertSentAt, 0, sizeof(alertSentAt));
  memset(alertActive, 0, sizeof(alertActive));

  dht.begin();
  Wire.begin(4, 5);   // SDA=D2, SCL=D1

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED not found"));
    while (true) delay(1000);
  }

  startup();
  connectWiFi();

  securedClient.setInsecure();
  ntp.begin();
  ntp.update();

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(t) && !isnan(h)) {
    room.temp = t; room.hum = h;
    room.maxTemp = t; room.minTemp = t;
    room.sumTemp = t; room.readings = 1;
  }

  fetchWeather();
  unsigned long now = millis();
  lastFetchMs = lastScreenMs = lastSensorMs = lastBotCheckMs = now;

  tgSend("🤖 DORM-OS V1.0 started!\n" + WiFi.localIP().toString() +
         "\n\nSend *help* for all commands.");
  drawScreen0();
}

// ════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  updateBlink();
  wifiLoop();
  checkAlarm();
  checkFocusTimer();

  // ── Telegram bot polling ─────────────────────────────
  if (now - lastBotCheckMs >= BOT_CHECK_MS) {
    lastBotCheckMs = now;
    if (WiFi.status() == WL_CONNECTED) {
      int numMsg = bot.getUpdates(bot.last_message_received + 1);
      for (int i = 0; i < numMsg; i++) {
        String chatId = bot.messages[i].chat_id;
        if (chatId == CHAT_ID) {
          handleCommand(bot.messages[i].text);
        }
      }
    }
  }

  // ── Weather fetch ─────────────────────────────────────
  if (now - lastFetchMs >= FETCH_MS) {
    fetchWeather();
    lastFetchMs = now;
    checkAlerts();
    sendDailyReport();
  }

  // ── Screen rotation ───────────────────────────────────
  if (now - lastScreenMs >= SCREEN_MS) {
    currentScreen = (currentScreen + 1) % NUM_SCREENS;
    lastScreenMs  = now;
    display.clearDisplay(); display.display();
    delay(25);
    currentScreen == 0 ? drawScreen0() : drawScreen1();
    return;
  }

  // ── Sensor read ───────────────────────────────────────
  if (now - lastSensorMs < SENSOR_MS) return;
  lastSensorMs = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(t) || isnan(h)) {
    sensorFails++;
    Serial.printf("[Sensor] Fail #%d\n", sensorFails);
    if (sensorFails >= 3 && !alertActive[A_SENSOR_FAIL]) {
      tgSend("⚠️ DHT11 sensor error — check D4 wiring.");
      startBlink(RED_LED, 0, 200, true);
      markAlert(A_SENSOR_FAIL);
    }
    if (currentScreen == 0) {
      display.clearDisplay();
      display.setTextSize(1); display.setTextColor(WHITE);
      display.setCursor(2, 0);   display.print("MY ROOM");
      display.drawLine(0, 9, 127, 9, WHITE);
      display.setCursor(10, 26); display.print("SENSOR ERROR");
      display.setCursor(4,  38); display.print("CHECK D4 WIRING");
      display.display();
    }
    return;
  }

  if (alertActive[A_SENSOR_FAIL]) {
    tgSend("✅ Sensor back online.");
    stopBlink();
    alertActive[A_SENSOR_FAIL] = false;
    sensorFails = 0;
  }
  sensorFails = 0;
  room.temp = t; room.hum = h;

  // Track daily stats
  if (t > room.maxTemp) room.maxTemp = t;
  if (t < room.minTemp) room.minTemp = t;
  room.sumTemp += t; room.readings++;

  Serial.printf("[Room] %.1fC  %.1f%%\n", t, h);

  if (currentScreen == 0) drawScreen0();
}
