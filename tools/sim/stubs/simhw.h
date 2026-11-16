// Fake hardware for the simulator: display framebuffer, touch, RTC, WiFi,
// web server, HTTP client and an in-memory LittleFS.
#pragma once
#include "Arduino.h"
#include <Adafruit_GFX.h>

// ---- display ----
#define ILI9341_WHITE 0xFFFF
#define ILI9341_BLACK 0x0000
struct SPIStub { void begin(int, int, int, int) {} };
extern SPIStub SPI;
class Adafruit_ILI9341 : public Adafruit_GFX {
public:
  uint16_t fb[240 * 320];
  Adafruit_ILI9341(SPIStub*, int, int, int) : Adafruit_GFX(240, 320) { memset(fb, 0, sizeof(fb)); }
  void begin() {}
  void setRotation(uint8_t) {}
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x >= 0 && x < 240 && y >= 0 && y < 320) fb[y * 240 + x] = c;
  }
  uint16_t color565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
};

// ---- touch ----
struct TS_Point { int16_t x = 0, y = 0, z = 0; };
extern bool simTouched; extern TS_Point simTouchPoint;
class XPT2046_Touchscreen {
public:
  XPT2046_Touchscreen(int) {}
  void begin() {}
  void setRotation(int) {}
  bool touched() { return simTouched; }
  TS_Point getPoint() { return simTouchPoint; }
};

// ---- RTC ----
class DateTime {
  int y, mo, d, h, mi, s;
public:
  DateTime(int Y = 2026, int M = 1, int D = 1, int H = 0, int Mi = 0, int S = 0) : y(Y), mo(M), d(D), h(H), mi(Mi), s(S) {}
  int year() const { return y; } int month() const { return mo; } int day() const { return d; }
  int hour() const { return h; } int minute() const { return mi; } int second() const { return s; }
  uint32_t unixtime() const { struct tm t = {}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s; return (uint32_t)timegm(&t); }
  int dayOfTheWeek() const { time_t u = unixtime(); struct tm t; gmtime_r(&u, &t); return t.tm_wday; }
};
struct WireStub { void begin(int, int) {} };
extern WireStub Wire;
extern DateTime simNow;
class RTC_PCF8563 {
public:
  bool begin() { return true; }
  void start() {}
  DateTime now() { return simNow; }
  void adjust(const DateTime& d) { simNow = d; }
};

// ---- filesystem ----
#define FILE_WRITE "w"
#define FILE_APPEND "a"
extern std::map<std::string, std::string> simFiles;
class File : public Print {
  std::string path; std::string data; size_t pos = 0; bool ok = false, writing = false;
public:
  File() {}
  File(const std::string& p, const char* mode) : path(p), ok(true) {
    if (!strcmp(mode, "r")) { if (!simFiles.count(p)) ok = false; else data = simFiles[p]; }
    else { writing = true; if (!strcmp(mode, "a") && simFiles.count(p)) data = simFiles[p]; }
  }
  operator bool() const { return ok; }
  int available() { return ok ? (int)(data.size() - pos) : 0; }
  int read() { return pos < data.size() ? (uint8_t)data[pos++] : -1; }
  size_t write(uint8_t c) override { data += (char)c; return 1; }
  String readStringUntil(char t) { std::string o; while (pos < data.size() && data[pos] != t) o += data[pos++]; if (pos < data.size()) pos++; return String(o); }
  void close() { if (ok && writing) simFiles[path] = data; ok = false; }
};
struct LittleFSStub {
  bool begin(bool = false) { return true; }
  bool exists(const char* p) { return simFiles.count(p) > 0; }
  bool remove(const char* p) { return simFiles.erase(p) > 0; }
  File open(const char* p, const char* mode = "r") { return File(p, mode); }
};
extern LittleFSStub LittleFS;

// ---- WiFi / network ----
#define WL_CONNECTED 3
#define WIFI_STA 1
extern bool simWifiUp;
struct WiFiStub {
  int status() { return simWifiUp ? WL_CONNECTED : 6; }
  IPAddress localIP() { return IPAddress(192, 168, 1, 42); }
  IPAddress subnetMask() { return IPAddress(255, 255, 255, 0); }
  void mode(int) {} void setHostname(const char*) {}
  void begin(const char*, const char*) {}
  int scanNetworks() { return 3; }
  String SSID(int i) { const char* n[] = {"HomeWiFi", "School-Guest", "Hotspot"}; return String(n[i % 3]); }
  String SSID() { return String("HomeWiFi"); }
  int RSSI(int = 0) { return -60; }
};
extern WiFiStub WiFi;
struct WiFiClient {
  IPAddress remoteIP() { return IPAddress(192, 168, 1, 77); }
  int available() { return 0; } int read() { return -1; }
  int readBytes(uint8_t*, size_t) { return 0; } int read(uint8_t*, size_t) { return 0; }
  bool connected() { return false; }
};
struct WiFiClientSecure : WiFiClient { void setInsecure() {} };
struct WiFiUDP : Print {
  bool begin(int) { return true; }
  int parsePacket() { return 0; } int read(char*, size_t) { return 0; }
  IPAddress remoteIP() { return IPAddress(); } int remotePort() { return 0; }
  void beginPacket(IPAddress, int) {} void endPacket() {}
  size_t write(uint8_t) override { return 1; }
};
#define HTTPC_STRICT_FOLLOW_REDIRECTS 1
struct HTTPClient {
  void setTimeout(int) {} void setFollowRedirects(int) {} void useHTTP10(bool) {}
  template <class C> bool begin(C&, const String&) { return true; }
  int GET() { return -1; } void end() {}
  String getString() { return String(""); } int getSize() { return 0; }
  bool connected() { return false; } WiFiClient* getStreamPtr() { static WiFiClient c; return &c; }
};
struct UpdateStub {
  bool begin(size_t) { return false; } size_t write(uint8_t*, size_t) { return 0; }
  bool end(bool = true) { return false; } bool isFinished() { return false; } void abort() {}
};
extern UpdateStub Update;
class WebServer {
public:
  std::map<std::string, std::function<void()>> routes;
  std::map<std::string, std::string> args;
  WebServer(int) {}
  void on(const char* p, std::function<void()> f) { routes[p] = f; }
  void begin() {} void handleClient() {}
  bool hasArg(const char* a) { return args.count(a) > 0; }
  String arg(const char* a) { return String(args[a]); }
  void send(int, const char* = "", const String& = String()) {}
  void sendHeader(const char*, const char*) {}
  WiFiClient client() { return WiFiClient(); }
};
inline void sntp_set_time_sync_notification_cb(void (*)(struct timeval*)) {}
