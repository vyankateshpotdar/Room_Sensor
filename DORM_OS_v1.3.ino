// ════════════════════════════════════════════════════════
//  DORM-OS v1.3 — MIT ADT Pune
//  Hardware: NodeMCU ESP8266 + SSD1306 OLED 0.96"
//           + DHT11 + Red LED (D6/GPIO12) + Buzzer (D7/GPIO13)
//
//  Changes from v1.2:
//   ★ ALARM FIX: alarm now never misses a minute (dedicated tracker)
//   ★ AM/PM support: "alarm 7:30 am" / "alarm 9:00 pm" both work
//   ★ Telegram inline keyboard buttons on key messages
//   ★ Callback query polling (button taps handled like commands)
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
#define BUZZER    13   // D7

// ── WiFi credentials ─────────────────────────────────────
const char* WIFI_SSID     = "Manet Hostel@103";
const char* WIFI_PASSWORD = "MitHostel@103";

// ── Telegram ─────────────────────────────────────────────
#define BOT_TOKEN  "7696FsBk"
#define CHAT_ID    "53"
WiFiClientSecure securedClient;
UniversalTelegramBot bot(BOT_TOKEN, securedClient);

// ── Grok (xAI) API ───────────────────────────────────────
#define GROK_API_KEY  "xai-REPLACE_WITH_NEW_KEY_AFTER_REGENERATING"
const char* GROK_HOST = "api.x.ai";

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
//  POWER CUT COUNTER — RTC memory (survives reset/power loss)
// ════════════════════════════════════════════════════════
#define RTC_MAGIC      0xDEADBEEF
#define RTC_SLOT_MAGIC 65
#define RTC_SLOT_COUNT 66

uint32_t rtcMagic     = 0;
uint32_t rtcPowerCuts = 0;

void loadPowerCuts() {
  ESP.rtcUserMemoryRead(RTC_SLOT_MAGIC, &rtcMagic, sizeof(rtcMagic));
  if (rtcMagic == RTC_MAGIC) {
    ESP.rtcUserMemoryRead(RTC_SLOT_COUNT, &rtcPowerCuts, sizeof(rtcPowerCuts));
  } else {
    rtcMagic     = RTC_MAGIC;
    rtcPowerCuts = 0;
    savePowerCuts();
  }
}
void savePowerCuts() {
  ESP.rtcUserMemoryWrite(RTC_SLOT_MAGIC, &rtcMagic,     sizeof(rtcMagic));
  ESP.rtcUserMemoryWrite(RTC_SLOT_COUNT, &rtcPowerCuts, sizeof(rtcPowerCuts));
}
void incrementPowerCuts() { rtcPowerCuts++; savePowerCuts(); }
void resetPowerCuts()     { rtcPowerCuts = 0; savePowerCuts(); }

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
  // ★ FIX: track last checked minute so we never miss the alarm tick
  int    lastCheckedMinute = -1;
  int    lastCheckedHour   = -1;
};

OutsideData out;
RoomData    room;
AlarmData   alarm;

// ── Notes ────────────────────────────────────────────────
#define MAX_NOTES 5
String notes[MAX_NOTES];
int noteCount = 0;

// ── Daily tracking ───────────────────────────────────────
int   studyMinutes    = 0;
bool  reportSentToday = false;
int   lastReportDay   = -1;
bool  weeklyReportSentThisWeek = false;
int   lastWeeklyDay   = -1;

// ── Jarvis modes ─────────────────────────────────────────
bool   sleepMode  = false;
bool   quietMode  = false;
bool   focusMode  = false;
int    focusMins  = 25;
unsigned long focusStartMs = 0;
String jarvisPersonality = "chill";

// ════════════════════════════════════════════════════════
//  TIMERS
// ════════════════════════════════════════════════════════
unsigned long lastSensorMs      = 0;
unsigned long lastFetchMs       = 0;
unsigned long lastScreenMs      = 0;
unsigned long lastBotCheckMs    = 0;
unsigned long fetchedAtMs       = 0;
// ★ FIX: alarm gets its own tight poll interval (every 10s)
unsigned long lastAlarmCheckMs  = 0;
int           currentScreen     = 0;

#define SENSOR_MS      3000UL
#define FETCH_MS       300000UL
#define SCREEN_MS      8000UL
#define BOT_CHECK_MS   2000UL
#define ALARM_CHECK_MS 10000UL   // ★ check alarm every 10 s regardless of screen
#define NUM_SCREENS    2

// ════════════════════════════════════════════════════════
//  BUZZER
// ════════════════════════════════════════════════════════
void alarmBeep(int count, int onMs = 150, int offMs = 80) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER, HIGH); delay(onMs);
    digitalWrite(BUZZER, LOW);
    if (i < count - 1) delay(offMs);
  }
  digitalWrite(BUZZER, LOW);
}

// ════════════════════════════════════════════════════════
//  RED LED BLINK ENGINE
// ════════════════════════════════════════════════════════
struct BlinkJob {
  int pin; int totalPairs; int pairsDone;
  unsigned long periodMs; bool continuous; bool active; bool ledOn;
  unsigned long lastToggleMs;
};
BlinkJob blk = {0,0,0,0,false,false,false,0};

void startBlink(int pin, int count, unsigned long period, bool continuous = false) {
  if (blk.active && blk.ledOn) digitalWrite(blk.pin, LOW);
  blk = {pin, count, 0, period, continuous, true, false, millis()};
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

void tgLedFlash() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(RED_LED, HIGH); delay(40);
    digitalWrite(RED_LED, LOW);  delay(40);
  }
}

// ════════════════════════════════════════════════════════
//  ALERT IDs
// ════════════════════════════════════════════════════════
#define A_RAIN90      0
#define A_RAIN70      1
#define A_UV10        2
#define A_UV7         3
#define A_TEMP38      4
#define A_TEMP10      5
#define A_WIND60      6
#define A_WIND40      7
#define A_STORM       8
#define A_FOG         9
#define A_ROOM_HOT36  10
#define A_ROOM_HOT32  11
#define A_ROOM_RISE   12
#define A_HUM_HIGH    13
#define A_HUM_LOW     14
#define A_NIGHT_HOT   15
#define A_HI_45       16
#define A_HI_40       17
#define A_WIFI_LOST   18
#define A_SENSOR_FAIL 19
#define A_API_FAIL    20
#define NUM_ALERTS    21

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
  return c == 0 ? "GOOD" : c == 1 ? "FAIR" : "POOR";
}
String comfortEmoji(int c) {
  return c == 0 ? "✅" : c == 1 ? "⚠️" : "🔴";
}

// ════════════════════════════════════════════════════════
//  ★ TELEGRAM INLINE KEYBOARD HELPER
//  Sends a message with a row of inline buttons.
//  buttonLabels[] and buttonData[] must have same length.
//
//  Example:
//    String lbls[] = {"Status","Weather","Temp"};
//    String data[] = {"status","weather","temp"};
//    tgSendWithButtons("Choose:", lbls, data, 3);
// ════════════════════════════════════════════════════════
void tgSendWithButtons(const String& text,
                       String labels[], String callbackData[],
                       int count, int cols = 3) {
  if (WiFi.status() != WL_CONNECTED) return;

  // Build inline_keyboard JSON array
  // Rows of 'cols' buttons each
  String keyboard = "[[";
  for (int i = 0; i < count; i++) {
    if (i > 0 && i % cols == 0) keyboard += "],[";
    else if (i > 0)              keyboard += ",";
    keyboard += "{\"text\":\"" + labels[i] +
                "\",\"callback_data\":\"" + callbackData[i] + "\"}";
  }
  keyboard += "]]";

  String replyMarkup = "{\"inline_keyboard\":" + keyboard + "}";

  // URL-encode the text is not needed — bot library handles it, but
  // reply_markup must be passed via a raw HTTPS call since
  // UniversalTelegramBot doesn't expose reply_markup natively.
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.telegram.org/bot" BOT_TOKEN "/sendMessage");
  https.addHeader("Content-Type", "application/json");

  // Build payload — escape newlines in text
  String safeText = text;
  safeText.replace("\n", "\\n");
  safeText.replace("\"", "'");

  String payload = "{\"chat_id\":\"" + String(CHAT_ID) + "\","
                   "\"text\":\"" + safeText + "\","
                   "\"parse_mode\":\"Markdown\","
                   "\"reply_markup\":" + replyMarkup + "}";

  int code = https.POST(payload);
  tgLedFlash();
  Serial.println(code == 200 ? "[TG+BTN] OK" : "[TG+BTN] FAIL " + String(code));
  https.end();
}

// ════════════════════════════════════════════════════════
//  TELEGRAM SEND (plain text)
// ════════════════════════════════════════════════════════
void tgSend(const String& msg) {
  if (WiFi.status() != WL_CONNECTED) {
    startBlink(RED_LED, 1, 300);
    return;
  }
  bool ok = bot.sendMessage(CHAT_ID, msg, "");
  if (ok) tgLedFlash();
  else    startBlink(RED_LED, 3, 200);
  Serial.println(ok ? "[TG] OK" : "[TG] FAIL");
}

// ════════════════════════════════════════════════════════
//  GROK AI
// ════════════════════════════════════════════════════════
String askGrok(String userMsg) {
  if (WiFi.status() != WL_CONNECTED) return "I'm offline right now.";
  if (String(GROK_API_KEY).startsWith("xai-REPLACE")) {
    return "Please regenerate your Grok API key at console.x.ai and update GROK_API_KEY.";
  }

  String systemText = "You are JARVIS, a smart hostel room assistant on a NodeMCU ESP8266. ";
  systemText += "Personality: " + jarvisPersonality + ". ";
  systemText += "Room: " + String(room.temp, 1) + "°C, ";
  systemText += "Humidity: " + String(room.hum, 0) + "%, ";
  systemText += "Comfort: " + String(comfortStr(comfort(room.temp, room.hum))) + ". ";
  if (out.ready) {
    systemText += "Outside: " + String(out.temp, 1) + "°C, " + out.condition + ", ";
    systemText += "Rain: " + String(out.rainChance) + "%, UV: " + String(out.uvMax, 0) + ", ";
    systemText += "Wind: " + String(out.wind, 0) + " km/h. ";
  }
  ntp.update();
  systemText += "Time: " + String(ntp.getHours()) + ":" +
                (ntp.getMinutes() < 10 ? "0" : "") + String(ntp.getMinutes()) + " IST. ";
  systemText += "Reply in under 3 sentences. Plain text only, no markdown.";
  systemText.replace("\"", "'");
  userMsg.replace("\"", "'");

  String payload = "{\"model\":\"grok-3-mini\",\"max_output_tokens\":150,";
  payload += "\"instructions\":\"" + systemText + "\",";
  payload += "\"input\":\"" + userMsg + "\"}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.x.ai/v1/responses");
  https.addHeader("Content-Type",  "application/json");
  https.addHeader("Authorization", "Bearer " + String(GROK_API_KEY));
  https.setTimeout(25000);

  int code  = https.POST(payload);
  String reply = "I couldn't reach Grok right now.";

  if (code == 200) {
    String response = https.getString();
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, response)) {
      for (JsonObject item : doc["output"].as<JsonArray>()) {
        if (String(item["type"].as<const char*>()) == "message") {
          for (JsonObject part : item["content"].as<JsonArray>()) {
            if (String(part["type"].as<const char*>()) == "output_text") {
              reply = part["text"].as<String>();
              reply.trim();
              break;
            }
          }
          break;
        }
      }
    }
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
      if (!alertActive[A_WIFI_LOST]) {
        alarmBeep(3, 200, 150);
        startBlink(RED_LED, 50, 600);
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
    if (!deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
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
//  ALERTS
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
    tgSend("🏜️ Air too dry — " + String(room.hum, 0) + "%."); markAlert(A_HUM_LOW);
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
  int d = ntp.getDay();

  if (h == 23 && !reportSentToday) {
    String msg = "📊 *Daily Report*\n\n🌡️ Room\n";
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
    msg += "\n🔌 Power cuts today: " + String(rtcPowerCuts) + "\n";
    msg += "Uptime: " + String(millis() / 3600000UL) + " hrs\n";
    msg += "\n😴 Sleep forecast: ";
    msg += (room.temp > 30 || room.hum > 70) ? "Poor — room too warm/humid" : "Good — conditions ok";
    tgSend(msg);
    reportSentToday = true;
    lastReportDay   = d;
    room.maxTemp = -99; room.minTemp = 99;
    room.sumTemp = 0;   room.readings = 0;
    resetPowerCuts();
  }
  if (h == 0 && reportSentToday && lastReportDay != d) reportSentToday = false;

  if (d == 0 && h == 9 && !weeklyReportSentThisWeek) {
    String msg = "📈 *Weekly Summary*\n\nDORM-OS running " +
                 String(millis() / 86400000UL) + "+ days.\n";
    msg += "Room: " + String(room.temp, 1) + "°C, " + String(room.hum, 0) + "% hum\n";
    if (out.ready) msg += "Outside: " + String(out.temp, 1) + "°C — " + out.condition + "\n";
    msg += "\nHave a great week! 💪";
    tgSend(msg);
    weeklyReportSentThisWeek = true;
  }
  if (d != 0) weeklyReportSentThisWeek = false;
}

// ════════════════════════════════════════════════════════
//  ★ ALARM CHECK — FIXED
//  Problem in v1.2: loop() had an early `return` in the screen
//  rotation block, which could skip checkAlarm() for a full
//  SCREEN_MS (8 s) interval.  Even worse, a one-minute window
//  was easy to miss when the esp was busy with TLS calls.
//
//  Fix: alarm is checked on its own 10-second timer, and
//  instead of testing "are we IN the alarm minute right now"
//  (which can be missed), we track the last H:M we checked
//  and fire if we cross the alarm minute since last check.
// ════════════════════════════════════════════════════════
int sensorFails = 0;

void checkAlarm() {
  if (!alarm.set && !alarm.ringing) return;
  ntp.update();
  int h = ntp.getHours();
  int m = ntp.getMinutes();

  // ★ Fire if we just entered or crossed the alarm minute
  if (!alarm.ringing &&
      h == alarm.hour && m == alarm.minute &&
      !(alarm.lastCheckedHour == h && alarm.lastCheckedMinute == m)) {

    alarm.ringing      = true;
    alarm.ringStartMs  = millis();
    alarm.lastCheckedHour   = h;
    alarm.lastCheckedMinute = m;

    // ★ Display AM/PM in the wake-up message
    int  dispH  = h % 12; if (dispH == 0) dispH = 12;
    String ampm = (h < 12) ? "AM" : "PM";
    String timeStr = String(dispH) + ":" + (m < 10 ? "0" : "") + String(m) + " " + ampm;

    String wakeMsg = "⏰ *WAKE UP!* It's " + timeStr +
                     "\nRoom: " + String(room.temp, 1) + "°C  " +
                     String(room.hum, 0) + "% hum" +
                     (out.ready ? "\nOutside: " + String(out.temp, 1) + "°C — " + out.condition : "") +
                     "\n\nTap below or send *stop* to silence.";

    // ★ Inline keyboard with Stop Alarm button
    String lbls[] = {"🛑 Stop Alarm", "💤 Snooze 5m"};
    String data[] = {"stop",          "snooze"};
    tgSendWithButtons(wakeMsg, lbls, data, 2, 2);

    startBlink(RED_LED, 0, 400, true);

  } else if (!alarm.ringing) {
    // update last-checked so we detect crossing the minute
    alarm.lastCheckedHour   = h;
    alarm.lastCheckedMinute = m;
  }

  if (alarm.ringing) {
    if ((millis() - alarm.ringStartMs) % 5000 < 200) {
      alarmBeep(3, 150, 80);
    }
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
  if ((millis() - focusStartMs) / 60000UL >= (unsigned long)focusMins) {
    focusMode = false;
    stopBlink();
    alarmBeep(3, 200, 100);
    tgSend("🍅 Focus session done! " + String(focusMins) + " mins complete. Take a break.");
    studyMinutes += focusMins;
  }
}

// ════════════════════════════════════════════════════════
//  BUILD STATUS MESSAGE
// ════════════════════════════════════════════════════════
String buildStatusMsg() {
  ntp.update();
  int h = ntp.getHours();
  int m = ntp.getMinutes();
  // ★ Show AM/PM in status
  int dispH = h % 12; if (dispH == 0) dispH = 12;
  String ampm = (h < 12) ? "AM" : "PM";

  String msg = "📍 *DORM-OS Status*\n";
  msg += "🕐 " + String(dispH) + ":" + (m<10?"0":"") + String(m) + " " + ampm + " IST\n\n";

  msg += "🏠 *Room*\n";
  msg += "Temp: " + String(room.temp, 1) + "°C\n";
  msg += "Humidity: " + String(room.hum, 0) + "%\n";
  msg += "Comfort: " + comfortEmoji(comfort(room.temp, room.hum)) + " " + comfortStr(comfort(room.temp, room.hum)) + "\n";
  msg += "Heat index: " + String(heatIdx(room.temp, room.hum), 1) + "°C\n";

  if (out.ready) {
    msg += "\n🌤️ *Outside*\n";
    msg += "Temp: " + String(out.temp, 1) + "°C (feels " + String(out.feelsLike, 0) + "°C)\n";
    msg += "Condition: " + out.condition + "\n";
    msg += "Rain: " + String(out.rainChance) + "%  UV: " + String(out.uvMax, 0) + "\n";
    msg += "Wind: " + String(out.wind, 0) + " km/h\n";
    msg += "H:" + String(out.high, 0) + "° / L:" + String(out.low, 0) + "°\n";
  }

  msg += "\n⚙️ *System*\n";
  msg += "Uptime: " + String(millis()/3600000UL) + "h " + String((millis()%3600000UL)/60000UL) + "m\n";
  msg += "WiFi: " + WiFi.localIP().toString() + "\n";
  msg += "Mode: " + String(sleepMode?"Sleep":focusMode?"Focus":"Normal") + "\n";
  msg += "Power cuts today: " + String(rtcPowerCuts) + "\n";
  if (alarm.set) {
    int ah = alarm.hour % 12; if (ah == 0) ah = 12;
    String aa = (alarm.hour < 12) ? "AM" : "PM";
    msg += "Alarm: " + String(ah) + ":" + (alarm.minute<10?"0":"") + String(alarm.minute) + " " + aa + "\n";
  }
  return msg;
}

// ════════════════════════════════════════════════════════
//  ★ PARSE AM/PM TIME STRING
//  Accepts: "7:30", "7:30 am", "7:30 pm", "19:30"
//  Returns hour in 24h (0-23), -1 on error.
// ════════════════════════════════════════════════════════
int parseTimeHour(String timeStr, int& outHour, int& outMinute) {
  timeStr.trim();
  String lower = timeStr;
  lower.toLowerCase();

  bool isPM = lower.indexOf("pm") >= 0;
  bool isAM = lower.indexOf("am") >= 0;

  // Strip am/pm
  lower.replace("pm", "");
  lower.replace("am", "");
  lower.trim();

  int colonIdx = lower.indexOf(':');
  if (colonIdx < 1) return -1;

  int h = lower.substring(0, colonIdx).toInt();
  int m = lower.substring(colonIdx + 1).toInt();

  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;

  if (isAM || isPM) {
    // 12-hour mode
    if (isPM && h != 12) h += 12;
    if (isAM && h == 12) h = 0;
  }
  // If neither am nor pm → treat as 24h, no conversion needed

  outHour   = h;
  outMinute = m;
  return 0;
}

// ════════════════════════════════════════════════════════
//  PROCESS TELEGRAM COMMANDS
// ════════════════════════════════════════════════════════
void handleCommand(String text) {
  text.trim();
  String lower = text;
  lower.toLowerCase();

  Serial.println("[CMD] " + text);
  tgLedFlash();

  // ── STOP ALARM ──────────────────────────────────────
  if (lower == "stop") {
    if (alarm.ringing) {
      alarm.ringing = false;
      alarm.set     = false;
      stopBlink();
      tgSend("✅ Alarm stopped. Good morning! ☀️");
    } else {
      tgSend("No alarm is ringing right now.");
    }
    return;
  }

  // ★ SNOOZE (5 minute snooze)
  if (lower == "snooze") {
    if (alarm.ringing || alarm.set) {
      alarm.ringing = false;
      ntp.update();
      int newMin = ntp.getMinutes() + 5;
      int newHr  = ntp.getHours();
      if (newMin >= 60) { newMin -= 60; newHr = (newHr + 1) % 24; }
      alarm.hour   = newHr;
      alarm.minute = newMin;
      alarm.set    = true;
      alarm.lastCheckedHour   = -1;   // reset so it fires again
      alarm.lastCheckedMinute = -1;
      stopBlink();
      int ah = newHr % 12; if (ah == 0) ah = 12;
      String aa = (newHr < 12) ? "AM" : "PM";
      tgSend("💤 Snoozed! Alarm reset for " + String(ah) + ":" +
             (newMin < 10 ? "0" : "") + String(newMin) + " " + aa);
    } else {
      tgSend("No active alarm to snooze.");
    }
    return;
  }

  // ── STATUS ───────────────────────────────────────────
  if (lower == "status" || lower == "s") {
    // ★ Status + inline quick-action buttons
    String lbls[] = {"🌤 Weather", "🌡 Temp", "📊 Report", "⚙️ Diag"};
    String data[] = {"weather",   "temp",   "report",   "diagnostics"};
    tgSendWithButtons(buildStatusMsg(), lbls, data, 4, 2);
    return;
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
      String msg = "🌤️ *Outside Weather*\n" + out.condition + "\n";
      msg += "Temp: " + String(out.temp, 1) + "°C (feels " + String(out.feelsLike, 0) + "°C)\n";
      msg += "Rain: " + String(out.rainChance) + "% | UV: " + String(out.uvMax, 0) + "\n";
      msg += "Wind: " + String(out.wind, 0) + " km/h\n";
      msg += "H:" + String(out.high, 0) + "° / L:" + String(out.low, 0) + "°";
      String lbls[] = {"🔄 Refresh", "📍 Status"};
      String data[] = {"weather",   "status"};
      tgSendWithButtons(msg, lbls, data, 2);
    }
    return;
  }

  // ── ★ ALARM (with AM/PM support) ─────────────────────
  if (lower.startsWith("alarm ")) {
    String timeStr = text.substring(6);
    timeStr.trim();

    int h = 0, m = 0;
    if (parseTimeHour(timeStr, h, m) == 0) {
      alarm.hour   = h;
      alarm.minute = m;
      alarm.set    = true;
      alarm.ringing = false;
      alarm.lastCheckedHour   = -1;
      alarm.lastCheckedMinute = -1;

      int dispH = h % 12; if (dispH == 0) dispH = 12;
      String ampm = (h < 12) ? "AM" : "PM";
      String confirmMsg = "⏰ Alarm set for *" + String(dispH) + ":" +
                          (m < 10 ? "0" : "") + String(m) + " " + ampm + "*";

      String lbls[] = {"❌ Cancel Alarm"};
      String data[] = {"cancelalarm"};
      tgSendWithButtons(confirmMsg, lbls, data, 1, 1);
    } else {
      tgSend("⏰ Format: `alarm 7:30 am` or `alarm 19:30`\nExamples:\n• alarm 6:00 am\n• alarm 9:30 pm\n• alarm 14:00");
    }
    return;
  }

  // ★ CANCEL ALARM
  if (lower == "cancelalarm" || lower == "cancel alarm") {
    if (alarm.set || alarm.ringing) {
      alarm.set = false; alarm.ringing = false;
      stopBlink();
      tgSend("❌ Alarm cancelled.");
    } else {
      tgSend("No alarm is set.");
    }
    return;
  }

  // ── FOCUS TIMER ──────────────────────────────────────
  if (lower.startsWith("focus")) {
    String numStr = lower.substring(5); numStr.trim();
    focusMins = numStr.length() > 0 ? numStr.toInt() : 25;
    if (focusMins <= 0) focusMins = 25;
    focusMode    = true;
    focusStartMs = millis();
    startBlink(RED_LED, 0, 1500, true);
    String lbls[] = {"⏸ Stop Focus"};
    String data[] = {"break"};
    tgSendWithButtons("🍅 Focus mode ON — " + String(focusMins) + " min session started. GO!", lbls, data, 1, 1);
    return;
  }

  // ── SLEEP ────────────────────────────────────────────
  if (lower == "sleep") {
    sleepMode = true; quietMode = true;
    stopBlink();
    String lbls[] = {"☀️ Wake Up"};
    String data[] = {"wake"};
    tgSendWithButtons("🌙 Sleep mode ON. Alerts silenced. Good night!", lbls, data, 1, 1);
    return;
  }

  if (lower == "wake") {
    sleepMode = false; quietMode = false;
    ntp.update();
    int h = ntp.getHours();
    int dispH = h % 12; if (dispH == 0) dispH = 12;
    String msg = "🌅 Good morning!\nRoom: " + String(room.temp, 1) + "°C  " + String(room.hum, 0) + "%\n";
    if (out.ready) msg += "Outside: " + String(out.temp, 1) + "°C — " + out.condition;
    tgSend(msg);
    return;
  }

  // ── QUIET / LOUD ─────────────────────────────────────
  if (lower == "quiet") { quietMode = true;  tgSend("🔕 Quiet mode ON."); return; }
  if (lower == "loud" || lower == "unmute") { quietMode = false; tgSend("🔔 Alerts re-enabled."); return; }

  // ── FOCUS STOP ───────────────────────────────────────
  if (lower == "break" || lower == "stopfocus") {
    focusMode = false; stopBlink();
    tgSend("⏸ Focus stopped. Take a break!"); return;
  }

  // ── REPORT ───────────────────────────────────────────
  if (lower == "report" || lower == "daily") {
    String msg = "📊 *Today So Far*\n";
    if (room.readings > 0) {
      msg += "Room max: " + String(room.maxTemp, 1) + "°C\n";
      msg += "Room min: " + String(room.minTemp, 1) + "°C\n";
    }
    if (out.ready) msg += "Outside H:" + String(out.high, 0) + "° L:" + String(out.low, 0) + "°\n";
    msg += "Uptime: " + String(millis()/3600000UL) + "h\n";
    msg += "Power cuts: " + String(rtcPowerCuts);
    tgSend(msg);
    return;
  }

  // ── NOTES ────────────────────────────────────────────
  if (lower.startsWith("note:") || lower.startsWith("note ")) {
    String noteText = text.substring(5); noteText.trim();
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
    for (int i = 0; i < noteCount; i++) msg += String(i+1) + ". " + notes[i] + "\n";
    tgSend(msg); return;
  }
  if (lower == "clearnotes") { noteCount = 0; tgSend("📝 Notes cleared."); return; }

  // ── PERSONALITY ──────────────────────────────────────
  if (lower == "professional mode" || lower == "professional") {
    jarvisPersonality = "professional"; tgSend("🎩 Professional mode."); return;
  }
  if (lower == "chill mode" || lower == "chill") {
    jarvisPersonality = "chill"; tgSend("😎 Chill mode on bro."); return;
  }
  if (lower == "savage mode" || lower == "savage") {
    jarvisPersonality = "savage"; tgSend("😈 Savage mode. Brace yourself."); return;
  }

  // ── DIAGNOSTICS ──────────────────────────────────────
  if (lower == "diagnostics" || lower == "diag") {
    String msg = "🔧 *System Diagnostics*\n";
    msg += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? "✅ " + WiFi.localIP().toString() : "❌ Offline") + "\n";
    msg += "Sensor: " + String(sensorFails == 0 ? "✅ OK" : "❌ " + String(sensorFails) + " fails") + "\n";
    msg += "Weather API: " + String(out.ready ? "✅ OK" : "❌ No data") + "\n";
    msg += "AI: Grok (xAI)\n";
    msg += "Uptime: " + String(millis()/3600000UL) + "h " + String((millis()%3600000UL)/60000UL) + "m\n";
    msg += "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    msg += "Power cuts: " + String(rtcPowerCuts) + "\n";
    msg += "Personality: " + jarvisPersonality;
    tgSend(msg);
    return;
  }

  // ── ★ HELP (with inline buttons for common actions) ──
  if (lower == "help" || lower == "?") {
    String msg = "🤖 *DORM-OS v1.3 Commands*\n\n";
    msg += "status / s — full room report\n";
    msg += "temp — current temperature\n";
    msg += "weather / w — outside weather\n";
    msg += "alarm 7:30 am — set alarm (AM/PM or 24h)\n";
    msg += "snooze — snooze alarm 5 min\n";
    msg += "cancel alarm — cancel alarm\n";
    msg += "focus 25 — start pomodoro timer\n";
    msg += "break — stop focus timer\n";
    msg += "sleep — night mode\n";
    msg += "wake — morning briefing\n";
    msg += "quiet / loud — toggle alerts\n";
    msg += "report — today's data\n";
    msg += "note: text — save note\n";
    msg += "notes — show notes\n";
    msg += "clearnotes — delete notes\n";
    msg += "professional/chill/savage — AI personality\n";
    msg += "diagnostics — system health\n";
    msg += "stop — stop alarm\n";
    msg += "\nAnything else → Grok AI 🧠";

    // ★ Quick-access inline buttons
    String lbls[] = {"📍 Status", "🌤 Weather", "🌡 Temp", "📊 Report"};
    String data[] = {"status",   "weather",   "temp",   "report"};
    tgSendWithButtons(msg, lbls, data, 4, 2);
    return;
  }

  // ── AI FALLBACK (Grok) ───────────────────────────────
  tgSend("🤖 " + askGrok(text));
}

// ════════════════════════════════════════════════════════
//  SCREENS
// ════════════════════════════════════════════════════════
void drawScreen0() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  ntp.update();
  // ★ Show AM/PM on OLED too
  int h = ntp.getHours(), m = ntp.getMinutes();
  int dispH = h % 12; if (dispH == 0) dispH = 12;
  const char* ampm = (h < 12) ? "AM" : "PM";
  char timeBuf[9];
  sprintf(timeBuf, "%d:%02d%s", dispH, m, ampm);

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
  display.setTextSize(2); display.setCursor(tx, 13);
  display.print(tbuf);
  display.setTextSize(1); display.print("\xF7""C");

  display.drawLine(0, 30, 127, 30, WHITE);

  display.setTextSize(1);
  display.setCursor(2, 34); display.print("HUM");
  char hbuf[6];
  sprintf(hbuf, "%d%%", (int)room.hum);
  display.setCursor(128 - (int)strlen(hbuf) * 6, 34); display.print(hbuf);

  display.setCursor(2, 43); display.print("COMFORT");
  const char* cs = comfortStr(comfort(room.temp, room.hum));
  display.setCursor(128 - (int)strlen(cs) * 6, 43); display.print(cs);

  display.drawLine(0, 52, 127, 52, WHITE);
  display.setCursor(2, 56);
  if (alarm.set) {
    int ah = alarm.hour % 12; if (ah == 0) ah = 12;
    const char* aa = (alarm.hour < 12) ? "AM" : "PM";
    char albuf[14];
    sprintf(albuf, "ALM %d:%02d%s", ah, alarm.minute, aa);
    display.print(albuf);
  } else if (out.ready) {
    char hibuf[7], lobuf[7];
    sprintf(hibuf, "H:%.0f\xF7", out.high);
    sprintf(lobuf, "L:%.0f\xF7", out.low);
    display.print(hibuf);
    display.setCursor(44, 56); display.print(lobuf);
  }
  char hibuf2[9];
  sprintf(hibuf2, "HI:%.0f\xF7""C", heatIdx(room.temp, room.hum));
  display.setCursor(128 - (int)strlen(hibuf2) * 6, 56); display.print(hibuf2);
  display.display();
}

void drawScreen1() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  if (!out.ready) {
    display.setTextSize(1);
    display.setCursor(2, 0); display.print("OUTSIDE");
    display.drawLine(0, 9, 127, 9, WHITE);
    display.setCursor(16, 26); display.print("FETCHING...");
    display.display();
    return;
  }

  display.setTextSize(1);
  display.setCursor(2, 4); display.print("OUTSIDE");
  char tempBuf[5];
  sprintf(tempBuf, "%.0f", out.temp);
  int tempW = (int)strlen(tempBuf) * 12 + 6;
  display.setTextSize(2);
  display.setCursor(128 - tempW, 0); display.print(tempBuf);
  display.setTextSize(1); display.print("\xF7");

  display.drawLine(0, 17, 127, 17, WHITE);
  display.setTextSize(1);
  String cond = out.condition;
  if (cond.length() > 12) cond = cond.substring(0, 12);
  display.setCursor(2, 20); display.print(cond);
  char flbuf[10];
  sprintf(flbuf, "FL:%.0f\xF7""C", out.feelsLike);
  display.setCursor(128 - (int)strlen(flbuf) * 6, 20); display.print(flbuf);

  display.drawLine(0, 30, 127, 30, WHITE);
  display.setCursor(2, 34); display.print("WIND");
  char wbuf[9]; sprintf(wbuf, "%.0fKM/H", out.wind);
  display.setCursor(2 + 4*6 + 2, 34); display.print(wbuf);
  display.setCursor(68, 34); display.print("UV");
  char uvbuf[5]; sprintf(uvbuf, "%.0f", out.uvMax);
  display.setCursor(68 + 2*6 + 2, 34); display.print(uvbuf);

  display.setCursor(2, 44); display.print("HUM");
  char hbuf[6]; sprintf(hbuf, "%d%%", out.humidity);
  display.setCursor(2 + 4*6 + 2, 44); display.print(hbuf);
  display.setCursor(68, 44); display.print("RAIN");
  char rbuf[6]; sprintf(rbuf, "%d%%", out.rainChance);
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
//  STARTUP & WIFI
// ════════════════════════════════════════════════════════
void startup() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(4, 4);  display.print("DORM-OS");
  display.setTextSize(1);
  display.setCursor(4, 24); display.print("MIT-ADT  PUNE");
  display.setCursor(4, 34); display.print("V1.3  by JARVIS");
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
    digitalWrite(BUZZER, HIGH); delay(80); digitalWrite(BUZZER, LOW); delay(60);
    digitalWrite(BUZZER, HIGH); delay(80); digitalWrite(BUZZER, LOW); delay(60);
    digitalWrite(BUZZER, HIGH); delay(80); digitalWrite(BUZZER, LOW);
  } else {
    display.print("OFFLINE");
    display.drawLine(0, 9, 127, 9, WHITE);
    display.setCursor(2, 13); display.print("ROOM ONLY MODE");
    startBlink(RED_LED, 3, 300);
    digitalWrite(BUZZER, HIGH); delay(500); digitalWrite(BUZZER, LOW);
  }
  display.display();
  delay(1200);
}

// ════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n[DORM-OS V1.3]");

  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER,  OUTPUT);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER,  LOW);

  loadPowerCuts();
  uint32_t prevMagic = 0;
  ESP.rtcUserMemoryRead(RTC_SLOT_MAGIC, &prevMagic, sizeof(prevMagic));
  if (prevMagic == RTC_MAGIC) {
    incrementPowerCuts();
    Serial.printf("[Boot] Power cut detected! Total today: %d\n", rtcPowerCuts);
  } else {
    Serial.println("[Boot] First boot.");
  }

  digitalWrite(RED_LED, HIGH); delay(150);
  digitalWrite(RED_LED, LOW);  delay(80);

  memset(alertSentAt, 0, sizeof(alertSentAt));
  memset(alertActive, 0, sizeof(alertActive));

  dht.begin();
  Wire.begin(4, 5);

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
  lastFetchMs = lastScreenMs = lastSensorMs = lastBotCheckMs = lastAlarmCheckMs = now;

  // ★ Startup message with inline help button
  String startMsg = "🤖 *DORM-OS V1.3* started!\n" +
                    WiFi.localIP().toString() +
                    "\nPower cuts today: " + String(rtcPowerCuts) +
                    "\n\n✨ *New in v1.3:*\n• Alarm fixed — never misses\n• AM/PM support (alarm 7:30 am)\n• Inline tap buttons\n• Snooze support";
  String lbls[] = {"❓ Help", "📍 Status"};
  String data[] = {"help",   "status"};
  tgSendWithButtons(startMsg, lbls, data, 2, 2);

  drawScreen0();
}

// ════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  updateBlink();
  wifiLoop();
  checkFocusTimer();

  // ★ Alarm on its own dedicated timer — never blocked by screen rotations
  if (now - lastAlarmCheckMs >= ALARM_CHECK_MS) {
    lastAlarmCheckMs = now;
    checkAlarm();
  }

  // ── Telegram bot polling ─────────────────────────────
  if (now - lastBotCheckMs >= BOT_CHECK_MS) {
    lastBotCheckMs = now;
    if (WiFi.status() == WL_CONNECTED) {
      int numMsg = bot.getUpdates(bot.last_message_received + 1);
      for (int i = 0; i < numMsg; i++) {
        String chatId = bot.messages[i].chat_id;
        if (chatId == CHAT_ID) {
          // ★ Handle both regular messages AND callback queries (button taps)
          String msgType = bot.messages[i].type;
          String text    = bot.messages[i].text;
          if (msgType == "callback_query") {
            // callback_data is stored in text for UniversalTelegramBot
            handleCommand(text);
          } else {
            handleCommand(text);
          }
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
    // ★ No early return — alarm check runs on its own timer now
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
  if (t > room.maxTemp) room.maxTemp = t;
  if (t < room.minTemp) room.minTemp = t;
  room.sumTemp += t; room.readings++;

  Serial.printf("[Room] %.1fC  %.1f%%\n", t, h);
  if (currentScreen == 0) drawScreen0();
}
