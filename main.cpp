#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <time.h>
#include <ESPmDNS.h>
#include <esp_sntp.h>
#include <esp_sleep.h>
#include "config.h"
#include "page.h"

GxEPD2_3C<GxEPD2_154_Z90c, GxEPD2_154_Z90c::HEIGHT> display(
    GxEPD2_154_Z90c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
WebServer server(80);
DNSServer dns;
Preferences prefs;

struct Cfg { double lat, lon; int tzMin; int method; int asr; };
struct Day { int s[5], e[5]; int hm, hd; };

#define FB_RISE 15
#define FB_ZAWAL 6
#define FB_SET 15

Cfg cfg = {23.8103, 90.4125, 360, 1, 1};
RTC_DATA_ATTR Day day;
RTC_DATA_ATTR int dayKey = -1;
RTC_DATA_ATTR int lastKey = -1;
String wSsid, wPass;
bool apMode = false;
bool configured = false;
bool cold = true;
bool dispInit = false;
uint32_t setupEnd = 0;

const char *NAMES[5] = {"Fajr", "Dhuhr", "Asr", "Maghrib", "Isha"};
const char *nm(int i, const tm &t) { return (i == 1 && t.tm_wday == 5) ? "Jumu'ah" : NAMES[i]; }
const char *DOW[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

String fmt(int m, bool suffix) {
  m = ((m % 1440) + 1440) % 1440;
  int h = m / 60;
  m %= 60;
  bool pm = h >= 12;
  h %= 12;
  if (h == 0) h = 12;
  char b[12];
  if (suffix) snprintf(b, sizeof b, "%d:%02d %s", h, m, pm ? "PM" : "AM");
  else snprintf(b, sizeof b, "%d:%02d", h, m);
  return String(b);
}

bool nowLocal(tm &t) {
  time_t s = time(nullptr);
  if (s < 1700000000) return false;
  s += (time_t)cfg.tzMin * 60;
  gmtime_r(&s, &t);
  return true;
}

int mins(const char *p) {
  int h, m;
  if (!p || sscanf(p, "%d:%d", &h, &m) != 2) return -1;
  return h * 60 + m;
}

bool fetchDay(const tm &t) {
  char url[220];
  snprintf(url, sizeof url,
           "https://api.aladhan.com/v1/timings/%02d-%02d-%04d?latitude=%.5f&longitude=%.5f&method=%d&school=%d",
           t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, cfg.lat, cfg.lon, cfg.method, cfg.asr);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("aladhan http %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument filter;
  filter["data"]["timings"] = true;
  filter["data"]["date"]["hijri"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;

  JsonObject T = doc["data"]["timings"];
  int fajr = mins(T["Fajr"]), sunrise = mins(T["Sunrise"]), dhuhr = mins(T["Dhuhr"]);
  int asr = mins(T["Asr"]), sunset = mins(T["Sunset"]), maghrib = mins(T["Maghrib"]);
  int isha = mins(T["Isha"]), midnight = mins(T["Midnight"]);
  if (fajr < 0 || sunrise < 0 || dhuhr < 0 || asr < 0 || sunset < 0 || maghrib < 0 ||
      isha < 0 || midnight < 0)
    return false;
  if (midnight < isha) midnight += 1440;

  day.s[0] = fajr;    day.e[0] = sunrise;
  day.s[1] = dhuhr;   day.e[1] = asr;
  day.s[2] = asr;     day.e[2] = sunset;
  day.s[3] = maghrib; day.e[3] = isha;
  day.s[4] = isha;    day.e[4] = midnight;
  const char *hdS = doc["data"]["date"]["hijri"]["day"];
  day.hd = hdS ? atoi(hdS) : 0;
  day.hm = doc["data"]["date"]["hijri"]["month"]["number"] | 0;
  dayKey = t.tm_year * 366 + t.tm_yday;
  return true;
}

int currentIdx(const tm &t) {
  int m = t.tm_hour * 60 + t.tm_min;
  for (int i = 0; i < 5; i++)
    if (m >= day.s[i] && m < day.e[i]) return i;
  if (m + 1440 >= day.s[4] && m + 1440 < day.e[4]) return 4;
  return -1;
}

void fbWin(int i, int &a, int &b) {
  if (i == 0) { a = day.e[0]; b = day.e[0] + FB_RISE; }
  else if (i == 1) { a = day.s[1] - FB_ZAWAL; b = day.s[1]; }
  else { a = day.e[2] - FB_SET; b = day.e[2]; }
}

int fbIdx(int m) {
  for (int i = 0; i < 3; i++) {
    int a, b;
    fbWin(i, a, b);
    if (m >= a && m < b) return i;
  }
  return -1;
}

void initDisplay() {
  if (dispInit) return;
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
  display.init(115200, cold, 10, false);
  display.setRotation(0);
  dispInit = true;
}

void drawMsg(const char *a, const String &b) {
  initDisplay();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_RED);
    display.setCursor(4, 40);
    display.print(a);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(4, 70);
    display.print(b);
  } while (display.nextPage());
}

void drawTimes(const tm &t, int cur, int fb) {
  int m = t.tm_hour * 60 + t.tm_min;
  initDisplay();
  int nxt = 0, nxtT = day.s[0] + 1440;
  for (int i = 0; i < 5; i++)
    if (day.s[i] > m) { nxt = i; nxtT = day.s[i]; break; }
  bool ram = day.hm == 9;
  int rh = ram ? 19 : 21, ty = ram ? 14 : 16, fy = 72 + 5 * rh;
  int fa = 0, fe = 0;
  if (fb >= 0) fbWin(fb, fa, fe);
  int night = day.s[0] + 1440 - day.s[3];
  int third = day.s[0] + 1440 - night / 3;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, 200, 20, GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 15);
    display.printf("%s %d %s", DOW[t.tm_wday], t.tm_mday, MON[t.tm_mon]);
    if (ram) {
      display.setCursor(104, 15);
      display.printf("Ramadan %d", day.hd);
    }

    display.fillRect(0, 20, 200, 50, fb >= 0 ? GxEPD_BLACK : GxEPD_RED);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(4, 36);
    if (fb >= 0) {
      display.print("Nafl forbidden until");
      display.setFont(&FreeSansBold18pt7b);
      display.setCursor(4, 63);
      display.print(fmt(fe, true));
    } else if (cur >= 0) {
      display.printf("%s ends at", nm(cur, t));
      display.setFont(&FreeSansBold18pt7b);
      display.setCursor(4, 63);
      display.print(fmt(day.e[cur], true));
    } else {
      display.printf("Next: %s at", nm(nxt, t));
      display.setFont(&FreeSansBold18pt7b);
      display.setCursor(4, 63);
      display.print(fmt(nxtT, true));
    }

    for (int i = 0; i < 5; i++) {
      int top = 72 + i * rh;
      bool on = (i == cur);
      display.drawFastHLine(0, top, 200, GxEPD_BLACK);
      display.setTextColor(on ? GxEPD_RED : GxEPD_BLACK);
      display.setFont(on ? &FreeSansBold9pt7b : &FreeSans9pt7b);
      display.setCursor(2, top + ty);
      display.print(nm(i, t));
      display.setFont(&FreeSans9pt7b);
      display.setCursor(80, top + ty);
      display.print(fmt(day.s[i], false));
      display.setCursor(128, top + ty);
      display.print("-");
      display.setCursor(136, top + ty);
      display.print(fmt(day.e[i], false));
      if (i == 1 && t.tm_wday == 5) display.drawRect(0, top, 200, rh, GxEPD_RED);
    }

    display.drawFastHLine(0, fy, 200, GxEPD_BLACK);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(2, fy + (ram ? 14 : 17));
    display.printf("Last 3rd  %s", fmt(third, true).c_str());
    if (ram) {
      display.setCursor(2, fy + 30);
      display.printf("Suhoor %s  Iftar %s", fmt(day.s[0], false).c_str(), fmt(day.s[3], false).c_str());
    }
  } while (display.nextPage());
}

void sendState() {
  if (apMode) {
    server.send(200, "application/json",
                "{\"ap\":true,\"lat\":" + String(cfg.lat, 5) + ",\"lon\":" + String(cfg.lon, 5) +
                    ",\"tz\":" + String(cfg.tzMin) + ",\"m\":" + String(cfg.method) +
                    ",\"a\":" + String(cfg.asr) + ",\"set\":" + (configured ? "true" : "false") +
                    ",\"cur\":-1,\"date\":\"\",\"now\":\"\",\"ssid\":\"" + wSsid + "\",\"t\":[]}");
    return;
  }
  tm t;
  if (!nowLocal(t)) {
    server.send(503, "text/plain", "no time yet");
    return;
  }
  if (dayKey != t.tm_year * 366 + t.tm_yday && !fetchDay(t)) {
    server.send(502, "text/plain", "fetch failed");
    return;
  }
  String j = "{\"lat\":" + String(cfg.lat, 5) + ",\"lon\":" + String(cfg.lon, 5) +
             ",\"tz\":" + String(cfg.tzMin) + ",\"m\":" + String(cfg.method) +
             ",\"a\":" + String(cfg.asr) + ",\"set\":" + (configured ? "true" : "false") +
             ",\"ssid\":\"" + wSsid + "\",\"cur\":" + String(currentIdx(t)) + ",\"date\":\"" + DOW[t.tm_wday] + " " +
             String(t.tm_mday) + " " + MON[t.tm_mon] + "\",\"now\":\"" +
             fmt(t.tm_hour * 60 + t.tm_min, true) + "\",\"t\":[";
  for (int i = 0; i < 5; i++) {
    j += "[\"" + fmt(day.s[i], true) + "\",\"" + fmt(day.e[i], true) + "\"]";
    if (i < 4) j += ",";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleSave() {
  double la = server.arg("lat").toDouble();
  double lo = server.arg("lon").toDouble();
  int tz = server.arg("tz").toInt();
  int m = server.arg("m").toInt();
  int a = server.arg("a").toInt();
  if (la < -90 || la > 90 || lo < -180 || lo > 180 || tz < -720 || tz > 840 ||
      m < 1 || m > 5 || a < 0 || a > 1) {
    server.send(400, "text/plain", "bad values");
    return;
  }
  Cfg old = cfg;
  cfg = {la, lo, tz, m, a};
  tm t;
  if (!apMode && (!nowLocal(t) || !fetchDay(t))) {
    cfg = old;
    server.send(502, "text/plain", "fetch failed");
    return;
  }
  prefs.begin("salah", false);
  prefs.putDouble("lat", la);
  prefs.putDouble("lon", lo);
  prefs.putInt("tz", tz);
  prefs.putInt("m", m);
  prefs.putInt("a", a);
  prefs.putBool("set", true);
  prefs.end();
  configured = true;
  lastKey = -1;
  setupEnd = millis() + 20000;
  sendState();
}

void handleWifi() {
  String s = server.arg("ssid");
  if (s.length() == 0) {
    server.send(400, "text/plain", "ssid required");
    return;
  }
  prefs.begin("salah", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", server.arg("pass"));
  prefs.end();
  server.send(200, "text/plain", "ok");
  delay(500);
  ESP.restart();
}

void routes() {
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/api/state", sendState);
  server.on("/api/save", handleSave);
  server.on("/api/wifi", handleWifi);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void runAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dns.start(53, "*", WiFi.softAPIP());
  routes();
  initDisplay();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_RED);
    display.setCursor(4, 30);
    display.print("Join Wi-Fi:");
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(4, 58);
    display.print(AP_SSID);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(4, 90);
    display.print("Password:");
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(4, 112);
    display.print(AP_PASS);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(4, 150);
    display.print("Then open:");
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(4, 172);
    display.print("192.168.4.1");
  } while (display.nextPage());
  uint32_t end = millis() + 600000;
  while (millis() < end) {
    dns.processNextRequest();
    server.handleClient();
    delay(2);
  }
  server.stop();
  dns.stop();
  apMode = false;
}

bool wifiUp() {
  if (wSsid.length() == 0) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wSsid.c_str(), wPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t t0 = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - t0 < 10000) delay(200);
}

void goSleep(uint32_t sec) {
  if (dispInit) display.hibernate();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
  esp_deep_sleep_start();
}

bool show(const tm &t, bool online) {
  int dk = t.tm_year * 366 + t.tm_yday;
  if (dayKey != dk && !(online && fetchDay(t))) return false;
  int cur = currentIdx(t);
  int fb = fbIdx(t.tm_hour * 60 + t.tm_min);
  int key = dk * 100 + (cur + 1) * 10 + fb + 1;
  if (key != lastKey) {
    drawTimes(t, cur, fb);
    lastKey = key;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  cold = esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER;
  if (cold) {
    dayKey = -1;
    lastKey = -1;
  }

  prefs.begin("salah", true);
  configured = prefs.getBool("set", false);
  if (configured) {
    cfg.lat = prefs.getDouble("lat", cfg.lat);
    cfg.lon = prefs.getDouble("lon", cfg.lon);
    cfg.tzMin = prefs.getInt("tz", cfg.tzMin);
    cfg.method = prefs.getInt("m", cfg.method);
    cfg.asr = prefs.getInt("a", cfg.asr);
  }
  wSsid = prefs.getString("ssid", "");
  wPass = prefs.getString("pass", "");
  prefs.end();

  tm t;
  bool online = false;
  bool have = !cold && nowLocal(t);
  if (!have || dayKey != t.tm_year * 366 + t.tm_yday) {
    online = wifiUp();
    if (online) syncTime();
  }
  if (!online && cold) runAP();
  if (!nowLocal(t)) goSleep(300);

  if (configured && cold) show(t, online);

  if (online && (cold || !configured)) {
    MDNS.begin("salah");
    routes();
    Serial.println(WiFi.localIP());
    if (!configured) drawMsg("Set location at:", WiFi.localIP().toString());
    setupEnd = millis() + 300000;
    while (millis() < setupEnd || !configured) {
      server.handleClient();
      delay(2);
    }
    server.stop();
    nowLocal(t);
  }

  if (!show(t, online)) goSleep(600);

  int now = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  int next = 86400;
  for (int i = 0; i < 5; i++) {
    int v[2] = {day.s[i] * 60, day.e[i] * 60};
    for (int k = 0; k < 2; k++)
      if (v[k] > now && v[k] < next) next = v[k];
  }
  for (int i = 0; i < 3; i++) {
    int a, b;
    fbWin(i, a, b);
    int v[2] = {a * 60, b * 60};
    for (int k = 0; k < 2; k++)
      if (v[k] > now && v[k] < next) next = v[k];
  }
  int sl = next - now + 2;
  if (sl < 10) sl = 10;
  if (sl > 7200) sl = 7200;
  goSleep(sl);
}

void loop() {}
