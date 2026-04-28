// ════════════════════════════════════════════════════════
//  WeatherKit — MIT ADT Pune
//  Screen 1 = YOUR ROOM  (DHT11 sensor only)
//  Screen 2 = OUTSIDE    (Open-Meteo API only)
//  LED: RED=D5/GPIO14  GREEN=D8/GPIO15
// ════════════════════════════════════════════════════════

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUDP.h>
#include <time.h>

// ── Display ──────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── DHT11 ────────────────────────────────────────────────
#define DHTPIN   2
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// ── LEDs ─────────────────────────────────────────────────
// GPIO12/13 = SPI pins on ESP8266 — never use for LEDs!
#define RED_LED    14    // D5
#define GREEN_LED  15    // D8

// ── WiFi ─────────────────────────────────────────────────
const char* WIFI_SSID     = "Manet Hostel@103";
const char* WIFI_PASSWORD = "MitHostel@103";

// ── Open-Meteo ───────────────────────────────────────────
const char* WEATHER_URL =
  "http://api.open-meteo.com/v1/forecast"
  "?latitude=18.4926&longitude=74.0255"
  "&current_weather=true"
  "&hourly=relativehumidity_2m,apparent_temperature,"
  "precipitation_probability"
  "&daily=temperature_2m_max,temperature_2m_min"
  "&timezone=Asia%2FKolkata"
  "&forecast_days=1";

// ── NTP IST ──────────────────────────────────────────────
WiFiUDP   ntpUDP;
NTPClient ntp(ntpUDP, "pool.ntp.org", 19800);

// ── Outside weather (API only) ───────────────────────────
float  outTemp       = 0;
float  outFeelsLike  = 0;
float  outHigh       = 0;
float  outLow        = 0;
float  outWind       = 0;
int    outHumidity   = 0;
int    outRainChance = 0;
int    outCode       = 0;
String outCondition  = "---";
bool   outDataReady  = false;

// ── Room sensor (DHT11 only) ─────────────────────────────
float roomTemp     = 0;
float roomHum      = 0;
float prevRoomTemp = 0;
bool  firstRun     = true;

// ── Timers ────────────────────────────────────────────────
unsigned long lastSensorRead   = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastScreenSwitch = 0;
unsigned long ledOnTime        = 0;
bool          ledActive        = false;
int           activeLed        = 0;
int           currentScreen    = 0;

#define SENSOR_INTERVAL   3000
#define WEATHER_INTERVAL  300000
#define SCREEN_SWITCH     7000

// ────────────────────────────────────────────────────────
String wmoDesc(int code) {
  if (code == 0)                return "Clear Sky";
  if (code == 1)                return "Mainly Clear";
  if (code == 2)                return "Partly Cloudy";
  if (code == 3)                return "Overcast";
  if (code == 45 || code == 48) return "Foggy";
  if (code >= 51 && code <= 55) return "Drizzle";
  if (code >= 61 && code <= 65) return "Rainy";
  if (code >= 71 && code <= 75) return "Snowy";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 85 && code <= 86) return "Snow Shower";
  if (code >= 95 && code <= 99) return "Thunderstorm";
  return "Unknown";
}

// ────────────────────────────────────────────────────────
void triggerLed(int pin) {
  if (ledActive) digitalWrite(activeLed, LOW);
  digitalWrite(pin, HIGH);
  activeLed = pin;
  ledOnTime = millis();
  ledActive = true;
}

// ────────────────────────────────────────────────────────
void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 1);
  display.print("Wi-Fi");
  display.drawLine(0, 11, 128, 11, WHITE);
  display.setCursor(0, 18);
  display.print("Connecting...");
  display.setCursor(0, 30);
  String s = String(WIFI_SSID);
  if (s.length() > 20) s = s.substring(0, 20);
  display.print(s);
  display.display();

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    display.fillRect(0, 44, 128, 10, BLACK);
    display.setCursor(0, 46);
    for (int d = 0; d < (attempts % 4) + 1; d++) display.print(". ");
    display.display();
    delay(300);
    attempts++;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 1);
  if (WiFi.status() == WL_CONNECTED) {
    display.print("Wi-Fi");
    display.drawLine(0, 11, 128, 11, WHITE);
    display.setCursor(0, 18);
    display.print("Connected");
    display.setCursor(0, 30);
    display.print(WiFi.localIP().toString());
  } else {
    display.print("Offline");
    display.drawLine(0, 11, 128, 11, WHITE);
    display.setCursor(0, 22);
    display.print("No network.");
    display.setCursor(0, 34);
    display.print("Room data only.");
  }
  display.display();
  delay(1400);
}

// ────────────────────────────────────────────────────────
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  http.begin(client, WEATHER_URL);
  http.setTimeout(10000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> filter;
    filter["current_weather"]["temperature"] = true;
    filter["current_weather"]["windspeed"]   = true;
    filter["current_weather"]["weathercode"] = true;
    filter["daily"]["temperature_2m_max"]    = true;
    filter["daily"]["temperature_2m_min"]    = true;
    filter["hourly"]["relativehumidity_2m"]  = true;
    filter["hourly"]["apparent_temperature"] = true;
    filter["hourly"]["precipitation_probability"] = true;

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload,
                               DeserializationOption::Filter(filter));
    if (!err) {
      outTemp       = doc["current_weather"]["temperature"].as<float>();
      outWind       = doc["current_weather"]["windspeed"].as<float>();
      outCode       = doc["current_weather"]["weathercode"].as<int>();
      outCondition  = wmoDesc(outCode);
      outHigh       = doc["daily"]["temperature_2m_max"][0].as<float>();
      outLow        = doc["daily"]["temperature_2m_min"][0].as<float>();
      outHumidity   = doc["hourly"]["relativehumidity_2m"][0].as<int>();
      outFeelsLike  = doc["hourly"]["apparent_temperature"][0].as<float>();
      outRainChance = doc["hourly"]["precipitation_probability"][0].as<int>();
      outDataReady  = true;
      Serial.printf("[Outside] %.1fC  %s\n", outTemp, outCondition.c_str());
    }
  }
  http.end();
}

// ════════════════════════════════════════════════════════
//  SCREEN 1 — MY ROOM (DHT11 only, no weather data)
// ════════════════════════════════════════════════════════
void drawRoomScreen(float temp, float hum, int dir) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // Header
  display.setCursor(0, 1);
  display.print("MY ROOM");

  ntp.update();
  time_t epoch = ntp.getEpochTime();
  struct tm* t = localtime(&epoch);
  const char* days[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  const char* months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                           "JUL","AUG","SEP","OCT","NOV","DEC"};
  char dateBuf[14];
  sprintf(dateBuf, "%s %02d %s", days[t->tm_wday], t->tm_mday, months[t->tm_mon]);
  display.setCursor(128 - strlen(dateBuf) * 6, 1);
  display.print(dateBuf);

  display.drawLine(0, 11, 128, 11, WHITE);

  // Big clock
  int h = ntp.getHours();
  int m = ntp.getMinutes();
  char timeBuf[6];
  sprintf(timeBuf, "%02d:%02d", h, m);
  display.setTextSize(2);
  display.setCursor(24, 13);
  display.print(timeBuf);
  display.setTextSize(1);
  display.setCursor(113, 15);
  display.print(h < 12 ? "AM" : "PM");

  display.drawLine(0, 33, 128, 33, WHITE);

  // Labels
  display.setTextSize(1);
  display.setCursor(0, 36);
  display.print("ROOM TEMP");
  display.setCursor(72, 36);
  display.print("HUMIDITY");

  // Big values
  display.setTextSize(2);
  char tbuf[8];
  sprintf(tbuf, "%.1f", temp);
  display.setCursor(0, 47);
  display.print(tbuf);
  display.setTextSize(1);
  display.print("\xF7""C");

  // Direction indicator
  if (dir == 1) {
    display.setCursor(58, 50);
    display.print("^");
  } else if (dir == -1) {
    display.setCursor(58, 50);
    display.print("v");
  }

  display.setTextSize(2);
  char hbuf[5];
  sprintf(hbuf, "%d%%", (int)hum);
  display.setCursor(72, 47);
  display.print(hbuf);

  display.display();
}

// ════════════════════════════════════════════════════════
//  SCREEN 2 — OUTSIDE WEATHER (API only, no sensor data)
// ════════════════════════════════════════════════════════
void drawOutsideScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // Header
  display.setCursor(0, 1);
  display.print("OUTSIDE");
  display.setCursor(60, 1);
  display.print("Loni Kalbhor");
  display.drawLine(0, 11, 128, 11, WHITE);

  if (!outDataReady) {
    display.setCursor(14, 26);
    display.print("Fetching weather");
    display.setCursor(28, 38);
    display.print("please wait...");
    display.display();
    return;
  }

  // Condition + big temp
  String cond = outCondition;
  if (cond.length() > 14) cond = cond.substring(0, 14);
  display.setCursor(0, 14);
  display.print(cond);

  char tempBuf[6];
  sprintf(tempBuf, "%.0f", outTemp);
  display.setTextSize(2);
  int tx = 128 - (strlen(tempBuf) * 12) - 12;
  display.setCursor(tx, 12);
  display.print(tempBuf);
  display.setTextSize(1);
  display.print("\xF7""C");

  // Feels like
  char flBuf[20];
  sprintf(flBuf, "Feels like %.0f\xF7""C", outFeelsLike);
  display.setCursor(0, 26);
  display.print(flBuf);

  display.drawLine(0, 35, 128, 35, WHITE);

  // H / L / Humidity
  char hiBuf[8], loBuf[8], humBuf[8];
  sprintf(hiBuf, "H:%.0f\xF7", outHigh);
  sprintf(loBuf, "L:%.0f\xF7", outLow);
  sprintf(humBuf, "Hum:%d%%", outHumidity);
  display.setCursor(0,  38); display.print(hiBuf);
  display.setCursor(36, 38); display.print(loBuf);
  display.setCursor(72, 38); display.print(humBuf);

  // Wind / Rain
  char windBuf[14], rainBuf[10];
  sprintf(windBuf, "Wind:%.0fkm/h", outWind);
  sprintf(rainBuf, "Rain:%d%%", outRainChance);
  display.setCursor(0,  52); display.print(windBuf);
  display.setCursor(72, 52); display.print(rainBuf);

  display.display();
}

// ────────────────────────────────────────────────────────
void glideRoom(float fromT, float toT, float fromH, float toH, int dir) {
  for (int i = 1; i <= 8; i++) {
    float t = fromT + (toT - fromT) * i / 8.0f;
    float h = fromH + (toH - fromH) * i / 8.0f;
    drawRoomScreen(t, h, dir);
    delay(20);
  }
}

// ────────────────────────────────────────────────────────
void startup() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(4, 10);
  display.print("Weather");
  display.setTextSize(1);
  display.setCursor(36, 32);
  display.print("MIT-ADT Pune");
  display.drawRect(24, 52, 80, 3, WHITE);
  display.display();
  for (int p = 2; p <= 78; p += 3) {
    display.fillRect(25, 53, p, 1, WHITE);
    display.display();
    delay(16);
  }
  delay(500);
  for (int y = 0; y < SCREEN_HEIGHT; y += 2) {
    display.fillRect(0, y, SCREEN_WIDTH, 2, BLACK);
    display.display();
    delay(4);
  }
}

// ════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  pinMode(RED_LED,   OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  digitalWrite(RED_LED,   LOW);
  digitalWrite(GREEN_LED, LOW);

  // LED self-test
  digitalWrite(RED_LED,   HIGH); delay(200); digitalWrite(RED_LED,   LOW); delay(100);
  digitalWrite(GREEN_LED, HIGH); delay(200); digitalWrite(GREEN_LED, LOW);

  dht.begin();
  Wire.begin(4, 5);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED not found"));
    while (true) delay(1000);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);

  startup();
  connectWiFi();

  ntp.begin();
  ntp.update();

  fetchWeather();
  lastWeatherFetch = millis();
}

// ════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // LED off
  if (ledActive && now - ledOnTime >= 150) {
    digitalWrite(activeLed, LOW);
    ledActive = false;
  }

  // Weather refresh every 5 min
  if (now - lastWeatherFetch >= WEATHER_INTERVAL) {
    fetchWeather();
    lastWeatherFetch = now;
  }

  // Screen rotate every 7s
  if (now - lastScreenSwitch >= SCREEN_SWITCH) {
    currentScreen = (currentScreen + 1) % 2;
    lastScreenSwitch = now;
    display.clearDisplay();
    display.display();
    delay(40);
    if (currentScreen == 0) drawRoomScreen(roomTemp, roomHum, 0);
    else                    drawOutsideScreen();
    return;
  }

  // Sensor every 3s
  if (now - lastSensorRead < SENSOR_INTERVAL) return;
  lastSensorRead = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(t) || isnan(h)) {
    Serial.println("[Sensor] Read failed");
    if (currentScreen == 0) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 1);
      display.print("MY ROOM");
      display.drawLine(0, 11, 128, 11, WHITE);
      display.setCursor(10, 26);
      display.print("Sensor error");
      display.setCursor(4, 40);
      display.print("Check D4 wiring");
      display.display();
    }
    return;
  }

  Serial.printf("[Room] %.1fC  %.1f%%\n", t, h);

  int dir = 0;
  if      (t > prevRoomTemp + 0.2f) { dir =  1; triggerLed(RED_LED);   }
  else if (t < prevRoomTemp - 0.2f) { dir = -1; triggerLed(GREEN_LED); }

  if (currentScreen == 0) {
    if (firstRun) { drawRoomScreen(t, h, dir); firstRun = false; }
    else          { glideRoom(roomTemp, t, roomHum, h, dir); }
  }

  roomTemp     = t;
  roomHum      = h;
  prevRoomTemp = t;
}
