// ════════════════════════════════════════════════════════
//  WeatherKit V4.1 — MIT ADT Pune
//  CHANGES v4.1:
//   • Outside screen: new grid layout — no overlap
//   • GREEN_LED replaced by BUZZER (active-LOW buzzer on D8/GPIO15)
//   • Buzzer fix: active-LOW logic, idle = HIGH
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
#include <time.h>
#include <UniversalTelegramBot.h>

// ── Display ──────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── DHT11 ────────────────────────────────────────────────
#define DHTPIN   2
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// ── Hardware ─────────────────────────────────────────────
#define RED_LED   12   // D6 — alert blinks
#define BUZZER    13   // D7 — active-HIGH buzzer

// ── WiFi credentials ─────────────────────────────────────
const char* WIFI_SSID     = "Manet Hostel@103";
const char* WIFI_PASSWORD = "MitHostel@103";

// ── Telegram ─────────────────────────────────────────────
#define BOT_TOKEN  "7696786747:AAGcho9VTAvZiotkCT3tj86VvyYBuytFsBk"
#define CHAT_ID    "5392399263"
WiFiClientSecure securedClient;
UniversalTelegramBot bot(BOT_TOKEN, securedClient);

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

// ── NTP (IST = UTC+5:30 = 19800s) ───────────────────────
WiFiUDP   ntpUDP;
NTPClient ntp(ntpUDP, "pool.ntp.org", 19800);

// ════════════════════════════════════════════════════════
//  DATA
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
  float temp = 0;
  float hum  = 0;
};

OutsideData out;
RoomData    room;

float prevOutTemp = 0;
int   prevOutRain = 0;
int   prevOutCode = 0;

float         rapidBase     = 0;
unsigned long rapidStartMs  = 0;
bool          rapidTracking = false;

int hotStreak  = 0;
int rainStreak = 0;

// ════════════════════════════════════════════════════════
//  TIMERS
// ════════════════════════════════════════════════════════
unsigned long lastSensorMs  = 0;
unsigned long lastFetchMs   = 0;
unsigned long lastScreenMs  = 0;
unsigned long fetchedAtMs   = 0;
int           currentScreen = 0;

#define SENSOR_MS   3000UL
#define FETCH_MS    300000UL
#define SCREEN_MS   8000UL
#define NUM_SCREENS 2

// ════════════════════════════════════════════════════════
//  BUZZER HELPER — active-LOW
//  beep(n, onMs, offMs) — blocking, only at startup/reconnect
// ════════════════════════════════════════════════════════
void beep(int count, int onMs = 80, int offMs = 80) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER, HIGH);   // HIGH = ON  (active-HIGH)
    delay(onMs);
    digitalWrite(BUZZER, LOW);    // LOW = OFF
    if (i < count - 1) delay(offMs);
  }
  digitalWrite(BUZZER, LOW);      // ensure OFF when done
}

// ════════════════════════════════════════════════════════
//  LED BLINK ENGINE  (red LED only)
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
#define A_RAIN90        0
#define A_RAIN70        1
#define A_UV10          2
#define A_UV7           3
#define A_TEMP38        4
#define A_TEMP10        5
#define A_WIND60        6
#define A_WIND40        7
#define A_FEELS_HOT     8
#define A_FEELS_COLD    9
#define A_STORM         10
#define A_FOG           11
#define A_RAPID_WEATHER 12
#define A_CODE_CHANGE   13
#define A_ROOM_HOT36    14
#define A_ROOM_HOT32    15
#define A_ROOM_RISE     16
#define A_HUM_HIGH      17
#define A_HUM_LOW       18
#define A_NIGHT_HOT     19
#define A_HI_45         20
#define A_HI_40         21
#define A_DEW_26        22
#define A_DEW_24        23
#define A_BAD_SLEEP     24
#define A_NO_WORKOUT    25
#define A_HEATWAVE      26
#define A_RAINSTREAK    27
#define A_WIFI_LOST     28
#define A_SENSOR_FAIL   29
#define A_API_FAIL      30
#define NUM_ALERTS      31

const unsigned long CD[NUM_ALERTS] = {
  3600000UL, 10800000UL, 3600000UL, 10800000UL, 10800000UL,
  10800000UL, 3600000UL, 10800000UL, 7200000UL, 7200000UL,
  3600000UL, 10800000UL, 2700000UL, 10800000UL, 3600000UL,
  7200000UL, 1800000UL, 1800000UL, 1800000UL, 7200000UL,
  3600000UL, 7200000UL, 7200000UL, 7200000UL, 3600000UL,
  7200000UL, 86400000UL, 86400000UL, 0UL, 0UL, 0UL,
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

// ════════════════════════════════════════════════════════
//  TELEGRAM
// ════════════════════════════════════════════════════════
void tgSend(const String& msg) {
  if (WiFi.status() != WL_CONNECTED) {
    startBlink(RED_LED, 1, 300);
    return;
  }
  bool ok = bot.sendMessage(CHAT_ID, msg, "");
  if (ok) beep(1, 60);               // short confirmation beep
  else    startBlink(RED_LED, 1, 200);
  Serial.println(ok ? "[TG] OK: " + msg : "[TG] FAIL");
}

// ════════════════════════════════════════════════════════
//  WIFI — ROBUST RECONNECT
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
        tgSend("📡 WeatherKit offline — WiFi lost.");
        startBlink(RED_LED, 0, 600, true);
        markAlert(A_WIFI_LOST);
      }
    }
    if (now - lastReconnectMs >= 30000UL) {
      lastReconnectMs = now;
      if (now - wifiLostAtMs > 120000UL) {
        Serial.println("[WiFi] Full restart");
        WiFi.disconnect(true);
        delay(500);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      } else {
        Serial.println("[WiFi] Reconnect");
        WiFi.reconnect();
      }
    }
  } else {
    if (!wifiWasUp) {
      wifiWasUp = true;
      stopBlink();
      Serial.println("[WiFi] Back online");
      beep(2, 100, 60);              // 2 beeps on reconnect
      if (alertActive[A_WIFI_LOST]) {
        tgSend("✅ WeatherKit back online — " + WiFi.localIP().toString());
        alertActive[A_WIFI_LOST] = false;
      }
    }
  }
}

// ════════════════════════════════════════════════════════
//  FETCH WEATHER
// ════════════════════════════════════════════════════════
int sensorFails = 0;
int apiFails    = 0;

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
      prevOutTemp = out.temp;
      prevOutRain = out.rainChance;
      prevOutCode = out.code;

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
      Serial.printf("[Out] %.1fC %s UV%.0f\n",
                    out.temp, out.condition.c_str(), out.uvMax);
    } else {
      apiFails++;
      Serial.println("[API] Parse error");
    }
  } else {
    apiFails++;
    Serial.printf("[API] HTTP %d\n", code);
  }
  http.end();

  if (apiFails >= 3 && !alertActive[A_API_FAIL]) {
    tgSend("🌐 Open-Meteo unreachable — no weather data.");
    startBlink(RED_LED, 3, 300);
    markAlert(A_API_FAIL);
  }
}

// ════════════════════════════════════════════════════════
//  CHECK ALL ALERTS
// ════════════════════════════════════════════════════════
void checkAlerts() {
  if (!out.ready) return;

  if (out.rainChance > 90 && canAlert(A_RAIN90)) {
    tgSend("🌧 Heavy rain " + String(out.rainChance) + "% — avoid travel.");
    startBlink(RED_LED, 3, 300); markAlert(A_RAIN90);
  } else if (out.rainChance > 70 && canAlert(A_RAIN70)) {
    tgSend("☔ Rain likely " + String(out.rainChance) + "% — carry umbrella.");
    startBlink(RED_LED, 2, 400); markAlert(A_RAIN70);
  }
  if (out.uvMax >= 10 && canAlert(A_UV10)) {
    tgSend("☠️ Extreme UV " + String(out.uvMax,0) + " — stay indoors.");
    startBlink(RED_LED, 5, 100); markAlert(A_UV10);
  } else if (out.uvMax >= 7 && canAlert(A_UV7)) {
    tgSend("🔆 High UV " + String(out.uvMax,0) + " — avoid 11AM-3PM.");
    startBlink(RED_LED, 3, 200); markAlert(A_UV7);
  }
  if (out.temp > 38 && canAlert(A_TEMP38)) {
    tgSend("🔥 Extreme heat " + String(out.temp,1) + "°C outside.");
    startBlink(RED_LED, 5, 100); markAlert(A_TEMP38);
  }
  if (out.temp < 10 && canAlert(A_TEMP10)) {
    tgSend("🥶 Unusually cold " + String(out.temp,1) + "°C outside.");
    startBlink(RED_LED, 2, 400); markAlert(A_TEMP10);
  }
  if (out.wind > 60 && canAlert(A_WIND60)) {
    tgSend("🌪️ Dangerous wind " + String(out.wind,0) + " km/h — stay inside.");
    startBlink(RED_LED, 5, 100); markAlert(A_WIND60);
  } else if (out.wind > 40 && canAlert(A_WIND40)) {
    tgSend("💨 Strong wind " + String(out.wind,0) + " km/h.");
    startBlink(RED_LED, 3, 400); markAlert(A_WIND40);
  }
  if ((out.feelsLike - out.temp) > 5 && canAlert(A_FEELS_HOT)) {
    tgSend("😓 Humidity adding heat — feels " + String(out.feelsLike,0) +
           "° vs " + String(out.temp,0) + "° actual.");
    markAlert(A_FEELS_HOT);
  }
  if ((out.temp - out.feelsLike) > 4 && canAlert(A_FEELS_COLD)) {
    tgSend("🌬️ Wind chill — feels " + String(out.feelsLike,0) +
           "° vs " + String(out.temp,0) + "° actual.");
    markAlert(A_FEELS_COLD);
  }
  if (out.code >= 95 && canAlert(A_STORM)) {
    tgSend("⛈️ Storm active — stay inside, close windows.");
    startBlink(RED_LED, 0, 500, true); markAlert(A_STORM);
  }
  if ((out.code == 45 || out.code == 48) && canAlert(A_FOG)) {
    tgSend("🌫️ Foggy outside — drive carefully.");
    startBlink(RED_LED, 2, 400); markAlert(A_FOG);
  }
  if ((out.rainChance - prevOutRain) > 40 && canAlert(A_RAPID_WEATHER)) {
    tgSend("⚡ Weather changing fast — rain jumped to " + String(out.rainChance) + "%.");
    startBlink(RED_LED, 4, 150); markAlert(A_RAPID_WEATHER);
  }
  if (prevOutCode < 95 && out.code >= 95 && canAlert(A_CODE_CHANGE)) {
    tgSend("🔄 Conditions turned stormy suddenly.");
    startBlink(RED_LED, 5, 100); markAlert(A_CODE_CHANGE);
  }
  if (room.temp > 36 && canAlert(A_ROOM_HOT36)) {
    tgSend("🚨 Room dangerously hot — " + String(room.temp,1) + "°C!");
    startBlink(RED_LED, 5, 100); markAlert(A_ROOM_HOT36);
  } else if (room.temp > 32 && canAlert(A_ROOM_HOT32)) {
    tgSend("🌡️ Room hot — " + String(room.temp,1) + "°C. Turn on fan.");
    startBlink(RED_LED, 3, 400); markAlert(A_ROOM_HOT32);
  }
  if (!rapidTracking) {
    rapidBase = room.temp; rapidStartMs = millis(); rapidTracking = true;
  } else if (millis() - rapidStartMs >= 600000UL) {
    float rise = room.temp - rapidBase;
    if (rise >= 3.0f && canAlert(A_ROOM_RISE)) {
      tgSend("📈 Room rose " + String(rise,1) +
             "°C in 10min — now " + String(room.temp,1) + "°C.");
      startBlink(RED_LED, 0, 600, true); markAlert(A_ROOM_RISE);
    }
    rapidBase = room.temp; rapidStartMs = millis();
  }
  if (room.hum > 80 && canAlert(A_HUM_HIGH)) {
    tgSend("💧 High humidity " + String(room.hum,0) + "% — mold risk.");
    startBlink(RED_LED, 2, 400); markAlert(A_HUM_HIGH);
  }
  if (room.hum < 20 && canAlert(A_HUM_LOW)) {
    tgSend("🏜️ Air too dry — " + String(room.hum,0) + "% humidity.");
    markAlert(A_HUM_LOW);
  }
  ntp.update();
  int nowH = ntp.getHours();
  if ((nowH >= 22 || nowH < 6) && room.temp > 30 && canAlert(A_NIGHT_HOT)) {
    tgSend("😴 Too hot to sleep — room " + String(room.temp,1) + "°C.");
    startBlink(RED_LED, 3, 400); markAlert(A_NIGHT_HOT);
  }
  float hi = heatIdx(out.temp, out.humidity);
  if (hi >= 45 && canAlert(A_HI_45)) {
    tgSend("🚨 Heat stroke danger — heat index " + String(hi,0) + "°C!");
    startBlink(RED_LED, 5, 100); markAlert(A_HI_45);
  } else if (hi >= 40 && canAlert(A_HI_40)) {
    tgSend("🏥 Heat exhaustion risk — index " + String(hi,0) + "°C.");
    startBlink(RED_LED, 4, 150); markAlert(A_HI_40);
  }
  float dp = out.temp - ((100.0f - out.humidity) / 5.0f);
  if (dp > 26 && canAlert(A_DEW_26)) {
    tgSend("😵 Unbearable moisture — dew point " + String(dp,0) + "°C.");
    startBlink(RED_LED, 3, 300); markAlert(A_DEW_26);
  } else if (dp > 24 && canAlert(A_DEW_24)) {
    tgSend("💦 Oppressive humidity — dew point " + String(dp,0) + "°C.");
    markAlert(A_DEW_24);
  }
  if ((nowH >= 22 || nowH < 6) &&
      (room.temp > 28 || room.hum > 70) && canAlert(A_BAD_SLEEP)) {
    tgSend("🥵 Poor sleep conditions — " + String(room.temp,1) +
           "°C  " + String(room.hum,0) + "%.");
    markAlert(A_BAD_SLEEP);
  }
  if (out.uvMax > 7 && out.temp > 35 && canAlert(A_NO_WORKOUT)) {
    tgSend("🚫 Skip outdoor workout — UV " + String(out.uvMax,0) +
           " + temp " + String(out.temp,0) + "°C.");
    markAlert(A_NO_WORKOUT);
  }
  if (room.temp > out.temp + 3 && room.temp > 30 && canAlert(A_NIGHT_HOT)) {
    tgSend("🪟 Open windows — outside " + String(out.temp,0) +
           "° cooler than room " + String(room.temp,0) + "°.");
    markAlert(A_NIGHT_HOT);
  }
  if (out.high > 38) hotStreak++; else hotStreak = 0;
  if (hotStreak >= 3 && canAlert(A_HEATWAVE)) {
    tgSend("🔥 Heatwave — 3+ days above 38°C. Stay hydrated.");
    startBlink(RED_LED, 5, 100); markAlert(A_HEATWAVE);
  }
  if (out.rainChance > 70) rainStreak++; else rainStreak = 0;
  if (rainStreak >= 3 && canAlert(A_RAINSTREAK)) {
    tgSend("🌧️ Extended rain — " + String(rainStreak) + " fetches above 70%.");
    markAlert(A_RAINSTREAK);
  }
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
  display.print("MY ROOM");
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
  if (out.ready) {
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
//  SCREEN 1 — OUTSIDE  (fixed grid layout)
//
//  ┌──────────────────────────────┐  y=0
//  │ OUTSIDE              28°    │  header + big temp
//  ├──────────────────────────────┤  y=17
//  │ PARTLY CLOUDY   FL:30°C     │  y=20
//  ├──────────────────────────────┤  y=30
//  │ WIND 22KM/H   UV  6         │  y=34
//  │ HUM  65%      RAIN 40%      │  y=44
//  ├──────────────────────────────┤  y=54
//  │ H:32°C              L:24°C  │  y=57
//  └──────────────────────────────┘
// ════════════════════════════════════════════════════════
void drawScreen1() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  if (!out.ready) {
    display.setTextSize(1);
    display.setCursor(2, 0);   display.print("OUTSIDE");
    display.drawLine(0, 9, 127, 9, WHITE);
    display.setCursor(16, 26); display.print("FETCHING...");
    display.display();
    return;
  }

  // ── Row 0: "OUTSIDE" left (size1) + big temp right (size2) ──
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

  // ── Divider below size-2 row ──
  display.drawLine(0, 17, 127, 17, WHITE);

  // ── Row 1: condition LEFT, feels-like RIGHT ──
  display.setTextSize(1);
  String cond = out.condition;
  if (cond.length() > 12) cond = cond.substring(0, 12);
  display.setCursor(2, 20);
  display.print(cond);

  char flbuf[10];
  sprintf(flbuf, "FL:%.0f\xF7""C", out.feelsLike);
  display.setCursor(128 - (int)strlen(flbuf) * 6, 20);
  display.print(flbuf);

  // ── Divider ──
  display.drawLine(0, 30, 127, 30, WHITE);

  // ── Rows 2-3: 2-column grid ──
  // Left col  x=2  :  WIND value  /  HUM value
  // Right col x=68 :  UV value    /  RAIN value

  // WIND
  display.setCursor(2, 34);
  display.print("WIND");
  char wbuf[9];
  sprintf(wbuf, "%.0fKM/H", out.wind);
  display.setCursor(26, 34);   // x = 2 + 4chars*6px
  display.print(wbuf);

  // UV
  display.setCursor(68, 34);
  display.print("UV");
  char uvbuf[5];
  sprintf(uvbuf, "%.0f", out.uvMax);
  display.setCursor(80, 34);   // x = 68 + 2chars*6px
  display.print(uvbuf);

  // HUM
  display.setCursor(2, 44);
  display.print("HUM");
  char hbuf[6];
  sprintf(hbuf, "%d%%", out.humidity);
  display.setCursor(20, 44);   // x = 2 + 3chars*6px
  display.print(hbuf);

  // RAIN
  display.setCursor(68, 44);
  display.print("RAIN");
  char rbuf[6];
  sprintf(rbuf, "%d%%", out.rainChance);
  display.setCursor(92, 44);   // x = 68 + 4chars*6px
  display.print(rbuf);

  // ── Divider ──
  display.drawLine(0, 54, 127, 54, WHITE);

  // ── Footer: H/L ──
  char hibuf[8], lobuf[8];
  sprintf(hibuf, "H:%.0f\xF7""C", out.high);
  sprintf(lobuf, "L:%.0f\xF7""C", out.low);
  display.setCursor(2, 57);
  display.print(hibuf);
  display.setCursor(128 - (int)strlen(lobuf) * 6, 57);
  display.print(lobuf);

  display.display();
}

// ════════════════════════════════════════════════════════
//  STARTUP ANIMATION
// ════════════════════════════════════════════════════════
void startup() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(4, 4);   display.print("WEATHER");
  display.setTextSize(1);
  display.setCursor(4, 24);  display.print("MIT-ADT  PUNE");
  display.setCursor(4, 34);  display.print("V4.1");
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
//  CONNECT WIFI (initial boot)
// ════════════════════════════════════════════════════════
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(2, 0);  display.print("WI-FI");
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
    display.print("CONNECTED");
    display.drawLine(0, 9, 127, 9, WHITE);
    display.setCursor(2, 13); display.print(WiFi.localIP().toString());
    beep(3, 80, 60);           // 3 beeps — boot success
  } else {
    display.print("OFFLINE");
    display.drawLine(0, 9, 127, 9, WHITE);
    display.setCursor(2, 13); display.print("ROOM ONLY MODE");
    startBlink(RED_LED, 3, 300);
    beep(1, 500);              // 1 long beep — boot fail
  }
  display.display();
  delay(1200);
}

// ════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n[WeatherKit V4.1]");

  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER,  OUTPUT);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER,  LOW);   // LOW = OFF for active-HIGH buzzer

  // Self-test: red LED flash + 1 beep
  digitalWrite(RED_LED, HIGH); delay(150);
  digitalWrite(RED_LED, LOW);  delay(80);
  beep(1, 100);                // 1 beep self-test

  memset(alertSentAt, 0, sizeof(alertSentAt));
  memset(alertActive, 0, sizeof(alertActive));

  dht.begin();
  Wire.begin(4, 5);            // SDA=GPIO4(D2)  SCL=GPIO5(D1)

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED not found"));
    while (true) delay(1000);
  }

  startup();
  connectWiFi();

  securedClient.setInsecure(); // Telegram TLS

  ntp.begin();
  ntp.update();

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(t) && !isnan(h)) {
    room.temp = t; room.hum = h; rapidBase = t;
  }

  fetchWeather();
  unsigned long now = millis();
  lastFetchMs = lastScreenMs = lastSensorMs = now;

  tgSend("🔌 WeatherKit V4.1 started — " + WiFi.localIP().toString());
  drawScreen0();
}

// ════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  updateBlink();
  wifiLoop();

  if (now - lastFetchMs >= FETCH_MS) {
    fetchWeather();
    lastFetchMs = now;
    checkAlerts();
  }

  if (now - lastScreenMs >= SCREEN_MS) {
    currentScreen = (currentScreen + 1) % NUM_SCREENS;
    lastScreenMs  = now;
    display.clearDisplay();
    display.display();
    delay(25);
    currentScreen == 0 ? drawScreen0() : drawScreen1();
    return;
  }

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
  Serial.printf("[Room] %.1fC  %.1f%%\n", t, h);

  if (currentScreen == 0) drawScreen0();
}
