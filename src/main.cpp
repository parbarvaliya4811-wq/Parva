// CYD Mini OS - ESP32-2432S028 (Cheap Yellow Display)
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>

#define SW 320
#define SH 240
#define C_BG    0x0861
#define C_PANEL 0x18E3
#define C_ACC   0x04FF
#define C_OK    0x07E0
#define C_BAD   0xF800
#define C_WARN  0xFD20
#define C_TXT   0xFFFF
#define C_DIM   0x8410
#define WX_LAT "21.17"
#define WX_LON "72.83"
#define WX_CITY "Surat"

TFT_eSPI tft = TFT_eSPI();
SPIClass tsSPI(VSPI);
XPT2046_Touchscreen ts(33, 36);
Preferences prefs;

struct Settings { uint8_t bright = 200; bool sound = true, autodim = false, invert = false; String ssid, pass; } S;

enum { A_HOME, A_CLOCK, A_WIFI, A_GAMES, A_DRAW, A_FOCUS, A_TEST, A_SET, A_INFO };
int app = A_HOME;
bool needInit = true;
void goApp(int a) { app = a; needInit = true; }

// ---------------- touch ----------------
int tx = 0, ty = 0, rawx = 0, rawy = 0, rawz = 0;
bool down = false, pressed = false, released = false;

void pollTouch() {
  static uint8_t miss = 0;
  bool d = false;
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    rawx = p.x; rawy = p.y; rawz = p.z;
    if (p.z > 200) {
      d = true;
      tx = constrain(map(p.x, 200, 3800, 0, SW - 1), 0, SW - 1);
      ty = constrain(map(p.y, 240, 3800, 0, SH - 1), 0, SH - 1);
    }
  }
  pressed = false; released = false;
  if (d) { miss = 0; if (!down) pressed = true; down = true; }
  else if (down) { if (++miss > 3) { down = false; released = true; miss = 0; } }
}
bool hit(int x, int y, int w, int h) { return pressed && tx >= x && tx < x + w && ty >= y && ty < y + h; }
bool backHit() { return hit(0, 0, 44, 28); }

// ---------------- hardware helpers ----------------
void setBright(uint8_t v) { ledcWrite(0, v); }
void beep(int f, int ms) { if (!S.sound) return; ledcWriteTone(1, f); delay(ms); ledcWriteTone(1, 0); }
void setLed(bool r, bool g, bool b) { digitalWrite(4, !r); digitalWrite(16, !g); digitalWrite(17, !b); }

void loadSettings() {
  prefs.begin("cyd", false);
  S.bright = prefs.getUChar("br", 200); S.sound = prefs.getBool("snd", true);
  S.autodim = prefs.getBool("ad", false); S.invert = prefs.getBool("inv", false);
  S.ssid = prefs.getString("ssid", ""); S.pass = prefs.getString("pass", "");
}
void saveSettings() {
  prefs.putUChar("br", S.bright); prefs.putBool("snd", S.sound);
  prefs.putBool("ad", S.autodim); prefs.putBool("inv", S.invert);
}

// ---------------- drawing helpers ----------------
void txt(const String &s, int x, int y, int f, uint16_t fg, uint16_t bg, uint8_t d = TL_DATUM, int pad = 0) {
  tft.setTextColor(fg, bg); tft.setTextDatum(d); tft.setTextPadding(pad);
  tft.drawString(s, x, y, f); tft.setTextPadding(0);
}
void btn(int x, int y, int w, int h, uint16_t c, const String &t, int f = 2, uint16_t fg = 0xFFFF) {
  tft.fillRoundRect(x, y, w, h, 6, c); txt(t, x + w / 2, y + h / 2, f, fg, c, MC_DATUM);
}
void header(const char *t) {
  tft.fillRect(0, 0, SW, 24, C_PANEL);
  txt("<", 14, 12, 4, C_ACC, C_PANEL, MC_DATUM);
  txt(t, SW / 2, 12, 2, C_TXT, C_PANEL, MC_DATUM);
}
void hdrInfo(const String &s) { txt(s, SW - 6, 12, 2, C_WARN, C_PANEL, MR_DATUM, 130); }
void clearBody() { tft.fillRect(0, 24, SW, SH - 24, C_BG); }
void toast(const String &s) {
  tft.fillRoundRect(40, 100, 240, 40, 8, C_PANEL); tft.drawRoundRect(40, 100, 240, 40, 8, C_ACC);
  txt(s, SW / 2, 120, 2, C_TXT, C_PANEL, MC_DATUM);
}
bool getT(tm &t) { return getLocalTime(&t, 0) && t.tm_year >= 124; }

// ---------------- on-screen keyboard ----------------
const char *KP[3][4] = {
  {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@"},
  {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._@"},
  {"!@#$%^&*()", "~`+=[]{}<>", "|\\;:'\",/?_", "-_.#&()=:/"}};

void kbKeys(int page) {
  tft.fillRect(0, 58, SW, SH - 58, C_BG);
  for (int r = 0; r < 4; r++) for (int c = 0; c < 10; c++) {
    char s[2] = {KP[page][r][c], 0};
    btn(c * 32 + 1, 60 + r * 36, 30, 34, C_PANEL, s, 2);
  }
  btn(1, 206, 58, 32, page == 1 ? C_ACC : C_PANEL, "Aa");
  btn(61, 206, 58, 32, page == 2 ? C_ACC : C_PANEL, "#+=");
  btn(121, 206, 98, 32, C_PANEL, "space");
  btn(221, 206, 48, 32, C_BAD, "Del");
  btn(271, 206, 48, 32, C_OK, "OK");
}
void kbField(const String &t, bool mask) {
  String s = t;
  if (mask) { s = ""; for (unsigned i = 0; i < t.length(); i++) s += '*'; }
  if (s.length() > 24) s = s.substring(s.length() - 24);
  tft.fillRect(4, 26, 312, 28, 0x0000); txt(s + "_", 8, 31, 2, C_TXT, 0x0000);
}
bool kbInput(const char *title, String &out, bool mask) {
  int page = 0; String t = "";
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, SW, 22, C_PANEL);
  txt(title, 6, 3, 2, C_TXT, C_PANEL); txt("X", 300, 11, 2, C_BAD, C_PANEL, MC_DATUM);
  kbKeys(page); kbField(t, mask);
  while (true) {
    pollTouch();
    if (pressed) {
      if (ty < 24 && tx > 270) return false;
      if (ty >= 60 && ty < 204) {
        int r = (ty - 60) / 36, c = tx / 32;
        if (r < 4 && c < 10 && t.length() < 63) { t += KP[page][r][c]; kbField(t, mask); beep(1800, 8); }
      } else if (ty >= 204) {
        if (tx < 60) { page = page == 1 ? 0 : 1; kbKeys(page); }
        else if (tx < 120) { page = page == 2 ? 0 : 2; kbKeys(page); }
        else if (tx < 220) { t += ' '; kbField(t, mask); }
        else if (tx < 270) { if (t.length()) t.remove(t.length() - 1); kbField(t, mask); }
        else { out = t; return true; }
      }
    }
    delay(5);
  }
}

// ---------------- WiFi helpers ----------------
bool wifiConnect(const String &ssid, const String &pass) {
  tft.fillScreen(C_BG);
  txt("Connecting to", SW / 2, 90, 2, C_DIM, C_BG, MC_DATUM);
  txt(ssid, SW / 2, 120, 4, C_TXT, C_BG, MC_DATUM);
  WiFi.disconnect(); WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) { S.ssid = ssid; S.pass = pass; prefs.putString("ssid", ssid); prefs.putString("pass", pass); }
  txt(ok ? "Connected!" : "Failed", SW / 2, 160, 4, ok ? C_OK : C_BAD, C_BG, MC_DATUM);
  delay(1000);
  return ok;
}

// ---------------- weather ----------------
struct Wx { bool ok = false; float t = 0, wind = 0; int hum = 0, code = 0; uint32_t last = 0; } wx;
void fetchWx() {
  wx.last = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient h; h.setTimeout(6000);
  if (!h.begin(cl, "https://api.open-meteo.com/v1/forecast?latitude=" WX_LAT "&longitude=" WX_LON
                   "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m")) return;
  if (h.GET() == 200) {
    JsonDocument d;
    if (!deserializeJson(d, h.getString())) {
      JsonObject c = d["current"];
      wx.t = c["temperature_2m"]; wx.hum = c["relative_humidity_2m"];
      wx.code = c["weather_code"]; wx.wind = c["wind_speed_10m"]; wx.ok = true;
    }
  }
  h.end();
}
const char *wxText(int c) {
  if (c == 0) return "Clear"; if (c == 1) return "Mostly clear"; if (c == 2) return "Partly cloudy";
  if (c == 3) return "Overcast"; if (c == 45 || c == 48) return "Fog"; if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain"; if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers"; if (c >= 95) return "Thunderstorm"; return "--";
}

// ================= HOME =================
const char *NAMES[8] = {"Clock", "WiFi", "Games", "Draw", "Focus", "Test", "Setup", "Info"};
const uint16_t TCOL[8] = {0x04FF, 0x07E0, 0xFD20, 0xF81F, 0xF800, 0x8410, 0x5AEB, 0x07FF};
void icon(int i, int cx, int cy) {
  uint16_t w = C_TXT;
  switch (i) {
    case 0: tft.drawCircle(cx, cy, 18, w); tft.drawLine(cx, cy, cx, cy - 12, w); tft.drawLine(cx, cy, cx + 8, cy + 4, w); break;
    case 1: for (int k = 0; k < 4; k++) tft.fillRect(cx - 16 + k * 9, cy + 14 - (k + 1) * 8, 6, (k + 1) * 8, w); break;
    case 2: tft.fillRoundRect(cx - 20, cy - 10, 40, 22, 8, w); tft.fillCircle(cx + 9, cy, 3, C_BAD); tft.fillRect(cx - 14, cy - 1, 10, 3, C_BG); tft.fillRect(cx - 11, cy - 4, 3, 10, C_BG); break;
    case 3: tft.drawLine(cx - 14, cy + 14, cx + 12, cy - 12, w); tft.drawLine(cx - 13, cy + 14, cx + 13, cy - 12, w); tft.drawLine(cx - 15, cy + 14, cx + 11, cy - 12, w); tft.fillCircle(cx - 14, cy + 14, 3, C_BG); break;
    case 4: tft.fillCircle(cx, cy, 17, w); tft.fillCircle(cx, cy, 11, TCOL[4]); tft.fillCircle(cx, cy, 4, w); break;
    case 5: tft.drawLine(cx - 16, cy, cx + 16, cy, w); tft.drawLine(cx, cy - 16, cx, cy + 16, w); tft.drawCircle(cx, cy, 10, w); break;
    case 6: tft.fillCircle(cx, cy, 14, w); tft.fillCircle(cx, cy, 6, TCOL[6]); for (int a = 0; a < 8; a++) tft.fillCircle(cx + 17 * cos(a * 0.785f), cy + 17 * sin(a * 0.785f), 3, w); break;
    case 7: txt("i", cx, cy, 6, w, TCOL[7], MC_DATUM); break;
  }
}
int homeSec = -1;
void homeTime() {
  tm t; char b[16];
  if (getT(t)) snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min); else strcpy(b, "--:--");
  txt(b, SW - 8, 12, 2, C_TXT, C_PANEL, MR_DATUM, 50);
  tft.fillCircle(SW - 70, 12, 4, WiFi.status() == WL_CONNECTED ? C_OK : C_BAD);
}
void homeApp() {
  if (needInit) {
    needInit = false; tft.fillScreen(C_BG);
    tft.fillRect(0, 0, SW, 24, C_PANEL); txt("CYD OS", 8, 12, 2, C_ACC, C_PANEL, ML_DATUM);
    for (int i = 0; i < 8; i++) {
      int x = 8 + (i % 4) * 78, y = 36 + (i / 4) * 100;
      tft.fillRoundRect(x, y, 72, 90, 10, TCOL[i]);
      icon(i, x + 36, y + 36);
      txt(NAMES[i], x + 36, y + 74, 2, C_TXT, TCOL[i], MC_DATUM);
    }
    homeTime(); homeSec = 0; setLed(0, 0, 0);
  }
  static uint32_t t0 = 0;
  if (millis() - t0 > 5000) { t0 = millis(); homeTime(); }
  if (pressed) for (int i = 0; i < 8; i++) {
    int x = 8 + (i % 4) * 78, y = 36 + (i / 4) * 100;
    if (hit(x, y, 72, 90)) { beep(1500, 15); goApp(i + 1); }
  }
}

// ================= CLOCK =================
const uint16_t CLK_COL[4] = {0xFFFF, 0x07FF, 0xFFE0, 0x07E0};
int clkCol = 0, clkSec = -1;
void clockApp() {
  if (needInit) {
    needInit = false; tft.fillScreen(C_BG); header("Clock"); clkSec = -1;
    if (WiFi.status() == WL_CONNECTED && (!wx.ok || millis() - wx.last > 900000UL)) {
      txt("Updating weather...", SW / 2, 160, 2, C_DIM, C_BG, MC_DATUM); fetchWx(); tft.fillRect(0, 140, SW, 100, C_BG);
    }
    tft.drawFastHLine(10, 140, 300, C_PANEL);
    txt(WX_CITY, 12, 148, 2, C_DIM, C_BG);
    if (!wx.ok) txt(WiFi.status() == WL_CONNECTED ? "No weather data" : "Set WiFi in the WiFi app", SW / 2, 190, 2, C_DIM, C_BG, MC_DATUM);
    else {
      txt(String((int)roundf(wx.t)), 62, 190, 6, C_TXT, C_BG, MC_DATUM, 90);
      txt("C", 112, 178, 4, C_ACC, C_BG);
      txt(wxText(wx.code), 220, 172, 4, C_TXT, C_BG, MC_DATUM);
      txt("Hum " + String(wx.hum) + "%  Wind " + String((int)wx.wind), 220, 204, 2, C_DIM, C_BG, MC_DATUM);
    }
  }
  if (backHit()) { goApp(A_HOME); return; }
  if (pressed && ty > 28) { clkCol = (clkCol + 1) % 4; clkSec = -1; }
  if (WiFi.status() == WL_CONNECTED && millis() - wx.last > 900000UL) { fetchWx(); needInit = true; return; }
  tm t;
  if (getT(t)) {
    if (t.tm_sec != clkSec) {
      clkSec = t.tm_sec; char b[16];
      snprintf(b, sizeof b, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
      txt(b, SW / 2, 74, 6, CLK_COL[clkCol], C_BG, MC_DATUM, 300);
      strftime(b, sizeof b, "%a %d %b", &t);
      txt(b, SW / 2, 120, 4, C_DIM, C_BG, MC_DATUM, 200);
    }
  } else if (clkSec != -2) {
    clkSec = -2; txt("Waiting for time...", SW / 2, 80, 4, C_DIM, C_BG, MC_DATUM, 300);
  }
}

// ================= WIFI TOOLKIT =================
int wtab = 0, wn = 0, woff = 0, wy0 = 0; bool wscan = false, wredraw = true;
int8_t whist[270]; int whn = 0; uint32_t wT = 0; float wspd = -2;

void bars(int x, int y, int rssi) {
  int n = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
  for (int k = 0; k < 4; k++) tft.fillRect(x + k * 6, y + 14 - (k + 1) * 4, 4, (k + 1) * 4, k < n ? C_OK : C_PANEL);
}
void wTabs() { const char *n[4] = {"Scan", "Chan", "Meter", "Speed"};
  for (int i = 0; i < 4; i++) btn(i * 80 + 1, 210, 78, 28, i == wtab ? C_ACC : C_PANEL, n[i]); }
void wStartScan() { WiFi.scanDelete(); WiFi.scanNetworks(true); wscan = true; wn = 0; woff = 0; }
void wDrawScan() {
  tft.fillRect(0, 24, SW, 184, C_BG);
  if (wscan) { txt("Scanning...", SW / 2, 110, 4, C_DIM, C_BG, MC_DATUM); return; }
  if (wn <= 0) { txt("No networks. Tap Scan.", SW / 2, 110, 2, C_DIM, C_BG, MC_DATUM); return; }
  for (int i = 0; i < 6 && woff + i < wn; i++) {
    int idx = woff + i, y = 26 + i * 30;
    tft.fillRect(0, y, SW, 29, i % 2 ? C_BG : C_PANEL);
    String s = WiFi.SSID(idx); if (!s.length()) s = "(hidden)"; if (s.length() > 18) s = s.substring(0, 18);
    uint16_t bg = i % 2 ? C_BG : C_PANEL;
    txt(s, 6, y + 7, 2, C_TXT, bg);
    if (WiFi.encryptionType(idx) != WIFI_AUTH_OPEN) txt("sec", 190, y + 11, 1, C_WARN, bg);
    txt(String(WiFi.RSSI(idx)), 262, y + 7, 2, C_DIM, bg, TR_DATUM);
    bars(284, y + 6, WiFi.RSSI(idx));
  }
  if (wn > 6) txt(String(woff + 1) + "-" + String(min(woff + 6, wn)) + " of " + String(wn) + " (swipe)", SW - 4, 196, 1, C_DIM, C_BG, TR_DATUM);
}
void wDrawChan() {
  tft.fillRect(0, 24, SW, 184, C_BG);
  float sc[16] = {0};
  for (int i = 0; i < wn; i++) {
    int ch = WiFi.channel(i); float w = max(0, WiFi.RSSI(i) + 100) / 10.0f;
    for (int d = -2; d <= 2; d++) { int c = ch + d; if (c >= 1 && c <= 13) sc[c] += w / (1 + abs(d)); }
  }
  float mx = 1; for (int c = 1; c <= 13; c++) mx = max(mx, sc[c]);
  for (int c = 1; c <= 13; c++) {
    int h = (int)(sc[c] / mx * 120), x = 8 + (c - 1) * 24;
    uint16_t col = sc[c] < mx * 0.34f ? C_OK : sc[c] < mx * 0.67f ? C_WARN : C_BAD;
    tft.fillRect(x, 170 - h, 20, h, col);
    txt(String(c), x + 10, 182, 2, C_TXT, C_BG, TC_DATUM);
  }
  int best = 1; for (int c : {1, 6, 11}) if (sc[c] < sc[best] || best == 1) { if (sc[c] <= sc[best]) best = c; }
  txt("Least crowded of 1/6/11: " + String(best), SW / 2, 36, 2, C_ACC, C_BG, MC_DATUM);
}
void wDrawMeter() { tft.fillRect(0, 24, SW, 184, C_BG); whn = 0; wT = 0; }
void wMeterTick() {
  if (millis() - wT < 250) return; wT = millis();
  if (WiFi.status() != WL_CONNECTED) { txt("Connect to a network first (Scan tab)", SW / 2, 110, 2, C_DIM, C_BG, MC_DATUM); return; }
  int r = WiFi.RSSI();
  if (whn < 270) whist[whn++] = r; else { memmove(whist, whist + 1, 269); whist[269] = r; }
  txt(String(r), 70, 50, 6, r > -60 ? C_OK : r > -75 ? C_WARN : C_BAD, C_BG, MC_DATUM, 130);
  txt("dBm", 140, 56, 4, C_DIM, C_BG);
  txt(WiFi.SSID(), 310, 40, 2, C_TXT, C_BG, TR_DATUM, 140);
  txt(r > -55 ? "Excellent" : r > -67 ? "Good" : r > -78 ? "Fair" : "Weak", 310, 62, 2, C_DIM, C_BG, TR_DATUM, 140);
  tft.fillRect(15, 95, 290, 108, 0x0000);
  for (int i = 0; i < whn; i++) {
    int h = constrain((whist[i] + 100) * 100 / 70, 0, 100);
    tft.drawFastVLine(25 + i, 202 - h, h, whist[i] > -60 ? C_OK : whist[i] > -75 ? C_WARN : C_BAD);
  }
}
void wDrawSpeed() {
  tft.fillRect(0, 24, SW, 184, C_BG);
  btn(80, 120, 160, 46, C_ACC, "Start test", 4, 0x0000);
  if (WiFi.status() != WL_CONNECTED) txt("Not connected", SW / 2, 70, 4, C_BAD, C_BG, MC_DATUM);
  else if (wspd > 0) { txt(String(wspd, 1), SW / 2, 62, 6, C_OK, C_BG, MC_DATUM); txt("Mbps download", SW / 2, 96, 2, C_DIM, C_BG, MC_DATUM); }
  else if (wspd == -1) txt("Test failed", SW / 2, 70, 4, C_BAD, C_BG, MC_DATUM);
  else txt("Downloads 3 MB to measure speed", SW / 2, 70, 2, C_DIM, C_BG, MC_DATUM);
}
void runSpeed() {
  tft.fillRect(0, 24, SW, 184, C_BG); txt("Testing...", SW / 2, 110, 4, C_WARN, C_BG, MC_DATUM);
  WiFiClientSecure cl; cl.setInsecure(); HTTPClient h; h.setTimeout(8000);
  wspd = -1;
  if (h.begin(cl, "https://speed.cloudflare.com/__down?bytes=3000000") && h.GET() == 200) {
    WiFiClient *st = h.getStreamPtr(); static uint8_t buf[1024];
    uint32_t tot = 0, t0 = millis();
    while (h.connected() && tot < 3000000 && millis() - t0 < 12000) {
      int a = st->available();
      if (a > 0) tot += st->read(buf, a > 1024 ? 1024 : a); else delay(1);
    }
    uint32_t ms = millis() - t0;
    if (tot > 100000 && ms > 0) wspd = tot * 8.0f / (ms * 1000.0f);
  }
  h.end();
}
void wifiJoin(int idx) {
  String ssid = WiFi.SSID(idx), pw = "";
  if (WiFi.encryptionType(idx) != WIFI_AUTH_OPEN) { if (!kbInput(("Password: " + ssid).c_str(), pw, true)) { needInit = true; return; } }
  wifiConnect(ssid, pw); needInit = true;
}
void wifiApp() {
  if (needInit) { needInit = false; tft.fillScreen(C_BG); header("WiFi Toolkit"); wTabs(); wtab = 0; wStartScan(); wDrawScan(); }
  if (backHit()) { goApp(A_HOME); return; }
  hdrInfo(WiFi.status() == WL_CONNECTED ? WiFi.SSID().substring(0, 12) : "offline");
  if (hit(0, 210, SW, 30)) {
    int t = tx / 80;
    if (t == 0 && wtab == 0) wStartScan();
    wtab = t; wTabs();
    if (wtab == 0) wDrawScan(); else if (wtab == 1) wDrawChan(); else if (wtab == 2) wDrawMeter(); else wDrawSpeed();
  }
  if (wscan) {
    int n = WiFi.scanComplete();
    if (n >= 0) { wscan = false; wn = n; if (wtab == 0) wDrawScan(); else if (wtab == 1) wDrawChan(); }
  }
  if (wtab == 0 && !wscan) {
    if (pressed && ty > 26 && ty < 208) wy0 = ty;
    if (released && wy0 > 0) {
      int dy = ty - wy0;
      if (abs(dy) > 25) { woff = constrain(woff + (dy < 0 ? 3 : -3), 0, max(0, wn - 6)); wDrawScan(); }
      else { int i = (wy0 - 26) / 30; if (woff + i < wn && i < 6) wifiJoin(woff + i); }
      wy0 = 0;
    }
  }
  if (wtab == 2) wMeterTick();
  if (wtab == 3 && hit(80, 120, 160, 46) && WiFi.status() == WL_CONNECTED) { runSpeed(); wDrawSpeed(); }
}

// ================= GAMES =================
int gsub = 0;
void gamesMenu() {
  tft.fillScreen(C_BG); header("Games"); gsub = 0;
  btn(20, 40, 280, 50, C_OK, "Snake", 4, 0x0000);
  btn(20, 100, 280, 50, C_ACC, "Pong", 4, 0x0000);
  btn(20, 160, 280, 50, C_WARN, "2048", 4, 0x0000);
}
// ---- Snake ----
#define SG 12
#define SCOLS 26
#define SROWS 18
struct Pt { int8_t x, y; };
Pt sn[SCOLS * SROWS]; int slen, sdir, sndir, sscore, shs; Pt sfood; uint32_t sT; bool sdead;
const int8_t DX[4] = {1, 0, -1, 0}, DY[4] = {0, 1, 0, -1};
void sCell(int x, int y, uint16_t c) { tft.fillRect(4 + x * SG + 1, 24 + y * SG + 1, SG - 2, SG - 2, c); }
void sPlaceFood() {
  bool ok;
  do { ok = true; sfood.x = random(SCOLS); sfood.y = random(SROWS);
    for (int i = 0; i < slen; i++) if (sn[i].x == sfood.x && sn[i].y == sfood.y) { ok = false; break; } } while (!ok);
  sCell(sfood.x, sfood.y, C_BAD);
}
void gameOver(int score, int &hs, const char *key) {
  if (score > hs) { hs = score; prefs.putInt(key, hs); }
  tft.fillRoundRect(60, 85, 200, 80, 8, C_PANEL); tft.drawRoundRect(60, 85, 200, 80, 8, C_BAD);
  txt("GAME OVER", SW / 2, 105, 4, C_BAD, C_PANEL, MC_DATUM);
  txt("Score " + String(score) + "   Best " + String(hs), SW / 2, 132, 2, C_TXT, C_PANEL, MC_DATUM);
  txt("tap to retry", SW / 2, 152, 2, C_DIM, C_PANEL, MC_DATUM);
  beep(300, 250);
}
void snakeInit() {
  tft.fillScreen(C_BG); header("Snake"); tft.fillRect(4, 24, 312, 216, 0x10A2);
  slen = 3; for (int i = 0; i < 3; i++) { sn[i].x = 10 - i; sn[i].y = 9; sCell(sn[i].x, sn[i].y, C_OK); }
  sdir = sndir = 0; sscore = 0; sdead = false; shs = prefs.getInt("hsn", 0);
  sPlaceFood(); hdrInfo("Score 0"); sT = millis();
}
bool snakeLoop() {
  if (backHit()) return true;
  if (sdead) { if (pressed && ty > 28) snakeInit(); return false; }
  if (down && ty > 28) {
    int hx = 4 + sn[0].x * SG + SG / 2, hy = 24 + sn[0].y * SG + SG / 2, dx = tx - hx, dy = ty - hy;
    if (abs(dx) > 8 || abs(dy) > 8) { int nd = abs(dx) > abs(dy) ? (dx > 0 ? 0 : 2) : (dy > 0 ? 1 : 3); if (nd != (sdir + 2) % 4) sndir = nd; }
  }
  int iv = max(55, 140 - sscore * 4);
  if ((int)(millis() - sT) < iv) return false; sT = millis();
  sdir = sndir; int nx = sn[0].x + DX[sdir], ny = sn[0].y + DY[sdir];
  bool grow = (nx == sfood.x && ny == sfood.y);
  bool bad = nx < 0 || ny < 0 || nx >= SCOLS || ny >= SROWS;
  if (!bad) { int lim = grow ? slen : slen - 1; for (int i = 0; i < lim; i++) if (sn[i].x == nx && sn[i].y == ny) { bad = true; break; } }
  if (bad) { sdead = true; gameOver(sscore, shs, "hsn"); return false; }
  if (!grow) sCell(sn[slen - 1].x, sn[slen - 1].y, 0x10A2); else slen++;
  memmove(&sn[1], &sn[0], (slen - 1) * sizeof(Pt));
  sn[0].x = nx; sn[0].y = ny; sCell(nx, ny, C_OK);
  if (grow) { sscore++; beep(1800, 15); sPlaceFood(); hdrInfo("Score " + String(sscore)); }
  return false;
}
// ---- Pong ----
#define PW 6
#define PH 40
#define BS 6
float bx, by, vx, vy, ay; int py, opy, oay, pscore, plives, phs; float obx, oby; uint32_t pT; bool pdead;
void pongServe() { bx = 160; by = 130; vx = (random(2) ? 1 : -1) * 2.6f; vy = random(-15, 16) / 10.0f; obx = bx; oby = by; }
void pongHdr() { hdrInfo("Pts " + String(pscore) + "  Lives " + String(plives)); }
void pongInit() {
  tft.fillScreen(C_BG); header("Pong");
  for (int y = 28; y < 240; y += 16) tft.fillRect(159, y, 2, 8, C_PANEL);
  pscore = 0; plives = 3; pdead = false; py = opy = 130; ay = oay = 130; phs = prefs.getInt("hpg", 0);
  pongServe(); pongHdr(); pT = millis();
  tft.fillRect(8, py - PH / 2, PW, PH, C_ACC); tft.fillRect(306, (int)ay - PH / 2, PW, PH, C_WARN);
}
bool pongLoop() {
  if (backHit()) return true;
  if (pdead) { if (pressed && ty > 28) pongInit(); return false; }
  if (millis() - pT < 16) return false; pT = millis();
  if (down && ty > 28) py = constrain(ty, 24 + PH / 2, 240 - PH / 2);
  float sp = fminf(2.4f + pscore * 0.05f, 4.2f);
  if (ay < by - 3) ay += sp; else if (ay > by + 3) ay -= sp;
  ay = constrain(ay, 24 + PH / 2, 240 - PH / 2);
  bx += vx; by += vy;
  if (by < 24) { by = 24; vy = -vy; } if (by > 240 - BS) { by = 240 - BS; vy = -vy; }
  if (vx < 0 && bx <= 8 + PW && bx + BS >= 8 && by + BS >= py - PH / 2 && by <= py + PH / 2) {
    vx = fminf(-vx * 1.05f, 7); vy = (by + BS / 2 - py) / (PH / 2.0f) * 3.0f; bx = 8 + PW; pscore++; pongHdr(); beep(1200, 8);
  }
  if (vx > 0 && bx + BS >= 306 && bx <= 306 + PW && by + BS >= ay - PH / 2 && by <= ay + PH / 2) { vx = -vx; bx = 306 - BS; beep(800, 8); }
  tft.fillRect((int)obx, (int)oby, BS, BS, C_BG);
  if (bx < 0) { plives--; pongHdr(); if (plives <= 0) { pdead = true; gameOver(pscore, phs, "hpg"); return false; } pongServe(); }
  else if (bx > SW) { pscore += 5; pongHdr(); beep(1500, 30); pongServe(); }
  if (py != opy) { tft.fillRect(8, opy - PH / 2, PW, PH, C_BG); tft.fillRect(8, py - PH / 2, PW, PH, C_ACC); opy = py; }
  if ((int)ay != oay) { tft.fillRect(306, oay - PH / 2, PW, PH, C_BG); tft.fillRect(306, (int)ay - PH / 2, PW, PH, C_WARN); oay = (int)ay; }
  tft.fillRect((int)bx, (int)by, BS, BS, C_TXT); obx = bx; oby = by;
  return false;
}
// ---- 2048 ----
int g[4][4], gs, ghs, sx0, sy0; bool gover;
const uint16_t TC[12] = {0x2104, 0xEF7D, 0xEF3A, 0xFD8F, 0xFC6A, 0xFB2C, 0xFA08, 0xEEAA, 0xEE88, 0xEE46, 0xEE04, 0x07E0};
void g2Tile(int r, int c) {
  int x = 10 + c * 52, y = 30 + r * 52, v = g[r][c];
  uint16_t bg = TC[min(v, 11)];
  tft.fillRoundRect(x, y, 48, 48, 5, bg);
  if (v) txt(String(1 << v), x + 24, y + 24, v < 10 ? 4 : 2, v < 3 ? 0x2104 : 0xFFFF, bg, MC_DATUM);
}
void g2Side() {
  txt("SCORE", 270, 36, 2, C_DIM, C_BG, TC_DATUM); txt(String(gs), 270, 56, 4, C_TXT, C_BG, TC_DATUM, 90);
  txt("BEST", 270, 96, 2, C_DIM, C_BG, TC_DATUM); txt(String(ghs), 270, 116, 4, C_WARN, C_BG, TC_DATUM, 90);
}
void g2Spawn() {
  int e[16][2], n = 0;
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) if (!g[r][c]) { e[n][0] = r; e[n][1] = c; n++; }
  if (!n) return; int k = random(n); g[e[k][0]][e[k][1]] = random(10) ? 1 : 2;
}
void g2Init() {
  tft.fillScreen(C_BG); header("2048"); tft.fillRoundRect(6, 26, 216, 212, 8, C_PANEL);
  memset(g, 0, sizeof g); gs = 0; gover = false; ghs = prefs.getInt("h2048", 0);
  g2Spawn(); g2Spawn();
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) g2Tile(r, c);
  btn(230, 170, 84, 40, C_ACC, "New", 2, 0x0000); txt("swipe to move", 272, 222, 1, C_DIM, C_BG, TC_DATUM);
  g2Side();
}
bool g2Slide(int a[4]) {
  int o[4] = {0, 0, 0, 0}, k = 0; bool merged = false;
  for (int i = 0; i < 4; i++) {
    if (!a[i]) continue;
    if (k > 0 && o[k - 1] == a[i] && !merged) { o[k - 1]++; gs += 1 << o[k - 1]; merged = true; }
    else { o[k++] = a[i]; merged = false; }
  }
  bool ch = false; for (int i = 0; i < 4; i++) { if (o[i] != a[i]) ch = true; a[i] = o[i]; } return ch;
}
bool g2Move(int d) { // 0 L,1 R,2 U,3 D
  bool ch = false;
  for (int i = 0; i < 4; i++) {
    int a[4];
    for (int j = 0; j < 4; j++) a[j] = d == 0 ? g[i][j] : d == 1 ? g[i][3 - j] : d == 2 ? g[j][i] : g[3 - j][i];
    if (g2Slide(a)) ch = true;
    for (int j = 0; j < 4; j++) { if (d == 0) g[i][j] = a[j]; else if (d == 1) g[i][3 - j] = a[j]; else if (d == 2) g[j][i] = a[j]; else g[3 - j][i] = a[j]; }
  }
  return ch;
}
bool g2Stuck() {
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
    if (!g[r][c]) return false;
    if (c < 3 && g[r][c] == g[r][c + 1]) return false;
    if (r < 3 && g[r][c] == g[r + 1][c]) return false;
  } return true;
}
bool g2Loop() {
  if (backHit()) return true;
  if (hit(230, 170, 84, 40)) { g2Init(); return false; }
  if (pressed) { sx0 = tx; sy0 = ty; }
  if (released && !gover && sy0 > 28 && sx0 < 224) {
    int dx = tx - sx0, dy = ty - sy0;
    if (abs(dx) > 25 || abs(dy) > 25) {
      int d = abs(dx) > abs(dy) ? (dx < 0 ? 0 : 1) : (dy < 0 ? 2 : 3);
      if (g2Move(d)) {
        g2Spawn(); beep(900, 8);
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) g2Tile(r, c);
        if (gs > ghs) { ghs = gs; prefs.putInt("h2048", ghs); }
        g2Side();
        if (g2Stuck()) { gover = true; tft.fillRoundRect(40, 110, 140, 40, 8, C_PANEL); txt("No moves!", 110, 130, 4, C_BAD, C_PANEL, MC_DATUM); beep(300, 250); }
      }
    }
  }
  return false;
}
void gamesApp() {
  if (needInit) { needInit = false; gamesMenu(); }
  if (gsub == 0) {
    if (backHit()) { goApp(A_HOME); return; }
    if (hit(20, 40, 280, 50)) { gsub = 1; snakeInit(); }
    else if (hit(20, 100, 280, 50)) { gsub = 2; pongInit(); }
    else if (hit(20, 160, 280, 50)) { gsub = 3; g2Init(); }
    return;
  }
  bool ex = gsub == 1 ? snakeLoop() : gsub == 2 ? pongLoop() : g2Loop();
  if (ex) gamesMenu();
}

// ================= DRAW (pixel art, saved to flash) =================
#define CW 80
#define CHH 44
uint8_t cv[CW * CHH]; int dcol = 1, dslot = 0, dlx = -1, dly = -1;
const uint16_t PAL[8] = {0x0000, 0xFFFF, 0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x001F, 0xF81F};
void dPalette() { for (int i = 0; i < 8; i++) { tft.fillRect(i * 40, 176, 40, 32, PAL[i]); if (i == dcol) { tft.drawRect(i * 40 + 1, 177, 38, 30, i == 1 ? C_BAD : C_TXT); tft.drawRect(i * 40 + 2, 178, 36, 28, i == 1 ? C_BAD : C_TXT); } else tft.drawRect(i * 40, 176, 40, 32, C_PANEL); } }
void dButtons() { btn(0, 208, 64, 32, C_PANEL, "Back"); btn(64, 208, 64, 32, C_PANEL, "Clear"); btn(128, 208, 64, 32, C_PANEL, "Slot " + String(dslot + 1)); btn(192, 208, 64, 32, C_OK, "Save", 2, 0); btn(256, 208, 64, 32, C_ACC, "Load", 2, 0); }
void dRedraw() { for (int y = 0; y < CHH; y++) for (int x = 0; x < CW; x++) tft.fillRect(x * 4, y * 4, 4, 4, PAL[cv[y * CW + x]]); }
void dSet(int x, int y) { if (x < 0 || y < 0 || x >= CW || y >= CHH) return; if (cv[y * CW + x] == dcol) return; cv[y * CW + x] = dcol; tft.fillRect(x * 4, y * 4, 4, 4, PAL[dcol]); }
String dPath() { return "/d" + String(dslot) + ".bin"; }
void drawApp() {
  if (needInit) { needInit = false; memset(cv, 0, sizeof cv); tft.fillScreen(0x0000); dPalette(); dButtons(); dlx = dly = -1; }
  if (hit(0, 208, 64, 32)) { goApp(A_HOME); return; }
  if (hit(64, 208, 64, 32)) { memset(cv, 0, sizeof cv); tft.fillRect(0, 0, 320, 176, 0); }
  if (hit(128, 208, 64, 32)) { dslot = (dslot + 1) % 4; dButtons(); }
  if (hit(192, 208, 64, 32)) { File f = LittleFS.open(dPath(), "w"); if (f) { f.write(cv, sizeof cv); f.close(); btn(192, 208, 64, 32, C_OK, "Saved!", 2, 0); } else btn(192, 208, 64, 32, C_BAD, "Error"); delay(600); dButtons(); }
  if (hit(256, 208, 64, 32)) {
    File f = LittleFS.open(dPath(), "r");
    if (f && f.size() == sizeof cv) { f.read(cv, sizeof cv); f.close(); dRedraw(); } else btn(256, 208, 64, 32, C_BAD, "Empty");
    delay(500); dButtons();
  }
  if (hit(0, 176, 320, 32)) { dcol = tx / 40; dPalette(); }
  if (down && ty < 176) {
    int cx = tx / 4, cy = ty / 4;
    if (dlx < 0) { dSet(cx, cy); }
    else { int n = max(abs(cx - dlx), abs(cy - dly)); for (int i = 1; i <= n; i++) dSet(dlx + (cx - dlx) * i / n, dly + (cy - dly) * i / n); }
    dlx = cx; dly = cy;
  } else if (!down) { dlx = dly = -1; }
}

// ================= FOCUS (Pomodoro) =================
const int PMIN[3] = {25, 5, 15}; const char *PNAME[3] = {"Focus", "Short", "Long"};
int pmode = 0, pdone = 0, plastSec = -1; bool prun = false; uint32_t pend = 0, prem = 25 * 60000UL;
int todayKey() { tm t; return getT(t) ? t.tm_year * 400 + t.tm_yday : 0; }
void pDrawModes() { for (int i = 0; i < 3; i++) btn(10 + i * 104, 30, 96, 30, i == pmode ? (i == 0 ? C_BAD : C_OK) : C_PANEL, PNAME[i]); }
void pDrawBtns() { btn(20, 176, 130, 44, prun ? C_WARN : C_OK, prun ? "Pause" : "Start", 4, 0); btn(170, 176, 130, 44, C_PANEL, "Reset", 4); }
void pDrawTime() {
  int s = (prem + 999) / 1000; char b[8]; snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
  txt(b, SW / 2, 105, 6, C_TXT, C_BG, MC_DATUM, 200);
  uint32_t tot = PMIN[pmode] * 60000UL; int w = (int)(280UL * (tot - prem) / tot);
  tft.fillRect(20, 142, 280, 12, C_PANEL); tft.fillRect(20, 142, w, 12, pmode == 0 ? C_BAD : C_OK);
  txt("Focus sessions today: " + String(pdone), SW / 2, 228, 2, C_DIM, C_BG, MC_DATUM, 250);
}
void pSetMode(int m) { pmode = m; prun = false; prem = PMIN[m] * 60000UL; setLed(0, 0, 0); pDrawModes(); pDrawBtns(); pDrawTime(); }
void focusApp() {
  if (needInit) {
    needInit = false; tft.fillScreen(C_BG); header("Focus Timer");
    int k = todayKey(); if (prefs.getInt("pday", 0) != k) { prefs.putInt("pday", k); prefs.putInt("pcnt", 0); }
    pdone = prefs.getInt("pcnt", 0); pDrawModes(); pDrawBtns(); pDrawTime();
  }
  if (backHit()) { setLed(0, 0, 0); goApp(A_HOME); return; }
  for (int i = 0; i < 3; i++) if (hit(10 + i * 104, 30, 96, 30)) pSetMode(i);
  if (hit(20, 176, 130, 44)) {
    prun = !prun; if (prun) { pend = millis() + prem; if (pmode == 0) setLed(1, 0, 0); else setLed(0, 1, 0); } else setLed(0, 0, 1);
    pDrawBtns();
  }
  if (hit(170, 176, 130, 44)) pSetMode(pmode);
  if (prun) {
    prem = (int32_t)(pend - millis()) > 0 ? pend - millis() : 0;
    if ((int)((prem + 999) / 1000) != plastSec) { plastSec = (prem + 999) / 1000; pDrawTime(); }
    if (prem == 0) {
      for (int i = 0; i < 3; i++) { beep(1500, 150); delay(100); }
      if (pmode == 0) { pdone++; prefs.putInt("pcnt", pdone); pSetMode(pdone % 4 == 0 ? 2 : 1); } else pSetMode(0);
    }
  }
}

// ================= HARDWARE TEST =================
uint32_t tT = 0;
void testApp() {
  if (needInit) {
    needInit = false; tft.fillScreen(C_BG); header("Hardware Test");
    btn(4, 82, 74, 32, C_BAD, "LED R"); btn(82, 82, 74, 32, C_OK, "LED G", 2, 0); btn(160, 82, 74, 32, 0x001F, "LED B"); btn(238, 82, 78, 32, C_PANEL, "LED off");
    btn(4, 120, 150, 32, C_ACC, "Beep", 2, 0); btn(166, 120, 150, 32, C_WARN, "Color cycle", 2, 0);
    tft.drawRect(0, 158, SW, 82, C_PANEL); txt("touch here to draw", SW / 2, 199, 2, C_PANEL, C_BG, MC_DATUM);
  }
  if (backHit()) { setLed(0, 0, 0); goApp(A_HOME); return; }
  if (hit(4, 82, 74, 32)) setLed(1, 0, 0); if (hit(82, 82, 74, 32)) setLed(0, 1, 0);
  if (hit(160, 82, 74, 32)) setLed(0, 0, 1); if (hit(238, 82, 78, 32)) setLed(0, 0, 0);
  if (hit(4, 120, 150, 32)) { bool s = S.sound; S.sound = true; beep(880, 150); beep(1320, 150); S.sound = s; }
  if (hit(166, 120, 150, 32)) { const uint16_t c[5] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000}; for (int i = 0; i < 5; i++) { tft.fillScreen(c[i]); delay(500); } needInit = true; return; }
  if (down && ty > 160) { tft.fillCircle(tx, ty, 3, C_ACC); tft.drawFastHLine(tx - 8, ty, 17, C_WARN); tft.drawFastVLine(tx, ty - 8, 17, C_WARN); }
  if (millis() - tT > 250) {
    tT = millis();
    txt("Touch raw x:" + String(rawx) + " y:" + String(rawy) + " z:" + String(rawz), 6, 30, 2, C_TXT, C_BG, TL_DATUM, 300);
    txt("Screen x:" + String(tx) + " y:" + String(ty) + (down ? " DOWN" : "     "), 6, 48, 2, C_TXT, C_BG, TL_DATUM, 300);
    txt("LDR: " + String(analogRead(34)) + "   Heap: " + String(ESP.getFreeHeap() / 1024) + "KB", 6, 64, 2, C_DIM, C_BG, TL_DATUM, 300);
  }
}

// ================= SETTINGS =================
void setDraw() {
  tft.fillScreen(C_BG); header("Setup");
  txt("Brightness", 20, 34, 2, C_DIM, C_BG);
  tft.fillRoundRect(20, 54, 280, 10, 5, C_PANEL); tft.fillRoundRect(20, 54, map(S.bright, 10, 255, 0, 280), 10, 5, C_ACC);
  btn(20, 78, 280, 34, C_PANEL, String("Sound: ") + (S.sound ? "ON" : "OFF"));
  btn(20, 118, 280, 34, C_PANEL, String("Auto-dim (light sensor): ") + (S.autodim ? "ON" : "OFF"));
  btn(20, 158, 280, 34, C_PANEL, String("Invert colors: ") + (S.invert ? "ON" : "OFF"));
  btn(20, 198, 280, 34, C_BAD, "Forget WiFi: " + (S.ssid.length() ? S.ssid.substring(0, 12) : String("none")));
}
void setApp() {
  if (needInit) { needInit = false; setDraw(); }
  if (backHit()) { goApp(A_HOME); return; }
  if (down && ty >= 40 && ty < 74) { S.bright = constrain(map(tx, 20, 300, 10, 255), 10, 255); setBright(S.bright); tft.fillRect(20, 54, 280, 10, C_BG); tft.fillRoundRect(20, 54, 280, 10, 5, C_PANEL); tft.fillRoundRect(20, 54, map(S.bright, 10, 255, 0, 280), 10, 5, C_ACC); }
  if (released) saveSettings();
  if (hit(20, 78, 280, 34)) { S.sound = !S.sound; saveSettings(); setDraw(); }
  if (hit(20, 118, 280, 34)) { S.autodim = !S.autodim; saveSettings(); setDraw(); }
  if (hit(20, 158, 280, 34)) { S.invert = !S.invert; tft.invertDisplay(S.invert); saveSettings(); setDraw(); }
  if (hit(20, 198, 280, 34)) { S.ssid = ""; S.pass = ""; prefs.remove("ssid"); prefs.remove("pass"); WiFi.disconnect(); setDraw(); }
}

// ================= INFO =================
uint32_t iT = 0;
void infoApp() {
  if (needInit) { needInit = false; tft.fillScreen(C_BG); header("Info"); iT = 0; }
  if (backHit()) { goApp(A_HOME); return; }
  if (millis() - iT < 1000) return; iT = millis();
  String l[8];
  l[0] = String(ESP.getChipModel()) + " x" + ESP.getChipCores() + " @ " + ESP.getCpuFreqMHz() + "MHz";
  l[1] = "Flash: " + String(ESP.getFlashChipSize() / 1048576) + " MB   Heap: " + String(ESP.getFreeHeap() / 1024) + " KB";
  l[2] = "Storage: " + String(LittleFS.usedBytes() / 1024) + " / " + String(LittleFS.totalBytes() / 1024) + " KB";
  l[3] = WiFi.status() == WL_CONNECTED ? "WiFi: " + WiFi.SSID() : String("WiFi: offline");
  l[4] = WiFi.status() == WL_CONNECTED ? "IP: " + WiFi.localIP().toString() + "  " + String(WiFi.RSSI()) + "dBm" : String("IP: -");
  l[5] = "MAC: " + WiFi.macAddress();
  l[6] = "Uptime: " + String(millis() / 60000) + " min";
  l[7] = "LDR: " + String(analogRead(34));
  for (int i = 0; i < 8; i++) txt(l[i], 10, 36 + i * 24, 2, i < 3 ? C_TXT : C_DIM, C_BG, TL_DATUM, 300);
}

// ================= main =================
void autoDim() {
  static uint32_t t = 0; static float lvl = 200;
  if (!S.autodim || millis() - t < 300) return; t = millis();
  int target = constrain(map(analogRead(34), 200, 2500, 255, 25), 25, 255);
  lvl += (target - lvl) * 0.2f; setBright((uint8_t)lvl);
}
void setup() {
  Serial.begin(115200);
  pinMode(4, OUTPUT); pinMode(16, OUTPUT); pinMode(17, OUTPUT); setLed(0, 0, 0);
  loadSettings(); LittleFS.begin(true);
  tft.init(); tft.setRotation(1); tft.invertDisplay(S.invert);
  ledcSetup(0, 5000, 8); ledcAttachPin(21, 0); setBright(S.bright);
  ledcSetup(1, 2000, 8); ledcAttachPin(26, 1);
  tsSPI.begin(25, 39, 32, 33); ts.begin(tsSPI); ts.setRotation(1);
  tft.fillScreen(C_BG); txt("CYD OS", SW / 2, 105, 4, C_ACC, C_BG, MC_DATUM); txt("starting...", SW / 2, 140, 2, C_DIM, C_BG, MC_DATUM);
  WiFi.mode(WIFI_STA); if (S.ssid.length()) WiFi.begin(S.ssid.c_str(), S.pass.c_str());
  configTime(19800, 0, "pool.ntp.org", "time.google.com"); // IST (Surat)
  randomSeed(esp_random()); delay(600);
}
void loop() {
  pollTouch(); autoDim();
  switch (app) {
    case A_HOME: homeApp(); break; case A_CLOCK: clockApp(); break; case A_WIFI: wifiApp(); break;
    case A_GAMES: gamesApp(); break; case A_DRAW: drawApp(); break; case A_FOCUS: focusApp(); break;
    case A_TEST: testApp(); break; case A_SET: setApp(); break; case A_INFO: infoApp(); break;
  }
  delay(3);
}
