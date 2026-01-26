#pragma once

// WiFi
static const char* WIFI_SSID = "";
static const char* WIFI_PASS = "";

// OpenWeatherMap
static const char* OWM_API_KEY = "YOUR_API_KEY";
static const float LAT = 99.9999;
static const float LON = 99.9999;
static const char* OWM_LANG  = "it";
static const char* OWM_UNITS = "metric"; // metric / imperial / standard

// Endpoint immagine (BMP 300x200)
static const char* IMG_URL = "http://www.miosito.it/img.bmp";

// NTP / timezone (Europe/Rome con DST)
static const char* TZ_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3"; // DST auto

// Intervalli (minuti)
static const uint32_t WAKE_INTERVAL_MIN   = 1;
static const uint32_t WEATHER_INTERVAL_MIN = 20;
static const uint32_t IMG_INTERVAL_MIN     = 12;
