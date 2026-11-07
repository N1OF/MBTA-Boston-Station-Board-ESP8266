#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include <pgmspace.h>

// ==================================================
// USER CONFIGURATION
// ==================================================

// Credentials live in secrets.h (not committed). Copy secrets.h.example to
// secrets.h in this folder and fill it in.
#include "secrets.h"

const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASS;
const char* MBTA_KEY      = SECRET_MBTA_KEY;   // may be "", but a key is recommended

#define LCD_ADDR 0x27
#define SDA_PIN 4
#define SCL_PIN 5

// RAM knobs. Each Arrival is 44 bytes.
// *_PER_STATION is both the API page limit and the storage reserved per station.
#define PRED_PER_STATION   24
#define SCHED_PER_STATION  16
#define SCHED_WINDOW_MIN   90     // schedule look-ahead (commuter rail only)
#define MAX_ALERTS         6
#define ALERT_LEN          140
#define MAX_TRACKS         48

const uint32_t PRED_REFRESH  = 30000UL;   // per station (requests are staggered)
const uint32_t SCHED_REFRESH = 300000UL;  // per station
const uint32_t ALERT_REFRESH = 120000UL;
const uint32_t TRACK_REFRESH = 600000UL;

const uint32_t SCREEN_TIME = 9000UL;
const uint32_t TITLE_TIME  = 1500UL;      // station-name card when the station changes
const uint32_t SCROLL_TIME = 350UL;

const uint32_t ALERT_SCROLL_TIME = 300UL;
const uint32_t ALERT_PAUSE = 2000UL;

const time_t LOOKAHEAD = 6 * 3600;

// ==================================================
// STATIONS  (add a station here + its screens below)
// ==================================================
// subway = rapid transit / Silver Line route IDs serving the station
// cr     = commuter rail route IDs serving the station
// These strings go straight into the API's filter[route] list.

static const char API_HOST[] PROGMEM = "api-v3.mbta.com";

static const char ST0_ID[]     PROGMEM = "place-sstat";
static const char ST0_NAME[]   PROGMEM = "SOUTH STATION";
static const char ST0_SUBWAY[] PROGMEM = "Red,741,742,743,749,751";
static const char ST0_CR[]     PROGMEM =
  "CR-Worcester,CR-Needham,CR-Fairmount,CR-Franklin,CR-Foxboro,CR-Providence,"
  "CR-Greenbush,CR-Kingston,CR-Middleborough,CR-NewBedford";

static const char ST1_ID[]     PROGMEM = "place-north";
static const char ST1_NAME[]   PROGMEM = "NORTH STATION";
static const char ST1_SUBWAY[] PROGMEM = "Orange,Green-B,Green-C,Green-D,Green-E";
static const char ST1_CR[]     PROGMEM =
  "CR-Fitchburg,CR-Lowell,CR-Haverhill,CR-Newburyport";

// Extra lines to include in service alerts even though neither station serves them.
static const char ALERT_EXTRA[] PROGMEM = "Blue,Mattapan";

struct StationDef {
  PGM_P id;
  PGM_P name;
  PGM_P subway;
  PGM_P cr;
};

const StationDef STATIONS[] = {
  { ST0_ID, ST0_NAME, ST0_SUBWAY, ST0_CR },
  { ST1_ID, ST1_NAME, ST1_SUBWAY, ST1_CR },
};
const uint8_t NUM_STATIONS = sizeof(STATIONS) / sizeof(STATIONS[0]);

// ============================================
// DATA TYPES
// ============================================

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// Must be defined before the first function: the Arduino build inserts
// function prototypes ahead of any type that is declared after a function.
enum Endpoint : uint8_t { EP_PRED, EP_SCHED, EP_TRACKS, EP_ALERTS };

enum ServiceType : uint8_t {
  RED_LINE, ORANGE_LINE, GREEN_LINE, BLUE_LINE,
  SILVER_LINE, COMMUTER_RAIL, OTHER_SERVICE
};

struct ScreenDef {
  uint8_t station;
  uint8_t service;
};

// Rotation order. Screens with no departures are skipped automatically.
const ScreenDef SCREENS[] = {
  {0, RED_LINE}, {0, SILVER_LINE}, {0, COMMUTER_RAIL},
  {1, ORANGE_LINE}, {1, GREEN_LINE}, {1, COMMUTER_RAIL},
};
const uint8_t NUM_SCREENS = sizeof(SCREENS) / sizeof(SCREENS[0]);

struct Arrival {
  uint32_t tripHash;
  uint32_t stopHash;
  uint32_t eventTime;
  char     name[28];    // fully formatted: "SL1 Logan", "E Heath Street", "Worcester T5"
  uint8_t  service;
  uint8_t  station;
  bool     realtime;
  bool     departure;
};

struct TrackInfo {
  uint32_t stopHash;
  char     platform[4];
};

// ============================================
// STORAGE
// ============================================

Arrival schedules[SCHED_PER_STATION * NUM_STATIONS];     // grows with the station list
Arrival predictions[PRED_PER_STATION * NUM_STATIONS];
uint8_t scheduleCount = 0;
uint8_t predictionCount = 0;

#define MAX_MERGED ((SCHED_PER_STATION + PRED_PER_STATION) * NUM_STATIONS)
const Arrival* events[MAX_MERGED];            // sorted pointers into the two arrays
uint8_t eventCount = 0;

char alerts[MAX_ALERTS][ALERT_LEN];
uint8_t alertCount = 0;

TrackInfo tracks[MAX_TRACKS];
uint8_t trackCount = 0;

uint16_t tlsBuf = 0;   // 0 = library default (large) buffers

// ============================================
// DISPLAY STATE
// ============================================

enum ScreenMode : uint8_t { SHOW_ARRIVALS, SHOW_ALERT };
ScreenMode screen = SHOW_ARRIVALS;

uint8_t screenIdx = 0;
uint8_t lastStation = 255;
uint32_t titleUntil = 0;

uint16_t scrollOffset = 0;
int alertOffset = 0;
int alertIndex = 0;

char alertBuf[ALERT_LEN + 24];
uint16_t alertLen = 0;

bool alertFinished = false;
bool alertStarted = false;
uint32_t alertPauseStarted = 0;

uint32_t lastPred = 0, lastSched = 0, lastAlert = 0, lastTrack = 0;
uint32_t lastScreen = 0, lastScroll = 0, lastCountdown = 0, lastWiFi = 0;
uint8_t predNext = 0, schedNext = 0;

// ============================================
// LCD HELPERS
// ============================================

void lcdRow(uint8_t row, const char* text) {
  char line[17];
  uint8_t i = 0;
  while (i < 16 && text[i]) { line[i] = text[i]; i++; }
  while (i < 16) line[i++] = ' ';
  line[16] = 0;
  lcd.setCursor(0, row);
  lcd.print(line);
}

void lcdRowP(uint8_t row, PGM_P text) {
  char buf[17];
  strncpy_P(buf, text, 16);
  buf[16] = 0;
  lcdRow(row, buf);
}

void statusLCD(PGM_P top, PGM_P bottom) {
  lcdRowP(0, top);
  lcdRowP(1, bottom);
}

// ============================================
// TEXT / HASH HELPERS
// ============================================

void cleanInto(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  bool prevSpace = true;
  for (size_t i = 0; src[i] && o + 1 < cap; i++) {
    uint8_t c = (uint8_t)src[i];
    if (c == 0xE2 && (uint8_t)src[i + 1] == 0x80 && src[i + 2]) {
      uint8_t t = (uint8_t)src[i + 2];
      i += 2;
      if (t == 0x98 || t == 0x99)      c = '\'';
      else if (t == 0x93 || t == 0x94) c = '-';
      else if (t == 0x9C || t == 0x9D) c = '"';
      else                             c = ' ';
    } else if (c < 32 || c > 126) {
      c = ' ';
    }
    if (c == ' ') {
      if (prevSpace) continue;
      prevSpace = true;
    } else {
      prevSpace = false;
    }
    dst[o++] = (char)c;
  }
  while (o > 0 && dst[o - 1] == ' ') o--;
  dst[o] = 0;
}

uint32_t fnv(const char* s) {
  if (!s || !*s) return 0;
  uint32_t h = 2166136261UL;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619UL; }
  return h;
}

// ============================================
// TIME
// ============================================

bool timeReady() {
  return time(nullptr) > 1700000000;
}

time_t parseISO(const char* value) {
  if (!value || strlen(value) < 20) return 0;

  int year, month, day, hour, minute, second;
  if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d",
             &year, &month, &day, &hour, &minute, &second) != 6)
    return 0;

  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 60) return 0;

  int y = year - (month <= 2);
  int era = y / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  int adjustedMonth = month + (month > 2 ? -3 : 9);
  unsigned doy = (153U * (unsigned)adjustedMonth + 2) / 5 + (unsigned)day - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t)era * 146097 + doe - 719468;
  int64_t timestamp = days * 86400LL + hour * 3600LL + minute * 60LL + second;

  const char* tz = value + 19;
  if (*tz == '.') {
    ++tz;
    while (*tz >= '0' && *tz <= '9') ++tz;
  }
  if (*tz == '+' || *tz == '-') {
    int oh = 0, om = 0;
    if (sscanf(tz + 1, "%2d:%2d", &oh, &om) != 2 || oh > 23 || om > 59) return 0;
    int sign = (*tz == '+') ? 1 : -1;
    timestamp -= sign * (oh * 3600L + om * 60L);
  } else if (*tz != 'Z' && *tz != 'z') {
    return 0;
  }
  return (time_t)timestamp;
}

void formatClock(time_t ts, char* out, size_t cap) {
  struct tm lt;
  localtime_r(&ts, &lt);
  char b[8];
  strftime(b, sizeof(b), "%I:%M", &lt);
  strlcpy(out, b[0] == '0' ? b + 1 : b, cap);
}

// ============================================
// ROUTES
// ============================================

// prefix (>= 4 bytes) receives a short tag shown before the destination:
// "SL1".."SL5" for Silver Line, "B".."E" for Green Line branches, else "".
uint8_t classifyRoute(const char* r, char* prefix) {
  prefix[0] = 0;
  if (!strcmp_P(r, PSTR("Red")))     return RED_LINE;
  if (!strcmp_P(r, PSTR("Orange")))  return ORANGE_LINE;
  if (!strcmp_P(r, PSTR("Blue")))    return BLUE_LINE;
  if (!strncmp_P(r, PSTR("Green-"), 6)) {
    prefix[0] = r[6];
    prefix[1] = 0;
    return GREEN_LINE;
  }
  if (!strncmp_P(r, PSTR("CR-"), 3)) return COMMUTER_RAIL;
  switch (atoi(r)) {
    case 741: strcpy_P(prefix, PSTR("SL1")); return SILVER_LINE;
    case 742: strcpy_P(prefix, PSTR("SL2")); return SILVER_LINE;
    case 743: strcpy_P(prefix, PSTR("SL3")); return SILVER_LINE;
    case 751: strcpy_P(prefix, PSTR("SL4")); return SILVER_LINE;
    case 749: strcpy_P(prefix, PSTR("SL5")); return SILVER_LINE;
  }
  return OTHER_SERVICE;
}

PGM_P serviceLabel(uint8_t s) {
  switch (s) {
    case RED_LINE:    return PSTR("RED LINE");
    case ORANGE_LINE: return PSTR("ORANGE LINE");
    case GREEN_LINE:  return PSTR("GREEN LINE");
    case BLUE_LINE:   return PSTR("BLUE LINE");
    case SILVER_LINE: return PSTR("SILVER LINE");
    default:          return PSTR("COMMUTER RAIL");
  }
}

// ============================================
// WIFI
// ============================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  statusLCD(PSTR("MBTA ARRIVALS"), PSTR("CONNECTING WIFI"));

  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000)
    delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("IP: "));
    Serial.println(WiFi.localIP());
  }
}

// ============================================
// TLS: shrink BearSSL buffers when the server allows it
// ============================================

void probeTLS() {
  static const uint16_t sizes[] = {512, 1024, 2048, 4096};
  char host[sizeof(API_HOST)];
  strcpy_P(host, API_HOST);

  for (uint8_t i = 0; i < 4; i++) {
    BearSSL::WiFiClientSecure c;
    c.setInsecure();
    if (c.probeMaxFragmentLength(host, 443, sizes[i])) {
      tlsBuf = sizes[i];
      break;
    }
  }
  Serial.printf_P(PSTR("TLS buffer: %u (0 = default), heap: %u\n"),
                  tlsBuf, ESP.getFreeHeap());
}

// ============================================
// URL BUILDING (pieces come from flash)
// ============================================

void buildURL(String& url, Endpoint ep, uint8_t st) {
  url.reserve(420);
  url = F("https://api-v3.mbta.com/");

  switch (ep) {
    case EP_PRED:
      url += F("predictions?include=trip&page%5Blimit%5D=");
      url += String(PRED_PER_STATION);
      url += F("&filter%5Bstop%5D=");
      url += FPSTR(STATIONS[st].id);
      url += F("&filter%5Broute%5D=");
      url += FPSTR(STATIONS[st].subway);
      url += ',';
      url += FPSTR(STATIONS[st].cr);
      break;

    case EP_SCHED: {
      // Commuter rail only: subway/Silver Line always have real-time predictions.
      time_t t = time(nullptr);
      struct tm lt;
      localtime_r(&t, &lt);
      int mins = lt.tm_hour * 60 + lt.tm_min;
      if (lt.tm_hour < 3) mins += 24 * 60;   // service day runs past midnight
      int lo = mins - 2;
      int hi = mins + SCHED_WINDOW_MIN;
      char b[40];
      snprintf_P(b, sizeof(b),
                 PSTR("%02d:%02d&filter%%5Bmax_time%%5D=%02d:%02d"),
                 lo / 60, lo % 60, hi / 60, hi % 60);

      url += F("schedules?include=trip&page%5Blimit%5D=");
      url += String(SCHED_PER_STATION);
      url += F("&filter%5Bstop%5D=");
      url += FPSTR(STATIONS[st].id);
      url += F("&filter%5Broute%5D=");
      url += FPSTR(STATIONS[st].cr);
      url += F("&filter%5Bmin_time%5D=");
      url += b;
      break;
    }

    case EP_TRACKS:
      url += F("stops?page%5Blimit%5D=100&filter%5Blocation_type%5D=0&filter%5Bparent_station%5D=");
      for (uint8_t i = 0; i < NUM_STATIONS; i++) {
        if (i) url += ',';
        url += FPSTR(STATIONS[i].id);
      }
      break;

    case EP_ALERTS:
      url += F("alerts?page%5Blimit%5D=8&sort=-severity&filter%5Broute%5D=");
      for (uint8_t i = 0; i < NUM_STATIONS; i++) {
        url += FPSTR(STATIONS[i].subway);
        url += ',';
        url += FPSTR(STATIONS[i].cr);
        url += ',';
      }
      url += FPSTR(ALERT_EXTRA);
      break;
  }
}

// ============================================
// HTTPS / JSON
// ============================================

bool fetchJSON(Endpoint ep, uint8_t st, JsonDocument& doc, JsonDocument& filter) {
  if (WiFi.status() != WL_CONNECTED || !timeReady())
    return false;

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  if (tlsBuf) client.setBufferSizes(tlsBuf, tlsBuf);

  HTTPClient http;
  http.useHTTP10(true);   // no chunked encoding -> safe to stream into the parser

  bool ok;
  {
    String url;           // temporary; freed before the request runs
    buildURL(url, ep, st);
    ok = http.begin(client, url);
  }
  if (!ok) return false;

  http.setTimeout(12000);
  http.addHeader(F("Accept"), F("application/vnd.api+json"));
  if (MBTA_KEY[0]) http.addHeader(F("x-api-key"), MBTA_KEY);

  int code = http.GET();

  Serial.printf_P(PSTR("HTTP %d (ep %u st %u), heap: %u, block: %u\n"),
                  code, ep, st, ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());

  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  DeserializationError error =
    deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));

  http.end();

  if (error) {
    Serial.print(F("JSON error: "));
    Serial.println(error.c_str());
    return false;
  }
  return true;
}

// ============================================
// JSON HELPERS
// ============================================

const char* relId(JsonVariantConst item, const char* rel) {
  return item["relationships"][rel]["data"]["id"] | "";
}

const char* findHeadsign(JsonArrayConst included, const char* tripID) {
  for (JsonVariantConst item : included) {
    const char* type = item["type"];
    const char* id = item["id"];
    if (!type || !id) continue;
    if (!strcmp_P(type, PSTR("trip")) && !strcmp(id, tripID))
      return item["attributes"]["headsign"] | "";
  }
  return "";
}

// ============================================
// TRACKS
// ============================================

const char* lookupTrack(uint32_t stopHash) {
  for (uint8_t i = 0; i < trackCount; i++)
    if (tracks[i].stopHash == stopHash) return tracks[i].platform;
  return "";
}

void refreshTracks() {
  JsonDocument filter;
  filter["data"][0]["id"] = true;
  filter["data"][0]["attributes"]["platform_code"] = true;

  JsonDocument doc;
  if (!fetchJSON(EP_TRACKS, 0, doc, filter)) return;

  JsonArrayConst data = doc["data"].as<JsonArrayConst>();
  if (data.isNull()) return;

  trackCount = 0;
  for (JsonVariantConst item : data) {
    if (trackCount >= MAX_TRACKS) break;
    const char* id = item["id"];
    const char* platform = item["attributes"]["platform_code"];
    if (!id || !platform || !platform[0]) continue;

    tracks[trackCount].stopHash = fnv(id);
    strlcpy(tracks[trackCount].platform, platform, sizeof(tracks[0].platform));
    trackCount++;
  }
  Serial.printf_P(PSTR("Tracks: %u\n"), trackCount);
}

// ============================================
// EVENT PARSING
// ============================================

void buildEventFilter(JsonDocument& filter) {
  filter["data"][0]["attributes"]["arrival_time"] = true;
  filter["data"][0]["attributes"]["departure_time"] = true;
  filter["data"][0]["relationships"]["trip"]["data"]["id"] = true;
  filter["data"][0]["relationships"]["route"]["data"]["id"] = true;
  filter["data"][0]["relationships"]["stop"]["data"]["id"] = true;
  filter["included"][0]["type"] = true;
  filter["included"][0]["id"] = true;
  filter["included"][0]["attributes"]["headsign"] = true;
}

// Replaces this station's entries in arr[] with the freshly parsed ones.
void parseEvents(JsonDocument& doc, Arrival* arr, uint8_t& count, uint8_t cap,
                 uint8_t station, bool realtime) {
  JsonArrayConst data = doc["data"].as<JsonArrayConst>();
  JsonArrayConst included = doc["included"].as<JsonArrayConst>();
  if (data.isNull()) return;

  // Drop the old entries for this station (keeps the other stations' data).
  uint8_t keep = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (arr[i].station != station) {
      if (keep != i) arr[keep] = arr[i];
      keep++;
    }
  }
  count = keep;

  time_t now = time(nullptr);

  for (JsonVariantConst item : data) {
    if (count >= cap) break;

    JsonVariantConst attr = item["attributes"];
    const char* arrival = attr["arrival_time"];
    const char* departure = attr["departure_time"];
    bool isDep = departure && departure[0];

    time_t when = parseISO(isDep ? departure : arrival);
    if (!when || when < now - 45 || when > now + LOOKAHEAD) continue;

    char prefix[4];
    uint8_t svc = classifyRoute(relId(item, "route"), prefix);
    if (svc == OTHER_SERVICE) continue;

    const char* tripID = relId(item, "trip");
    const char* stopID = relId(item, "stop");

    Arrival& a = arr[count];
    a.tripHash  = fnv(tripID);
    a.stopHash  = fnv(stopID);
    a.eventTime = (uint32_t)when;
    a.service   = svc;
    a.station   = station;
    a.realtime  = realtime;
    a.departure = isDep;

    const char* head = findHeadsign(included, tripID);
    char clean[24];
    cleanInto(clean, sizeof(clean), head[0] ? head : "UNKNOWN");
    if (!clean[0]) strcpy_P(clean, PSTR("UNKNOWN"));

    // Build the final display name once, here, instead of on every redraw.
    if (prefix[0]) {
      snprintf_P(a.name, sizeof(a.name), PSTR("%s %s"), prefix, clean);
    } else if (svc == COMMUTER_RAIL) {
      const char* trk = lookupTrack(a.stopHash);
      if (trk[0]) snprintf_P(a.name, sizeof(a.name), PSTR("%s T%s"), clean, trk);
      else        strlcpy(a.name, clean, sizeof(a.name));
    } else {
      strlcpy(a.name, clean, sizeof(a.name));
    }
    count++;
  }
}

// ============================================
// MERGE (events[] only holds pointers)
// ============================================

void addMerged(const Arrival* a) {
  for (uint8_t i = 0; i < eventCount; i++) {
    const Arrival* e = events[i];
    if (e->tripHash == a->tripHash && e->stopHash == a->stopHash &&
        e->departure == a->departure) {
      if (a->realtime || !e->realtime) events[i] = a;
      return;
    }
  }
  if (eventCount < MAX_MERGED) events[eventCount++] = a;
}

void rebuildEvents() {
  eventCount = 0;
  uint32_t cutoff = (uint32_t)(time(nullptr) - 45);

  for (uint8_t i = 0; i < scheduleCount; i++)
    if (schedules[i].eventTime >= cutoff) addMerged(&schedules[i]);

  for (uint8_t i = 0; i < predictionCount; i++)
    if (predictions[i].eventTime >= cutoff) addMerged(&predictions[i]);

  for (uint8_t i = 1; i < eventCount; i++) {
    const Arrival* key = events[i];
    int j = i - 1;
    while (j >= 0 && events[j]->eventTime > key->eventTime) {
      events[j + 1] = events[j];
      j--;
    }
    events[j + 1] = key;
  }
}

// ============================================
// PREDICTIONS / SCHEDULES
// ============================================

void refreshPredictions(uint8_t st) {
  JsonDocument filter;
  buildEventFilter(filter);
  JsonDocument doc;

  if (!fetchJSON(EP_PRED, st, doc, filter)) return;

  parseEvents(doc, predictions, predictionCount, PRED_PER_STATION * NUM_STATIONS, st, true);
  rebuildEvents();
  Serial.printf_P(PSTR("Predictions: %u, merged: %u\n"), predictionCount, eventCount);
}

void refreshSchedules(uint8_t st) {
  JsonDocument filter;
  buildEventFilter(filter);
  JsonDocument doc;

  if (!fetchJSON(EP_SCHED, st, doc, filter)) return;

  parseEvents(doc, schedules, scheduleCount, SCHED_PER_STATION * NUM_STATIONS, st, false);
  rebuildEvents();
  Serial.printf_P(PSTR("Schedules: %u, merged: %u\n"), scheduleCount, eventCount);
}

// ============================================
// ALERTS
// ============================================

bool alertActive(JsonVariantConst attributes) {
  JsonArrayConst periods = attributes["active_period"].as<JsonArrayConst>();
  if (periods.isNull() || periods.size() == 0) return true;

  time_t now = time(nullptr);
  for (JsonVariantConst period : periods) {
    time_t start = parseISO(period["start"].as<const char*>());
    time_t end   = parseISO(period["end"].as<const char*>());
    if ((!start || now >= start) && (!end || now <= end)) return true;
  }
  return false;
}

void refreshAlerts() {
  JsonDocument filter;
  filter["data"][0]["attributes"]["header"] = true;
  filter["data"][0]["attributes"]["active_period"][0]["start"] = true;
  filter["data"][0]["attributes"]["active_period"][0]["end"] = true;

  JsonDocument doc;
  if (!fetchJSON(EP_ALERTS, 0, doc, filter)) return;

  JsonArrayConst data = doc["data"].as<JsonArrayConst>();
  if (data.isNull()) return;

  uint8_t n = 0;
  for (JsonVariantConst item : data) {
    if (n >= MAX_ALERTS) break;
    JsonVariantConst attr = item["attributes"];
    if (!alertActive(attr)) continue;

    const char* header = attr["header"];
    if (!header || !header[0]) continue;

    cleanInto(alerts[n], ALERT_LEN, header);
    if (alerts[n][0]) n++;
  }

  alertCount = n;
  if (alertIndex >= alertCount) alertIndex = 0;
  Serial.printf_P(PSTR("Alerts: %u\n"), alertCount);
}

// ============================================
// ARRIVAL DISPLAY
// ============================================

void scrollName(const char* name, uint16_t offset, char* out10) {
  size_t len = strlen(name);
  if (len <= 10) {
    memset(out10, ' ', 10);
    memcpy(out10, name, len);
    return;
  }
  size_t cycle = len + 3;
  for (uint8_t i = 0; i < 10; i++) {
    size_t k = (offset + i) % cycle;
    out10[i] = (k < len) ? name[k] : ' ';
  }
}

void countdown(const Arrival& a, char* out, size_t cap) {
  long remaining = (long)((time_t)a.eventTime - time(nullptr));

  if (remaining <= 0) {
    snprintf_P(out, cap, PSTR("BRD"));
    return;
  }

  long minutes = (remaining + 59) / 60;
  if (minutes > 99) formatClock((time_t)a.eventTime, out, cap);
  else snprintf_P(out, cap, PSTR("%ldmin"), minutes);

  if (!a.realtime) {
    size_t l = strlen(out);
    if (l + 1 < cap) { out[l] = '*'; out[l + 1] = 0; }
  }
}

bool screenHasEvents(uint8_t s) {
  time_t cutoff = time(nullptr) - 45;
  for (uint8_t i = 0; i < eventCount; i++) {
    const Arrival* e = events[i];
    if (e->station == SCREENS[s].station && e->service == SCREENS[s].service &&
        (time_t)e->eventTime >= cutoff)
      return true;
  }
  return false;
}

void renderArrivals() {
  if ((int32_t)(titleUntil - millis()) > 0) return;   // station title card showing

  const ScreenDef& sd = SCREENS[screenIdx];
  uint8_t found = 0;
  time_t now = time(nullptr);

  for (uint8_t i = 0; i < eventCount && found < 2; i++) {
    const Arrival* e = events[i];
    if (e->station != sd.station || e->service != sd.service) continue;
    if ((time_t)e->eventTime < now - 45) continue;

    char line[17];
    char cd[8];
    scrollName(e->name, scrollOffset, line);
    countdown(*e, cd, sizeof(cd));

    memset(line + 10, ' ', 6);
    size_t cl = strlen(cd);
    memcpy(line + 10, cd, cl > 6 ? 6 : cl);
    line[16] = 0;

    lcdRow(found, line);
    found++;
  }

  if (found == 0) {
    lcdRowP(0, serviceLabel(sd.service));
    lcdRowP(1, PSTR("NO DEPARTURES"));
  } else if (found == 1) {
    lcdRow(1, "");
  }
}

// ============================================
// ALERT MARQUEE
// ============================================

void startAlert() {
  if (alertCount == 0) return;

  screen = SHOW_ALERT;
  alertOffset = 0;
  alertFinished = false;
  alertStarted = false;
  alertPauseStarted = millis();
  lastScreen = millis();

  if (alertIndex >= alertCount) alertIndex = 0;

  snprintf_P(alertBuf, sizeof(alertBuf), PSTR("MBTA SERVICE ALERT - %s"),
             alerts[alertIndex]);
  for (char* p = alertBuf; *p; p++) *p = toupper((unsigned char)*p);
  alertLen = strlen(alertBuf);

  lcdRow(0, "");
  lcdRowP(1, PSTR("SERVICE ALERT"));
}

void renderAlert() {
  if (alertFinished) return;

  uint32_t now = millis();

  if (!alertStarted) {
    if (now - alertPauseStarted < ALERT_PAUSE) return;
    alertStarted = true;
  }

  if (alertOffset <= (int)alertLen + 16) {
    char line[17];
    for (uint8_t i = 0; i < 16; i++) {
      int p = alertOffset + i - 16;
      line[i] = (p >= 0 && p < (int)alertLen) ? alertBuf[p] : ' ';
    }
    line[16] = 0;
    lcdRow(0, line);
    lcdRowP(1, PSTR("SERVICE ALERT"));
    alertOffset++;
  } else {
    alertFinished = true;
    alertPauseStarted = now;
  }
}

// ============================================
// SCREEN ROTATION
// ============================================

void showScreen(uint8_t s) {
  bool newStation = (screen == SHOW_ALERT) || (lastStation != SCREENS[s].station);

  screen = SHOW_ARRIVALS;
  screenIdx = s;
  scrollOffset = 0;
  lastScreen = millis();
  lastStation = SCREENS[s].station;

  if (newStation) {
    titleUntil = millis() + TITLE_TIME;
    lcdRowP(0, STATIONS[SCREENS[s].station].name);
    lcdRowP(1, serviceLabel(SCREENS[s].service));
  }
}

void advanceScreen() {
  if (screen == SHOW_ALERT) {
    alertIndex++;
    if (alertIndex < alertCount) { startAlert(); return; }

    alertIndex = 0;
    for (uint8_t s = 0; s < NUM_SCREENS; s++)
      if (screenHasEvents(s)) { showScreen(s); return; }
    showScreen(0);
    return;
  }

  // Next screen (after this one) that has something to show.
  for (uint8_t s = screenIdx + 1; s < NUM_SCREENS; s++)
    if (screenHasEvents(s)) { showScreen(s); return; }

  // End of the arrival cycle: alerts, if any.
  if (alertCount > 0) {
    alertIndex = 0;
    startAlert();
    return;
  }

  // No alerts: wrap around.
  for (uint8_t s = 0; s <= screenIdx; s++)
    if (screenHasEvents(s)) { showScreen(s); return; }

  // Nothing live anywhere: keep cycling so the display still shows something.
  showScreen((screenIdx + 1) % NUM_SCREENS);
}

// ============================================
// SETUP
// ============================================

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();

  statusLCD(PSTR("MBTA ARRIVALS"), PSTR("SOUTH/NORTH STA"));
  delay(1500);

  connectWiFi();

  configTime("EST5EDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org", "time.google.com");

  statusLCD(PSTR("MBTA ARRIVALS"), PSTR("SYNCING CLOCK"));

  uint32_t started = millis();
  while (!timeReady() && millis() - started < 15000)
    delay(200);

  probeTLS();

  statusLCD(PSTR("MBTA ARRIVALS"), PSTR("LOADING DATA"));

  refreshTracks();
  for (uint8_t st = 0; st < NUM_STATIONS; st++) {
    refreshSchedules(st);
    refreshPredictions(st);
  }
  refreshAlerts();

  uint32_t t = millis();
  lastTrack = lastSched = lastPred = lastAlert = lastScreen = t;

  uint8_t first = 0;
  for (uint8_t s = 0; s < NUM_SCREENS; s++)
    if (screenHasEvents(s)) { first = s; break; }
  showScreen(first);
}

// ============================================
// MAIN LOOP
// ============================================

void loop() {
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED && now - lastWiFi >= 10000) {
    lastWiFi = now;
    WiFi.reconnect();
  }

  // One request per tick, rotating through stations, so the display never
  // freezes for several back-to-back HTTPS requests.
  if (now - lastTrack >= TRACK_REFRESH) { lastTrack = now; refreshTracks(); }

  if (now - lastPred >= PRED_REFRESH / NUM_STATIONS) {
    lastPred = now;
    refreshPredictions(predNext);
    predNext = (predNext + 1) % NUM_STATIONS;
  }

  if (now - lastSched >= SCHED_REFRESH / NUM_STATIONS) {
    lastSched = now;
    refreshSchedules(schedNext);
    schedNext = (schedNext + 1) % NUM_STATIONS;
  }

  if (now - lastAlert >= ALERT_REFRESH) { lastAlert = now; refreshAlerts(); }

  if (screen == SHOW_ALERT) {
    if (!alertFinished) {
      if (now - lastScroll >= ALERT_SCROLL_TIME) {
        lastScroll = now;
        renderAlert();
      }
    } else if (now - alertPauseStarted >= ALERT_PAUSE) {
      advanceScreen();
      if (screen != SHOW_ALERT) renderArrivals();
    }
  } else {
    if (now - lastScroll >= SCROLL_TIME) {
      lastScroll = now;
      scrollOffset++;
      renderArrivals();
    }
    if (now - lastCountdown >= 1000) {
      lastCountdown = now;
      renderArrivals();
    }
  }

  if (screen != SHOW_ALERT && now - lastScreen >= SCREEN_TIME) {
    advanceScreen();
    if (screen != SHOW_ALERT) renderArrivals();
  }

  delay(10);
}
