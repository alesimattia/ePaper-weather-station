#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <FS.h>
#include <time.h>

#include <Wire.h>
#include <Adafruit_BME280.h>
#include <ArduinoJson.h>

#include <GxEPD2_4C.h>
#include "epd4c/GxEPD2_420c_GDEY0420F51.h"

// Font SANS (Adafruit GFX)
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

#include "config.h"
#include "icons.h"
#include "image.h"   // fallback offline (img_test 300x200)

// --- Pin SPI display (Waveshare E-Paper ESP32 Driver Board) ---
static const int EPD_MOSI = 14;
static const int EPD_SCK  = 13;
static const int EPD_CS   = 15;
static const int EPD_DC   = 27;
static const int EPD_RST  = 26;
static const int EPD_BUSY = 25;
static const int EPD_MISO = 12;

// Display
GxEPD2_4C<GxEPD2_420c_GDEY0420F51, GxEPD2_420c_GDEY0420F51::HEIGHT>
display(GxEPD2_420c_GDEY0420F51(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// BME280
Adafruit_BME280 bme;

// RTC persistence
RTC_DATA_ATTR uint32_t rtc_last_weather_epoch = 0;
RTC_DATA_ATTR uint32_t rtc_last_img_epoch = 0;
RTC_DATA_ATTR uint32_t rtc_sunrise_epoch = 0;
RTC_DATA_ATTR uint32_t rtc_sunset_epoch = 0;

struct WeatherSlotRTC {
  char mainStr[16];
  int16_t temp_x10;
  uint32_t dt_epoch; // slot0: download time; slot1..3: forecast dt
  bool valid;
};
RTC_DATA_ATTR WeatherSlotRTC rtc_weather[4];

// Immagine di sfondo XBM 300x200
static const int IMG_W = 300;
static const int IMG_H = 200;
static const char* IMG_CACHE_PATH = "/img.xbm";

// Dimensioni icone (dal tuo icons.h)
static const int WX_W = 48;
static const int WX_H = 48;
static const int SM_W = 16;
static const int SM_H = 16;

// ---------- Icon wrapper ----------
struct IconXBM {
  const uint8_t* bits;
  uint16_t w;
  uint16_t h;
};

// ---------- Time helpers ----------
static uint32_t nowEpoch()
{
  time_t now = time(nullptr);
  if (now < 1700000000) return 0; // tempo non valido / non sincronizzato
  return (uint32_t)now;
}

static void fmtHHMM(uint32_t epoch, char* out, size_t outLen)
{
  if (!epoch) { snprintf(out, outLen, "--:--"); return; }
  time_t t = (time_t)epoch;
  struct tm tmLocal;
  localtime_r(&t, &tmLocal);
  snprintf(out, outLen, "%02d:%02d", tmLocal.tm_hour, tmLocal.tm_min);
}

static uint32_t remainingSec(uint32_t lastEpoch, uint32_t intervalMin)
{
  uint32_t now = nowEpoch();
  if (!now || !lastEpoch) return 0;
  uint32_t elapsed = now - lastEpoch;
  uint32_t interval = intervalMin * 60UL;
  if (elapsed >= interval) return 0;
  return interval - elapsed;
}

static bool isNightAt(uint32_t epoch)
{
  if (!epoch || !rtc_sunrise_epoch || !rtc_sunset_epoch) return false;
  // Notte: dopo tramonto oppure prima alba
  return (epoch >= rtc_sunset_epoch) || (epoch < rtc_sunrise_epoch);
}

// ---------- Weather icon selection (DAY/NIGHT) ----------
static IconXBM getWeatherIcon(const char* mainStr, uint32_t whenEpoch)
{
  bool night = isNightAt(whenEpoch);

  if (!mainStr) {
    return { night ? (const uint8_t*)img_cloudy_night : (const uint8_t*)img_cloudy, WX_W, WX_H };
  }

  // Clear
  if (strcmp(mainStr, "Clear") == 0) {
    return { night ? (const uint8_t*)img_sunny_night : (const uint8_t*)img_sunny, WX_W, WX_H };
  }

  // Clouds
  if (strcmp(mainStr, "Clouds") == 0) {
    return { night ? (const uint8_t*)img_cloudy_night : (const uint8_t*)img_cloudy, WX_W, WX_H };
  }

  // Rain / Drizzle
  if (strcmp(mainStr, "Rain") == 0 || strcmp(mainStr, "Drizzle") == 0) {
    return { night ? (const uint8_t*)img_rain_night : (const uint8_t*)img_rain, WX_W, WX_H };
  }

  // Snow
  if (strcmp(mainStr, "Snow") == 0) {
    return { (const uint8_t*)img_snow, WX_W, WX_H };
  }

  // Thunderstorm
  if (strcmp(mainStr, "Thunderstorm") == 0) {
    return { (const uint8_t*)img_thunder, WX_W, WX_H };
  }

  // Fog/Mist/Haze
  if (strcmp(mainStr, "Fog") == 0 || strcmp(mainStr, "Mist") == 0 || strcmp(mainStr, "Haze") == 0) {
    return { (const uint8_t*)img_fog, WX_W, WX_H };
  }

  // default
  return { night ? (const uint8_t*)img_cloudy_night : (const uint8_t*)img_cloudy, WX_W, WX_H };
}

// ---------- WiFi ----------
static bool connectWiFi(uint32_t timeoutMs = 15000)
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void syncNTP()
{
  // TZ Europa/Roma con DST automatico
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov", "time.google.com");
  for (int i = 0; i < 30; i++) {
    if (nowEpoch() != 0) return;
    delay(200);
  }
}

static void shutdownWiFi()
{
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  btStop();
}

// ---------- Parse XBM bytes from C array text/plain ----------
// Esempio input contiene:  const unsigned uint8_t my_img[] PROGMEM = { 0x00, 0xFF, ... };
static int hexValue(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

// Estrae bytes tra '{' e '}' riconoscendo token 0xNN o NN (decimale)
static bool parseCArrayToFile(const String& text, const char* outPath)
{
  int start = text.indexOf('{');
  int end   = text.lastIndexOf('}');
  if (start < 0 || end < 0 || end <= start) return false;

  File f = SPIFFS.open(outPath, FILE_WRITE);
  if (!f) return false;

  int i = start + 1;
  uint32_t written = 0;

  while (i < end) {
    // salta spazi, virgole, newline
    while (i < end) {
      char c = text[i];
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ',') i++;
      else break;
    }
    if (i >= end) break;

    // gestisci commenti C/C++ in modo semplice
    if (i + 1 < end && text[i] == '/' && text[i+1] == '/') {
      int nl = text.indexOf('\n', i);
      if (nl < 0) break;
      i = nl + 1;
      continue;
    }
    if (i + 1 < end && text[i] == '/' && text[i+1] == '*') {
      int close = text.indexOf("*/", i+2);
      if (close < 0) break;
      i = close + 2;
      continue;
    }

    uint8_t val = 0;

    // hex 0xNN
    if (i + 2 < end && text[i] == '0' && (text[i+1] == 'x' || text[i+1] == 'X')) {
      int h1 = hexValue(text[i+2]);
      int h2 = (i + 3 < end) ? hexValue(text[i+3]) : -1;
      if (h1 < 0) break;
      if (h2 < 0) { // 0xN
        val = (uint8_t)h1;
        i += 3;
      } else {
        val = (uint8_t)((h1 << 4) | h2);
        i += 4;
      }
      f.write(&val, 1);
      written++;
      continue;
    }

    // decimale (es. 255)
    if (isDigit((unsigned char)text[i])) {
      int j = i;
      uint32_t num = 0;
      while (j < end && isDigit((unsigned char)text[j])) {
        num = num * 10 + (text[j] - '0');
        j++;
      }
      val = (uint8_t)(num & 0xFF);
      i = j;
      f.write(&val, 1);
      written++;
      continue;
    }

    // token sconosciuto: avanza (evita loop infinito)
    i++;
  }

  f.close();

  // Per 300x200: bytes = 300*200/8 = 7500
  if (written < (IMG_W * IMG_H) / 8) return false;
  return true;
}

static bool downloadXbmTextAndCache(const char* url, const char* outPath)
{
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  String payload = http.getString();
  http.end();

  return parseCArrayToFile(payload, outPath);
}

// ---------- Draw cached XBM from SPIFFS ----------
static bool drawXbmFromSpiffs(const char* path, int x, int y, int w, int h, uint16_t color)
{
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return false;

  const size_t needed = (size_t)w * (size_t)h / 8;
  if (f.size() < (int)needed) { f.close(); return false; }

  uint8_t* buf = (uint8_t*)malloc(needed);
  if (!buf) { f.close(); return false; }

  size_t n = f.read(buf, needed);
  f.close();
  if (n != needed) { free(buf); return false; }

  display.drawXBitmap(x, y, buf, w, h, color);
  free(buf);
  return true;
}

static void drawFallbackImageH()
{
  // image.h: img_test 300x200
  display.drawXBitmap(0, 0, img_test, IMG_W, IMG_H, GxEPD_BLACK);
}

// ---------- OpenWeatherMap ----------
static bool fetchWeatherCurrent(WeatherSlotRTC& slot0, uint32_t& sunriseOut, uint32_t& sunsetOut)
{
  char url[256];
  snprintf(url, sizeof(url),
           "http://api.openweathermap.org/data/2.5/weather?lat=%.6f&lon=%.6f&appid=%s&units=%s&lang=%s",
           LAT, LON, OWM_API_KEY, OWM_UNITS, OWM_LANG);

  HTTPClient http;
  if (!http.begin(url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<3072> doc;
  if (deserializeJson(doc, payload)) return false;

  float temp = doc["main"]["temp"] | NAN;
  const char* mainStr = doc["weather"][0]["main"] | "";

  sunriseOut = doc["sys"]["sunrise"] | 0;
  sunsetOut  = doc["sys"]["sunset"]  | 0;

  memset(&slot0, 0, sizeof(slot0));
  strncpy(slot0.mainStr, mainStr, sizeof(slot0.mainStr) - 1);
  slot0.temp_x10 = (int16_t)lroundf(temp * 10.0f);

  // per corrente: orario di scaricamento
  slot0.dt_epoch = nowEpoch();
  slot0.valid = true;
  return true;
}

static bool fetchWeatherForecast3(WeatherSlotRTC& s1, WeatherSlotRTC& s2, WeatherSlotRTC& s3)
{
  char url[256];
  snprintf(url, sizeof(url),
           "http://api.openweathermap.org/data/2.5/forecast?lat=%.6f&lon=%.6f&appid=%s&units=%s&lang=%s&cnt=3",
           LAT, LON, OWM_API_KEY, OWM_UNITS, OWM_LANG);

  HTTPClient http;
  if (!http.begin(url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<8192> doc;
  if (deserializeJson(doc, payload)) return false;

  JsonArray list = doc["list"].as<JsonArray>();
  if (list.size() < 3) return false;

  WeatherSlotRTC* slots[3] = { &s1, &s2, &s3 };

  for (int i = 0; i < 3; i++) {
    JsonObject it = list[i];
    float temp = it["main"]["temp"] | NAN;
    const char* mainStr = it["weather"][0]["main"] | "";
    uint32_t dt = it["dt"] | 0;

    memset(slots[i], 0, sizeof(WeatherSlotRTC));
    strncpy(slots[i]->mainStr, mainStr, sizeof(slots[i]->mainStr) - 1);
    slots[i]->temp_x10 = (int16_t)lroundf(temp * 10.0f);
    slots[i]->dt_epoch = dt;
    slots[i]->valid = true;
  }
  return true;
}

// ---------- UI Drawing ----------
static void drawRightOverlay(float tC, float rh, float p_hPa)
{
  // overlay alto-destro: 100x200 (x=300..399, y=0..199)
  const int x = 300, y = 0, w = 100, h = 200;
  display.fillRect(x, y, w, h, GxEPD_BLACK);

  // Orario attuale (font più piccolo così entra)
  char hhmm[8]; fmtHHMM(nowEpoch(), hhmm, sizeof(hhmm));
  display.setTextColor(GxEPD_WHITE);
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(x + 8, y + 22);
  display.print(hhmm);

  display.setFont(&FreeSans9pt7b);
  display.setTextColor(GxEPD_WHITE);

  int cy = y + 48;

  // TEMP: icona rossa + testo bianco
  display.drawXBitmap(x + 6, cy - 12, (const uint8_t*)img_temperature, SM_W, SM_H, GxEPD_RED);
  display.setCursor(x + 26, cy);
  display.printf("%.1fC", tC);
  cy += 16;

  // HUM: icona rossa + testo bianco
  display.drawXBitmap(x + 6, cy - 12, (const uint8_t*)img_humidity, SM_W, SM_H, GxEPD_RED);
  display.setCursor(x + 26, cy);
  display.printf("%.0f%%", rh);
  cy += 16;

  // PRESS: solo testo bianco
  display.setCursor(x + 6, cy);
  display.printf("P %.0fh", p_hPa);
  cy += 16;

  // Spazio extra prima di alba/tramonto
  cy += 8;

  // Alba / Tramonto con icone gialle
  char sr[8], ss[8];
  fmtHHMM(rtc_sunrise_epoch, sr, sizeof(sr));
  fmtHHMM(rtc_sunset_epoch,  ss, sizeof(ss));

  // Alba (giallo)
  display.drawXBitmap(x + 6, cy - 12, (const uint8_t*)img_sunrise, SM_W, SM_H, GxEPD_YELLOW);
  display.setCursor(x + 26, cy);
  display.print(sr);
  cy += 16;

  // Tramonto (giallo)
  display.drawXBitmap(x + 6, cy - 12, (const uint8_t*)img_sunset, SM_W, SM_H, GxEPD_YELLOW);
  display.setCursor(x + 26, cy);
  display.print(ss);
  cy += 16;

  // Spazio extra prima di "Next:"
  cy += 8;

  // Next: tempo rimanente (minuti) per weather e img
  uint32_t rw = remainingSec(rtc_last_weather_epoch, WEATHER_INTERVAL_MIN);
  uint32_t ri = remainingSec(rtc_last_img_epoch, IMG_INTERVAL_MIN);

  uint32_t rwMin = (rw + 59) / 60;
  uint32_t riMin = (ri + 59) / 60;

  display.setCursor(x + 6, cy);
  display.print("Next:");
  cy += 16;
  display.setCursor(x + 6, cy);
  display.printf("W %lum I %lum", (unsigned long)rwMin, (unsigned long)riMin);
}

static void drawBottomWeatherBar()
{
  // overlay basso: y=200..299, h=100, nero
  const int y = 200;
  display.fillRect(0, y, 400, 100, GxEPD_BLACK);

  // separatori verticali: current | next1 | next2 | next3
  for (int sx : {100, 200, 300}) {
    display.drawLine(sx, y + 6, sx, y + 94, GxEPD_WHITE);
  }

  auto drawSlot = [&](int idx, int x0, int w)
  {
    if (!rtc_weather[idx].valid) return;

    // Se quandoEpoch (slot0 = download time, slot future = dt forecast) è dopo tramonto -> night icon
    IconXBM ic = getWeatherIcon(rtc_weather[idx].mainStr, rtc_weather[idx].dt_epoch);

    // icona centrata nel blocco (giallo)
    int ix = x0 + (w - (int)ic.w) / 2;
    int iy = y + 8;
    display.drawXBitmap(ix, iy, ic.bits, ic.w, ic.h, GxEPD_YELLOW);

    // Temperatura (rossa)
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_RED);
    display.setCursor(x0 + 8, y + 74);
    display.printf("%.1fC", rtc_weather[idx].temp_x10 / 10.0f);

    // Orario (bianco)
    char hhmm[8];
    fmtHHMM(rtc_weather[idx].dt_epoch, hhmm, sizeof(hhmm));

    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(x0 + 8, y + 92);
    display.print(hhmm);
  };

  drawSlot(0, 0,   100);
  drawSlot(1, 100, 100);
  drawSlot(2, 200, 100);
  drawSlot(3, 300, 100);
}

static void renderScreen(float tC, float rh, float p_hPa, bool forceFallbackImage)
{
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // Disegna immagine 300x200 a sinistra:
    // - se forceFallbackImage = true (niente internet / download fallito) => image.h
    // - altrimenti prova SPIFFS; se manca => image.h
    if (forceFallbackImage) {
      drawFallbackImageH();
    } else {
      bool ok = drawXbmFromSpiffs(IMG_CACHE_PATH, 0, 0, IMG_W, IMG_H, GxEPD_BLACK);
      if (!ok) drawFallbackImageH();
    }

    // Overlay destro alto (0..199)
    drawRightOverlay(tC, rh, p_hPa);

    // Barra meteo in basso (200..299)
    drawBottomWeatherBar();

  } while (display.nextPage());

  display.hibernate();
}

// ---------- Main ----------
void setup()
{
  Serial.begin(115200);
  delay(100);

  SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed!");
  }

  Wire.begin();
  bool bmeOk = bme.begin(0x76);
  if (!bmeOk) bmeOk = bme.begin(0x77);
  if (!bmeOk) Serial.println("BME280 not found!");

  display.init(115200, true, 2, false);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool coldBoot = (cause != ESP_SLEEP_WAKEUP_TIMER);

  // Leggi BME sempre
  float tC = NAN, rh = NAN, p_hPa = NAN;
  if (bmeOk) {
    tC = bme.readTemperature();
    rh = bme.readHumidity();
    p_hPa = bme.readPressure() / 100.0f;
  }

  // Decide se serve WiFi
  uint32_t now = nowEpoch();
  bool haveImg = SPIFFS.exists(IMG_CACHE_PATH);

  bool needWiFi = coldBoot;

  if (!needWiFi) {
    if (!now) needWiFi = true;           // per NTP
    if (!haveImg) needWiFi = true;       // cache immagine assente
    if (rtc_last_img_epoch == 0 || (now && (now - rtc_last_img_epoch) >= IMG_INTERVAL_MIN * 60UL)) needWiFi = true;
    if (rtc_last_weather_epoch == 0 || (now && (now - rtc_last_weather_epoch) >= WEATHER_INTERVAL_MIN * 60UL)) needWiFi = true;
    if (!rtc_weather[0].valid) needWiFi = true;
  }

  // flags per fallback display
  bool forceFallbackImageThisBoot = false;

  bool imgDue = !SPIFFS.exists(IMG_CACHE_PATH) ||
                (rtc_last_img_epoch == 0) ||
                (now && (now - rtc_last_img_epoch) >= IMG_INTERVAL_MIN * 60UL);

  bool weatherDue = (rtc_last_weather_epoch == 0) ||
                    (now && (now - rtc_last_weather_epoch) >= WEATHER_INTERVAL_MIN * 60UL) ||
                    (!rtc_weather[0].valid);

  if (needWiFi) {
    // Se non si connette entro 15s: spegni WiFi e mostra image.h al posto di ciò che doveva scaricare
    bool wifiOk = connectWiFi(15000);
    if (wifiOk) {
      syncNTP();
      now = nowEpoch();

      // IMG: aggiorna se scaduta o assente
      if (imgDue) {
        Serial.println("Downloading XBM text and caching...");
        // Se non riesce a scaricare l'immagine dall'endpoint mostra l'immagine di image.h.
        // NON sovrascrivere la cache se fallisce.
        if (downloadXbmTextAndCache(IMG_URL, IMG_CACHE_PATH)) {
          rtc_last_img_epoch = nowEpoch(); // timestamp download immagine
        } else {
          Serial.println("XBM download/parse failed -> using image.h fallback this boot");
          forceFallbackImageThisBoot = true;
        }
      }

      // METEO: aggiorna se scaduto o assente
      if (weatherDue) {
        Serial.println("Fetching weather...");
        WeatherSlotRTC s0, s1, s2, s3;
        uint32_t sunrise = 0, sunset = 0;

        bool ok0 = fetchWeatherCurrent(s0, sunrise, sunset);
        bool okF = fetchWeatherForecast3(s1, s2, s3);

        if (ok0) {
          rtc_weather[0] = s0;
          rtc_sunrise_epoch = sunrise;
          rtc_sunset_epoch  = sunset;
        }
        if (okF) {
          rtc_weather[1] = s1;
          rtc_weather[2] = s2;
          rtc_weather[3] = s3;
        }
        if (ok0 && okF) rtc_last_weather_epoch = nowEpoch();
      }

    } else {
      Serial.println("WiFi connect failed (15s). WiFi OFF until next wake.");
      // Se in questo giro era previsto un update immagine -> fallback
      if (imgDue) forceFallbackImageThisBoot = true;
    }

    shutdownWiFi();
  }

  // Render sempre
  renderScreen(tC, rh, p_hPa, forceFallbackImageThisBoot);

  // Deep sleep 1 minuto
  esp_sleep_enable_timer_wakeup((uint64_t)WAKE_INTERVAL_MIN * 60ULL * 1000000ULL);
  Serial.println("Deep sleep...");
  esp_deep_sleep_start();
}

void loop() {}
