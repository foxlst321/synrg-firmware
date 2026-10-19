// ============================================================
// PDA — Full Build: Display + Touch + RTC + Internal Storage(LittleFS) +
// Schedule + Screens + Settings(Time+Date+WiFi with multi-network
// priority) + Keyboard + To-Do(delete) + Homework(delete, Unscheduled
// bucket fixed) + Notes(save/load, noise-filtered) + Weather(live) +
// More(registry) + Slow icon animations + periodic display re-init +
// built-in web page (view + add + DELETE To-Do/Homework from browser) +
// "Nook Phone"-inspired reskin
// ============================================================

#include <SPI.h>
#include <Wire.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <time.h>
#include <esp_sntp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include "RTClib.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

// ============================================================
// HARDWARE PINS
// ============================================================
#define TFT_CS   D0
#define TFT_RST  D1
#define TFT_DC   D7
#define T_CS     D3
#define RTC_SDA  D4
#define RTC_SCL  D5
#define TFT_SCK  D8
#define TFT_MISO D9
#define TFT_MOSI D10
// D6 free (SD no longer used). D2 free but a boot strapping pin --
// reserved for the battery voltage sensor divider once built.

// Settings actions -- declared up here so the Arduino IDE's auto-generated
// function prototypes can see the type.
enum Screen { SCREEN_WELCOME, SCREEN_TOUCH_TEST, SCREEN_LOCK, SCREEN_HOME, SCREEN_APP, SCREEN_SETTINGS, SCREEN_SET_TIME, SCREEN_SET_DATE, SCREEN_TODO, SCREEN_HOMEWORK, SCREEN_KEYBOARD, SCREEN_NOTES, SCREEN_WIFI, SCREEN_WIFI_SAVED, SCREEN_WEATHER, SCREEN_MORE_CONFIRM, SCREEN_MORE_ABOUT, SCREEN_SNAKE, SCREEN_HW_PERIOD, SCREEN_OTA, SCREEN_THEMES, SCREEN_THEME_CUSTOM, SCREEN_TOUCH_CAL };
enum SnakeDir { SNAKE_UP, SNAKE_DOWN, SNAKE_LEFT, SNAKE_RIGHT };
enum MoreAction { MORE_ACTION_SET_TIME, MORE_ACTION_SET_DATE, MORE_ACTION_WIFI, MORE_ACTION_ABOUT, MORE_ACTION_UPDATE, MORE_ACTION_CLEAR_TODO, MORE_ACTION_CLEAR_HOMEWORK, MORE_ACTION_CLEAR_NOTES, MORE_ACTION_RUN_SETUP, MORE_ACTION_SNAKE, MORE_ACTION_ANIMATIONS, MORE_ACTION_WIDGET, MORE_ACTION_THEME, MORE_ACTION_DEVICE_NAME, MORE_ACTION_TOUCH_CAL };

// Theme palette types (see the THEMES block further down).
struct RGB { uint8_t r, g, b; };
struct Theme {
  const char* name;
  RGB bg, text, subtext, accent, statusbar, error, frame, shadow, surface;
  RGB todo, notes, homework, weather, settings, more;
};

#define FIRMWARE_VERSION "v2.6"

// ---- Over-the-air update: where the PDA looks for new firmware. These two
// files live in the public GitHub repo. version.txt holds just the version
// number (for example v1.8), firmware.bin is the compiled sketch. ----
#define OTA_VERSION_URL  "https://raw.githubusercontent.com/foxlst321/synrg-firmware/main/version.txt"
#define OTA_FIRMWARE_URL "https://raw.githubusercontent.com/foxlst321/synrg-firmware/main/firmware.bin"
#define MAX_SAVED_NETWORKS 5

// If a replacement/spare display renders the picture upside-down or
// sideways, change ONLY this one number (try 0, 1, 2 or 3) and re-flash.
// Touch is calibrated separately (ts.setRotation below) and does not need
// to change when this does.
#define DISPLAY_ROTATION 2

// The rotation actually in effect. Starts at the default above, but once
// "Flip Screen" is used on the Touch Check screen, the chosen value is
// saved to /display.txt and reloaded on every future power-up -- so a
// flip sticks permanently, no re-flashing needed.
int displayRotation = DISPLAY_ROTATION;

int loadDisplayRotation() {
  if (!LittleFS.exists("/display.txt")) return DISPLAY_ROTATION;
  File f = LittleFS.open("/display.txt");
  if (!f) return DISPLAY_ROTATION;
  char buf[4];
  int i = 0;
  while (f.available() && i < 3) buf[i++] = f.read();
  buf[i] = '\0';
  f.close();
  int r = atoi(buf);
  if (r < 0 || r > 3) r = DISPLAY_ROTATION;
  return r;
}

void saveDisplayRotation(int r) {
  LittleFS.remove("/display.txt");
  File f = LittleFS.open("/display.txt", FILE_WRITE);
  if (f) { f.print(r); f.close(); }
}

// ---- Touch calibration (Settings > System > Calibrate Touch, the web
// page, or hold the screen for 6 seconds). Three taps tell us which raw
// touch axis runs which way -- normal, mirrored, or swapped -- so a panel
// that reads in the wrong direction still lines up with the picture.
// Saved to /touch.txt as "swap,x0,x1,y0,y1,rotation"; without that file the
// original fixed mapping is used. ----
bool touchCalValid = false;
bool touchCalSwap = false;      // raw X/Y swapped relative to the screen
int touchCalX0, touchCalX1;     // raw readings at screen x = CAL_X0 / CAL_X1
int touchCalY0, touchCalY1;     // raw readings at screen y = CAL_Y0 / CAL_Y1
int touchCalRotation = DISPLAY_ROTATION;
const int CAL_X0 = 24, CAL_X1 = 216, CAL_Y0 = 40, CAL_Y1 = 296;

void loadTouchCal() {
  touchCalValid = false;
  if (!LittleFS.exists("/touch.txt")) return;
  File f = LittleFS.open("/touch.txt");
  if (!f) return;
  char buf[48];
  int i = 0;
  while (f.available() && i < 47) buf[i++] = f.read();
  buf[i] = '\0';
  f.close();
  int sw, x0, x1, y0, y1, rot;
  if (sscanf(buf, "%d,%d,%d,%d,%d,%d", &sw, &x0, &x1, &y0, &y1, &rot) != 6) return;
  if (abs(x1 - x0) < 300 || abs(y1 - y0) < 300) return;
  touchCalSwap = (sw == 1);
  touchCalX0 = x0; touchCalX1 = x1; touchCalY0 = y0; touchCalY1 = y1;
  touchCalRotation = rot;
  touchCalValid = true;
}

void saveTouchCal() {
  LittleFS.remove("/touch.txt");
  File f = LittleFS.open("/touch.txt", FILE_WRITE);
  if (f) {
    f.printf("%d,%d,%d,%d,%d,%d", touchCalSwap ? 1 : 0, touchCalX0, touchCalX1, touchCalY0, touchCalY1, touchCalRotation);
    f.close();
  }
}

// Home screen icon "breathing" animation on/off. Toggled from Settings and
// saved to /anim.txt so the choice survives a power cycle. Defaults to on.
bool homeAnimationsEnabled = true;

bool loadHomeAnimations() {
  if (!LittleFS.exists("/anim.txt")) return true;
  File f = LittleFS.open("/anim.txt");
  if (!f) return true;
  char c = f.available() ? f.read() : '1';
  f.close();
  return c != '0';
}

void saveHomeAnimations(bool on) {
  LittleFS.remove("/anim.txt");
  File f = LittleFS.open("/anim.txt", FILE_WRITE);
  if (f) { f.print(on ? 1 : 0); f.close(); }
}

// Which widget the top-left Home tile shows. Cycled from Settings and saved
// to /widget.txt so it survives a power cycle. Defaults to the clock.
enum HomeWidget { WIDGET_CLASS, WIDGET_NEXT_CLASS, WIDGET_CLOCK, WIDGET_WEATHER, WIDGET_TODO, WIDGET_COUNT };
const char* widgetNames[WIDGET_COUNT] = {"Class", "Next Class", "Clock", "Weather", "To-Do"};
int homeWidget = WIDGET_CLOCK;

int loadHomeWidget() {
  if (!LittleFS.exists("/widget.txt")) return WIDGET_CLOCK;
  File f = LittleFS.open("/widget.txt");
  if (!f) return WIDGET_CLOCK;
  char buf[4];
  int i = 0;
  while (f.available() && i < 3) buf[i++] = f.read();
  buf[i] = '\0';
  f.close();
  int w = atoi(buf);
  if (w < 0 || w >= WIDGET_COUNT) w = WIDGET_CLOCK;
  return w;
}

void saveHomeWidget(int w) {
  LittleFS.remove("/widget.txt");
  File f = LittleFS.open("/widget.txt", FILE_WRITE);
  if (f) { f.print(w); f.close(); }
}

// ---- Themes: every screen draws with the COLOR_* globals, so a theme just
// swaps that whole palette at once. Six presets plus a Custom theme built
// from a Light/Dark base and an accent color. Saved to /theme.txt as
// "theme,customBase,customAccent" so it survives a power cycle. ----
const Theme themes[] = {
  {"Nook",     {250,240,222}, {90,62,40},    {150,120,90},  {110,175,100}, {200,150,100}, {220,100,90}, {220,195,160}, {215,190,165}, {255,255,255},
               {140,200,220}, {190,160,210}, {140,200,140}, {240,180,120}, {190,170,150}, {220,160,170}},
  {"Midnight", {24,26,38},    {230,230,240}, {150,155,175}, {110,180,255}, {45,50,80},    {230,95,95},  {55,60,85},    {8,8,14},      {44,48,66},
               {70,140,200},  {140,100,190}, {70,160,110},  {210,140,70},  {100,105,130}, {190,90,130}},
  {"Ocean",    {225,240,248}, {20,60,90},    {80,120,150},  {0,150,190},   {40,110,160},  {220,90,90},  {170,205,225}, {185,212,230}, {255,255,255},
               {80,170,220},  {120,140,220}, {60,180,170},  {250,180,90},  {120,160,190}, {240,140,160}},
  {"Forest",   {232,240,225}, {40,65,40},    {100,125,95},  {80,150,70},   {90,130,80},   {200,90,70},  {190,210,175}, {200,215,190}, {255,255,255},
               {110,170,150}, {160,150,200}, {120,180,100}, {220,170,90},  {150,165,130}, {210,150,140}},
  {"Sunset",   {255,236,225}, {90,40,50},    {160,100,100}, {240,120,80},  {220,100,110}, {200,60,70},  {245,200,190}, {240,205,195}, {255,255,255},
               {250,160,120}, {220,130,170}, {240,190,100}, {250,140,90},  {200,150,160}, {180,120,200}},
  {"Mono",     {240,240,240}, {30,30,30},    {110,110,110}, {60,60,60},    {70,70,70},    {200,60,60},  {200,200,200}, {205,205,205}, {255,255,255},
               {110,110,110}, {130,130,130}, {90,90,90},    {150,150,150}, {120,120,120}, {100,100,100}},
};
const int presetThemeCount = sizeof(themes) / sizeof(themes[0]);
const int THEME_CUSTOM = presetThemeCount;   // index just past the presets
const int themeCount = presetThemeCount + 1;

const char* customBaseNames[2] = {"Light", "Dark"};
const RGB customAccents[8] = {
  {110,175,100}, {90,150,220}, {0,170,170}, {150,110,210},
  {230,110,160}, {230,90,80},  {240,150,60}, {200,170,40},
};

int currentTheme = 0;
int customBase = 0;     // 0 = Light (Nook-based), 1 = Dark (Midnight-based)
int customAccent = 0;   // index into customAccents

void loadThemeSettings() {
  if (!LittleFS.exists("/theme.txt")) return;
  File f = LittleFS.open("/theme.txt");
  if (!f) return;
  char buf[16];
  int i = 0;
  while (f.available() && i < 15) buf[i++] = f.read();
  buf[i] = '\0';
  f.close();
  int t = 0, b = 0, a = 0;
  sscanf(buf, "%d,%d,%d", &t, &b, &a);
  currentTheme = (t >= 0 && t < themeCount) ? t : 0;
  customBase = (b == 1) ? 1 : 0;
  customAccent = (a >= 0 && a < 8) ? a : 0;
}

// ---- Device name: chosen during first-time setup (or Settings > System >
// Device Name). Shown on the web page and used as the WiFi hostname.
// Saved to /name.txt. ----
#define DEVICE_NAME_MAX 20
char deviceName[DEVICE_NAME_MAX + 1] = "SynRG";

void loadDeviceName() {
  if (!LittleFS.exists("/name.txt")) return;
  File f = LittleFS.open("/name.txt");
  if (!f) return;
  char buf[DEVICE_NAME_MAX + 1];
  int i = 0;
  while (f.available() && i < DEVICE_NAME_MAX) {
    char c = f.read();
    if (c == '\n' || c == '\r') break;
    buf[i++] = c;
  }
  buf[i] = '\0';
  f.close();
  if (i > 0) strcpy(deviceName, buf);
}

void saveDeviceName() {
  LittleFS.remove("/name.txt");
  File f = LittleFS.open("/name.txt", FILE_WRITE);
  if (f) { f.print(deviceName); f.close(); }
}

// WiFi hostnames may only use letters, digits and dashes.
void buildHostname(char* out, size_t len) {
  size_t j = 0;
  for (size_t i = 0; deviceName[i] && j < len - 1; i++) {
    char c = deviceName[i];
    if (isalnum((unsigned char)c)) out[j++] = tolower((unsigned char)c);
    else if ((c == ' ' || c == '-' || c == '_') && j > 0 && out[j - 1] != '-') out[j++] = '-';
  }
  while (j > 0 && out[j - 1] == '-') j--;
  out[j] = '\0';
  if (j == 0) snprintf(out, len, "synrg");
}

void saveThemeSettings() {
  LittleFS.remove("/theme.txt");
  File f = LittleFS.open("/theme.txt", FILE_WRITE);
  if (f) { f.printf("%d,%d,%d", currentTheme, customBase, customAccent); f.close(); }
}

Adafruit_ILI9341 tft = Adafruit_ILI9341(&SPI, TFT_DC, TFT_CS, TFT_RST);
XPT2046_Touchscreen ts(T_CS);


RTC_PCF8563 rtc;
WebServer server(80);

// ============================================================
// SCHEDULE CONFIG — EDIT ONLY THIS BLOCK FOR A NEW SCHOOL YEAR
// ============================================================
struct Period {
  const char* name;
  int startHour, startMin;
  int endHour, endMin;
};

Period schedule[] = {
  {"1st Hour",     7, 30,  8, 20},
  {"2nd Hour",     8, 20,  9, 10},
  {"3rd Hour",     9, 10, 10,  0},
  {"4th Hour",    10,  0, 10, 50},
  {"Break",       10, 50, 10, 55},
  {"Tiger Time",  10, 55, 11, 25},
  {"Lunch",       11, 25, 11, 55},
  {"5th Hour",    11, 55, 12, 45},
  {"6th Hour",    12, 45, 13, 35},
  {"7th Hour",    13, 35, 14, 30},
};
const int numPeriods = sizeof(schedule) / sizeof(schedule[0]);
// ============================================================
// END SCHEDULE CONFIG
// ============================================================

// ============================================================
// WEATHER CONFIG — Gobles, Michigan
// ============================================================
#define WEATHER_LAT "42.35"
#define WEATHER_LON "-85.88"
#define WEATHER_LOCATION_LABEL "Gobles, MI"

const char* dayNames[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

uint16_t COLOR_BG, COLOR_TEXT, COLOR_SUBTEXT, COLOR_ACCENT, COLOR_STATUSBAR, COLOR_ERROR;
uint16_t COLOR_TODO, COLOR_NOTES, COLOR_HOMEWORK, COLOR_WEATHER, COLOR_SETTINGS, COLOR_MORE;
uint16_t COLOR_FRAME, COLOR_SHADOW, COLOR_BLUE, COLOR_GRAY, COLOR_SURFACE;
uint16_t tileColors[6];

uint16_t rgb565(RGB c) { return tft.color565(c.r, c.g, c.b); }
RGB darkenRGB(RGB c, int amt) {
  RGB o = { (uint8_t)(c.r > amt ? c.r - amt : 0), (uint8_t)(c.g > amt ? c.g - amt : 0), (uint8_t)(c.b > amt ? c.b - amt : 0) };
  return o;
}

// Loads a theme into the global palette. Custom = the Light (Nook) or Dark
// (Midnight) base with the chosen accent, and a darker shade of that accent
// for the status bar.
Theme themeFor(int idx) {
  Theme t = themes[0];
  if (idx == THEME_CUSTOM) {
    t = themes[customBase == 1 ? 1 : 0];
    RGB a = customAccents[customAccent];
    t.accent = a;
    t.statusbar = darkenRGB(a, 40);
  } else if (idx >= 0 && idx < presetThemeCount) {
    t = themes[idx];
  }
  return t;
}

void applyTheme(int idx) {
  Theme t = themeFor(idx);
  COLOR_BG        = rgb565(t.bg);
  COLOR_TEXT      = rgb565(t.text);
  COLOR_SUBTEXT   = rgb565(t.subtext);
  COLOR_ACCENT    = rgb565(t.accent);
  COLOR_STATUSBAR = rgb565(t.statusbar);
  COLOR_ERROR     = rgb565(t.error);
  COLOR_FRAME     = rgb565(t.frame);
  COLOR_SHADOW    = rgb565(t.shadow);
  COLOR_SURFACE   = rgb565(t.surface);
  COLOR_TODO      = rgb565(t.todo);
  COLOR_NOTES     = rgb565(t.notes);
  COLOR_HOMEWORK  = rgb565(t.homework);
  COLOR_WEATHER   = rgb565(t.weather);
  COLOR_SETTINGS  = rgb565(t.settings);
  COLOR_MORE      = rgb565(t.more);
  COLOR_BLUE      = tft.color565(90, 150, 220);
  COLOR_GRAY      = tft.color565(170, 170, 170);

  tileColors[0] = COLOR_MORE;      // widget tile background
  tileColors[1] = COLOR_TODO;
  tileColors[2] = COLOR_NOTES;
  tileColors[3] = COLOR_HOMEWORK;
  tileColors[4] = COLOR_WEATHER;
  tileColors[5] = COLOR_SETTINGS;
}

Screen currentScreen = SCREEN_LOCK;
bool firstBootFlow = false;   // true while walking through welcome -> touch test -> set time -> set date
int touchTestResult = 0;      // 0 = waiting for tap, 1 = pass, 2 = fail (mirrored)
const int TT_TARGET_X = 205;
const int TT_TARGET_Y = 50;
const int TT_TARGET_R = 16;
int currentAppIndex = -1;
int lastDrawnMinute = -1;

// ---- Automatic time sync: whenever WiFi is up, the ESP32 asks an internet
// time server for the time (roughly once an hour) and copies it into the
// RTC, so the clock never drifts. TIME_ZONE is US Eastern with automatic
// daylight saving; change it here if the PDA ever moves time zones. ----
#define TIME_ZONE "EST5EDT,M3.2.0,M11.1.0"
volatile bool ntpFreshSync = false;   // set by the SNTP task, read in loop()
bool rtcEverSynced = false;
DateTime lastRtcSync;

void onNtpSync(struct timeval* tv) { ntpFreshSync = true; }

void startTimeSync() {
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TIME_ZONE, "pool.ntp.org", "time.nist.gov", "time.google.com");
}

// Called from loop(): when a fresh internet time has arrived, write it into
// the RTC and redraw the clock on Home/Lock.
void applyNtpToRtc() {
  if (!ntpFreshSync) return;
  ntpFreshSync = false;
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  if (t.tm_year + 1900 < 2024) return;   // not a real time yet
  DateTime synced(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  rtc.adjust(synced);
  rtcEverSynced = true;
  lastRtcSync = synced;
  lastDrawnMinute = -1;   // force Home/Lock to redraw the corrected time
  Serial.println("RTC synced from internet time.");
}
bool wasTouched = false;
unsigned long touchHoldStartMs = 0;
int editHour = 0, editMinute = 0;
int editYear = 2026, editMonth = 1, editDay = 1;
unsigned long lastAnimMs = 0;
unsigned long lastReinitMs = 0;
const unsigned long REINIT_INTERVAL_MS = 5UL * 60UL * 1000UL;

// ---- Startup status, used by About screen ----
bool storageOK = false;
bool rtcOK = false;

// ---- Onscreen keyboard state (shared widget) ----
enum KeyboardContext { KB_CONTEXT_NONE, KB_CONTEXT_ADD_TODO, KB_CONTEXT_ADD_HOMEWORK, KB_CONTEXT_WIFI_PASSWORD, KB_CONTEXT_DEVICE_NAME };
KeyboardContext kbContext = KB_CONTEXT_NONE;
// Three pages: letters, numbers + common symbols, and fancy symbols.
enum KeyboardMode { KB_MODE_ABC, KB_MODE_123, KB_MODE_SYM };
KeyboardMode kbMode = KB_MODE_ABC;
bool kbShift = false;
char kbBuffer[64] = "";
int kbBufferLen = 0;

const char* kbRow1_abc = "qwertyuiop";
const char* kbRow2_abc = "asdfghjkl";
const char* kbRow3_abc = "zxcvbnm";
// Page 2: numbers and the symbols people type most.
const char* kbRow1_123 = "1234567890";
const char* kbRow2_123 = "-/:;()$&@\"";
const char* kbRow3_123 = ".,?!'#%*";
// Page 3: fancy symbols.
const char* kbRow1_sym = "[]{}<>^~|\\";
const char* kbRow2_sym = "_=+`";
const char* kbRow3_sym = "";

const char* kbGetRow1() {
  if (kbMode == KB_MODE_123) return kbRow1_123;
  if (kbMode == KB_MODE_SYM) return kbRow1_sym;
  return kbRow1_abc;
}
const char* kbGetRow2() {
  if (kbMode == KB_MODE_123) return kbRow2_123;
  if (kbMode == KB_MODE_SYM) return kbRow2_sym;
  return kbRow2_abc;
}
const char* kbGetRow3() {
  if (kbMode == KB_MODE_123) return kbRow3_123;
  if (kbMode == KB_MODE_SYM) return kbRow3_sym;
  return kbRow3_abc;
}

// ---- Delete tracking for To-Do and Homework lists ----
char todoLines[8][40];
int todoLineY[8];
int todoLineCount = 0;

char hwLines[8][60];
int hwLineY[8];
int hwLineCount = 0;

// ---- Notes drawing canvas state ----
int notesPrevX = -1, notesPrevY = -1;
int noteX[500], noteY[500];
bool noteNewStroke[500];
int notePointCount = 0;

// ---- WiFi state ----
enum WifiConnState { WIFI_CONN_IDLE, WIFI_CONN_CONNECTING, WIFI_CONN_CONNECTED, WIFI_CONN_FAILED };
WifiConnState wifiConnState = WIFI_CONN_IDLE;
char wifiSSIDs[10][33];
int wifiFoundCount = 0;
char wifiSelectedSSID[33] = "";
char wifiStatusMsg[64] = "";
unsigned long wifiConnectStartMs = 0;
unsigned long lastWifiCheckMs = 0;

// ---- Saved WiFi networks (priority list) ----
char savedNetSsidsCache[MAX_SAVED_NETWORKS][33];
char savedNetPassesCache[MAX_SAVED_NETWORKS][64];
int savedNetCount = 0;

// ---- Weather state ----
char weatherTemp[10] = "--";
char weatherDesc[24] = "Not fetched yet";
bool weatherFetching = false;

// ---- Firmware update (OTA) state ----
enum OtaState { OTA_IDLE, OTA_CHECKING, OTA_UP_TO_DATE, OTA_AVAILABLE, OTA_DOWNLOADING, OTA_FAILED, OTA_NO_WIFI };
OtaState otaState = OTA_IDLE;
char otaLatest[20] = "";
char otaMsg[40] = "";

// ---- Snake game direction type (declared up here, not down in the Snake
// section, because the Arduino IDE auto-generates function prototypes near
// the top of the file, and a prototype using this type would otherwise
// reference it before it exists) ----

// ---- Settings list: everything that used to live on its own "More" tile
// is now folded into the Settings screen as a scrollable/paged registry,
// alongside Set Time / Set Date / WiFi. This freed up a Home tile slot. ----
struct MoreItem {
  const char* name;
  MoreAction action;
};
MoreItem moreItems[] = {
  {"Set Time",             MORE_ACTION_SET_TIME},
  {"Set Date",             MORE_ACTION_SET_DATE},
  {"WiFi",                 MORE_ACTION_WIFI},
  {"Animations",           MORE_ACTION_ANIMATIONS},
  {"Home Widget",          MORE_ACTION_WIDGET},
  {"Theme",                MORE_ACTION_THEME},
  {"Device Name",          MORE_ACTION_DEVICE_NAME},
  {"Calibrate Touch",      MORE_ACTION_TOUCH_CAL},
  {"About",                MORE_ACTION_ABOUT},
  {"Update Firmware",      MORE_ACTION_UPDATE},
  {"Clear To-Do List",     MORE_ACTION_CLEAR_TODO},
  {"Clear Homework Log",   MORE_ACTION_CLEAR_HOMEWORK},
  {"Clear Notes",          MORE_ACTION_CLEAR_NOTES},
  {"Run First-Time Setup", MORE_ACTION_RUN_SETUP},
  {"Snake Game",           MORE_ACTION_SNAKE},
};
const int moreItemCount = sizeof(moreItems) / sizeof(moreItems[0]);
MoreAction pendingConfirmAction;
// Settings is a two-level menu: a list of categories, each opening a short
// list of the actions above. Every original button lives in exactly one
// category. settingsCategory == -1 means the top-level category list.
struct SettingsCategory {
  const char* name;
  MoreAction items[4];
  int count;
};
SettingsCategory settingsCategories[] = {
  {"Appearance",     {MORE_ACTION_THEME, MORE_ACTION_ANIMATIONS, MORE_ACTION_WIDGET}, 3},
  {"Time & Date",    {MORE_ACTION_SET_TIME, MORE_ACTION_SET_DATE}, 2},
  {"WiFi & Updates", {MORE_ACTION_WIFI, MORE_ACTION_UPDATE}, 2},
  {"Storage",        {MORE_ACTION_CLEAR_TODO, MORE_ACTION_CLEAR_HOMEWORK, MORE_ACTION_CLEAR_NOTES}, 3},
  {"System",         {MORE_ACTION_ABOUT, MORE_ACTION_DEVICE_NAME, MORE_ACTION_TOUCH_CAL, MORE_ACTION_RUN_SETUP}, 4},
  {"Games",          {MORE_ACTION_SNAKE}, 1},
};
const int settingsCategoryCount = sizeof(settingsCategories) / sizeof(settingsCategories[0]);
int settingsCategory = -1;

// ---- Home screen tile layout: 2 columns x 3 rows (portrait). Slot 0
// (top-left) is a live status widget, not a launcher -- slots 1-5 are
// the actual app shortcuts. ----
const int tileW = 104, tileH = 84;
const int col0 = 14, col1 = 122;
const int row0 = 34, row1 = 122, row2 = 210;
const char* tileNames[6] = {"", "To-Do", "Notes", "Homework", "Weather", "Settings"};
int tileX[6] = {col0, col1, col0, col1, col0, col1};
int tileY[6] = {row0, row0, row1, row1, row2, row2};

// ---- Homework period picker: which class period to file a new homework
// item under. Defaults to a guess, but is always adjustable -- including
// during the gap/buffer between classes when the guess is ambiguous. ----
char pendingHomeworkText[40] = "";
int hwPeriodPickerIndex = -1;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void drawWelcomeScreen();
void handleWelcomeTouch(int x, int y);
void drawTouchTestScreen();
void handleTouchTestTouch(int x, int y);
void drawGradientBG(uint16_t topColor, uint16_t bottomColor);
void drawSynRGLogo(int cx, int cy, float scale, uint16_t ringColor, uint16_t textColor);
void toggleTodoLine(const char* oldLine);
int touchedTodoCheckbox(int x, int y);
int loadDisplayRotation();
void saveDisplayRotation(int r);
void drawSnakeScreen();
void handleSnakeTouch(int x, int y);
void handleSnakeSwipe(int x, int y, bool isNewTouch);
void snakeInitGame();
void updateSnakeGame();
void snakePlaceFood();
void drawSnakeScore();
void drawSnakeField();
void drawSnakeGameOverOverlay();
bool touchedSnakeField(int x, int y);
void snakeApplyDirection(SnakeDir d);

void drawLockScreen(DateTime now);
void drawWidgetTile();
void drawHomeScreen(DateTime now);
void drawTodoScreen();
void drawHomeworkScreen();
int guessHomeworkPeriodIndex(int hour, int minute);
void drawHwPeriodScreen();
void handleHwPeriodTouch(int x, int y);
void drawSettingsScreen();
void handleSettingsTouch(int x, int y);
void drawSetTimeScreen();
void drawSetDateScreen();
void drawKeyboardScreen();
void drawNotesScreen();
void drawNotesSaveButton(bool pressed);
void drawWifiScreen();
void drawWifiSavedScreen();
void drawWeatherScreen();
void drawMoreConfirmScreen();
void drawMoreAboutScreen();
void drawOtaScreen();
void handleOtaTouch(int x, int y);
void checkOtaVersion();
void performOtaUpdate();
void drawOtaProgress(int pct);
void performMoreAction(MoreAction action);
const char* moreActionLabel(MoreAction a);
void handleWebRoot();
void handleWebAddTodo();
void handleWebAddHomework();
void handleWebDeleteTodo();
void handleWebDeleteHomework();

// ============================================================
// FILE HELPERS
// ============================================================
bool readLineFromFile(File &f, char* buf, int bufSize) {
  int i = 0;
  bool gotAny = false;
  while (f.available()) {
    char c = f.read();
    gotAny = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (i < bufSize - 1) buf[i++] = c;
  }
  buf[i] = '\0';
  return gotAny;
}

void deleteLineFromFile(const char* path, const char* targetLine) {
  char buf[60];
  bool deleted = false;

  LittleFS.remove("/tmp_del.txt");
  File src = LittleFS.open(path);
  File dst = LittleFS.open("/tmp_del.txt", FILE_WRITE);
  if (src && dst) {
    while (readLineFromFile(src, buf, sizeof(buf))) {
      if (!deleted && strcmp(buf, targetLine) == 0) {
        deleted = true;
        continue;
      }
      if (strlen(buf) > 0) dst.println(buf);
    }
  }
  if (src) src.close();
  if (dst) dst.close();

  LittleFS.remove(path);
  File tmp = LittleFS.open("/tmp_del.txt");
  File out = LittleFS.open(path, FILE_WRITE);
  if (tmp && out) {
    while (readLineFromFile(tmp, buf, sizeof(buf))) {
      if (strlen(buf) > 0) out.println(buf);
    }
  }
  if (tmp) tmp.close();
  if (out) out.close();
  LittleFS.remove("/tmp_del.txt");
}

int loadSavedNetworks(char ssids[][33], char passes[][64], int maxCount) {
  int count = 0;
  if (!LittleFS.exists("/wifinets.txt")) return 0;
  File f = LittleFS.open("/wifinets.txt");
  if (!f) return 0;
  char line[100];
  while (readLineFromFile(f, line, sizeof(line)) && count < maxCount) {
    if (strlen(line) == 0) continue;
    char* sep = strchr(line, '|');
    if (!sep) continue;
    *sep = '\0';
    strncpy(ssids[count], line, 32);
    ssids[count][32] = '\0';
    strncpy(passes[count], sep + 1, 63);
    passes[count][63] = '\0';
    count++;
  }
  f.close();
  return count;
}

void saveSavedNetworksOrdered(char ssids[][33], char passes[][64], int count) {
  LittleFS.remove("/wifinets.txt");
  File f = LittleFS.open("/wifinets.txt", FILE_WRITE);
  if (f) {
    for (int i = 0; i < count; i++) {
      f.print(ssids[i]);
      f.print("|");
      f.println(passes[i]);
    }
    f.close();
  }
}

void addOrUpdateNetworkAtTop(const char* ssid, const char* password) {
  char ssids[MAX_SAVED_NETWORKS][33];
  char passes[MAX_SAVED_NETWORKS][64];
  int count = loadSavedNetworks(ssids, passes, MAX_SAVED_NETWORKS);

  char newSsids[MAX_SAVED_NETWORKS][33];
  char newPasses[MAX_SAVED_NETWORKS][64];
  int newCount = 0;

  strncpy(newSsids[0], ssid, 32); newSsids[0][32] = '\0';
  strncpy(newPasses[0], password, 63); newPasses[0][63] = '\0';
  newCount = 1;

  for (int i = 0; i < count && newCount < MAX_SAVED_NETWORKS; i++) {
    if (strcmp(ssids[i], ssid) == 0) continue;
    strncpy(newSsids[newCount], ssids[i], 32); newSsids[newCount][32] = '\0';
    strncpy(newPasses[newCount], passes[i], 63); newPasses[newCount][63] = '\0';
    newCount++;
  }

  saveSavedNetworksOrdered(newSsids, newPasses, newCount);
}

void promoteNetworkToTop(int idx) {
  char ssids[MAX_SAVED_NETWORKS][33];
  char passes[MAX_SAVED_NETWORKS][64];
  int count = loadSavedNetworks(ssids, passes, MAX_SAVED_NETWORKS);
  if (idx < 0 || idx >= count) return;

  char newSsids[MAX_SAVED_NETWORKS][33];
  char newPasses[MAX_SAVED_NETWORKS][64];
  int newCount = 0;

  strncpy(newSsids[0], ssids[idx], 32); newSsids[0][32] = '\0';
  strncpy(newPasses[0], passes[idx], 63); newPasses[0][63] = '\0';
  newCount = 1;

  for (int i = 0; i < count; i++) {
    if (i == idx) continue;
    strncpy(newSsids[newCount], ssids[i], 32); newSsids[newCount][32] = '\0';
    strncpy(newPasses[newCount], passes[i], 63); newPasses[newCount][63] = '\0';
    newCount++;
  }

  saveSavedNetworksOrdered(newSsids, newPasses, newCount);
}

void deleteSavedNetwork(int idx) {
  char ssids[MAX_SAVED_NETWORKS][33];
  char passes[MAX_SAVED_NETWORKS][64];
  int count = loadSavedNetworks(ssids, passes, MAX_SAVED_NETWORKS);
  if (idx < 0 || idx >= count) return;

  char newSsids[MAX_SAVED_NETWORKS][33];
  char newPasses[MAX_SAVED_NETWORKS][64];
  int newCount = 0;

  for (int i = 0; i < count; i++) {
    if (i == idx) continue;
    strncpy(newSsids[newCount], ssids[i], 32); newSsids[newCount][32] = '\0';
    strncpy(newPasses[newCount], passes[i], 63); newPasses[newCount][63] = '\0';
    newCount++;
  }

  saveSavedNetworksOrdered(newSsids, newPasses, newCount);
}

void connectToBestSavedNetwork() {
  char ssids[MAX_SAVED_NETWORKS][33];
  char passes[MAX_SAVED_NETWORKS][64];
  int count = loadSavedNetworks(ssids, passes, MAX_SAVED_NETWORKS);
  if (count == 0) return;

  int found = WiFi.scanNetworks();
  if (found <= 0) {
    Serial.println("No networks found in range.");
    return;
  }

  for (int i = 0; i < count; i++) {
    for (int j = 0; j < found; j++) {
      if (strcmp(ssids[i], WiFi.SSID(j).c_str()) == 0) {
        strncpy(wifiSelectedSSID, ssids[i], 32);
        wifiSelectedSSID[32] = '\0';
        WiFi.begin(ssids[i], passes[i]);
        wifiConnState = WIFI_CONN_CONNECTING;
        wifiConnectStartMs = millis();
        Serial.print("Auto-connecting to priority network: ");
        Serial.println(ssids[i]);
        return;
      }
    }
  }
  Serial.println("No saved networks currently in range.");
}

// ============================================================
// CALENDAR HELPERS
// ============================================================
bool isLeapYear(int y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int daysInMonth(int month, int year) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) return 29;
  return days[month - 1];
}

void clampEditDay() {
  int maxD = daysInMonth(editMonth, editYear);
  if (editDay > maxD) editDay = maxD;
}

// ============================================================
// SCHEDULE ENGINE
// ============================================================
int getCurrentPeriodIndex(int hour, int minute) {
  int nowMinutes = hour * 60 + minute;
  for (int i = 0; i < numPeriods; i++) {
    int startMinutes = schedule[i].startHour * 60 + schedule[i].startMin;
    int endMinutes = schedule[i].endHour * 60 + schedule[i].endMin;
    if (nowMinutes >= startMinutes && nowMinutes < endMinutes) return i;
  }
  return -1;
}

void formatTime12h(char* buf, int h, int m) {
  int displayHour = h % 12;
  if (displayHour == 0) displayHour = 12;
  sprintf(buf, "%d:%02d %s", displayHour, m, h >= 12 ? "PM" : "AM");
}

int hourTo12(int h24) { int h = h24 % 12; return h == 0 ? 12 : h; }
int hourTo24(int h12, bool pm) { int h = h12 % 12; if (pm) h += 12; return h; }

// ============================================================
// WEB SERVER — view/add/delete To-Do and Homework from any browser
// ============================================================
String cssColor(RGB c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
  return String(buf);
}

String htmlEscape(const char* in) {
  String out;
  for (; *in; in++) {
    switch (*in) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '\'': out += "&#39;"; break;
      case '"': out += "&quot;"; break;
      default: out += *in;
    }
  }
  return out;
}

void handleWebRoot() {
  DateTime now = rtc.now();
  char timeStr[9];
  formatTime12h(timeStr, now.hour(), now.minute());

  // The page follows the PDA's current theme, so it matches the screen.
  Theme t = themeFor(currentTheme);
  String bg = cssColor(t.bg), text = cssColor(t.text), sub = cssColor(t.subtext);
  String accent = cssColor(t.accent), bar = cssColor(t.statusbar), err = cssColor(t.error);
  String frame = cssColor(t.frame), surface = cssColor(t.surface);
  String name = htmlEscape(deviceName);

  String html = "<!DOCTYPE html><html><head><title>" + name + "</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta name='theme-color' content='" + bar + "'>";
  html += "<style>body{font-family:sans-serif;background:" + bg + ";color:" + text + ";max-width:500px;margin:0 auto;padding:0 16px 20px;}";
  html += "header{background:" + bar + ";color:#fff;margin:0 -16px 16px;padding:14px 16px;border-radius:0 0 14px 14px;}";
  html += "header h1{margin:0;font-size:1.5em;} header p{margin:4px 0 0;opacity:.85;}";
  html += "h2{color:" + accent + ";border-bottom:2px solid " + frame + ";padding-bottom:4px;}";
  html += "li{margin:6px 0;display:flex;justify-content:space-between;align-items:center;gap:8px;}";
  html += ".delform{display:inline;} .delbtn{background:" + err + ";color:white;border:none;padding:3px 8px;border-radius:6px;cursor:pointer;font-size:0.8em;}";
  html += "input[type=text]{padding:8px;width:70%;background:" + surface + ";color:" + text + ";border:1px solid " + sub + ";border-radius:8px;}";
  html += "input[type=submit]{padding:8px 16px;background:" + accent + ";color:#fff;border:none;border-radius:8px;cursor:pointer;}</style>";
  html += "</head><body>";
  html += "<header><h1>" + name + "</h1><p>" + String(timeStr) + "</p></header>";

  html += "<h2>To-Do</h2><ul>";
  char line[40];
  File f = LittleFS.open("/todo.txt");
  if (f) {
    while (readLineFromFile(f, line, sizeof(line))) {
      if (strlen(line) > 0) {
        html += "<li><span>" + String(line) + "</span>";
        html += "<form class='delform' action='/deletetodo' method='GET'>";
        html += "<input type='hidden' name='line' value='" + String(line) + "'>";
        html += "<button class='delbtn' type='submit'>Delete</button></form></li>";
      }
    }
    f.close();
  }
  html += "</ul>";
  html += "<form action='/addtodo' method='GET'><input type='text' name='text' placeholder='New task'>";
  html += "<input type='submit' value='Add Task'></form>";

  html += "<h2>Homework</h2><ul>";
  char hwLine[60];
  File f2 = LittleFS.open("/homework.txt");
  if (f2) {
    while (readLineFromFile(f2, hwLine, sizeof(hwLine))) {
      if (strlen(hwLine) > 0) {
        String rawLineStr = String(hwLine);
        char* sep = strchr(hwLine, '|');
        if (sep) {
          *sep = '\0';
          html += "<li><span><b>" + String(hwLine) + ":</b> " + String(sep + 1) + "</span>";
          html += "<form class='delform' action='/deletehomework' method='GET'>";
          html += "<input type='hidden' name='line' value='" + rawLineStr + "'>";
          html += "<button class='delbtn' type='submit'>Delete</button></form></li>";
        }
      }
    }
    f2.close();
  }
  html += "</ul>";
  html += "<form action='/addhomework' method='GET'><input type='text' name='text' placeholder='New homework item'>";
  html += "<input type='submit' value='Add Homework'></form>";

  html += "<h2>Device</h2><p>" + String(FIRMWARE_VERSION) + "</p>";
  html += "<form action='/calibrate' method='GET' style='display:inline'><input type='submit' value='Calibrate Touch'></form> ";
  html += "<form action='/update' method='GET' style='display:inline'><input type='submit' value='Check for Update'></form> ";
  html += "<form action='/restart' method='GET' style='display:inline' onsubmit=\"return confirm('Restart the PDA?')\"><input type='submit' value='Restart'></form>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleWebAddTodo() {
  if (server.hasArg("text") && server.arg("text").length() > 0) {
    File f = LittleFS.open("/todo.txt", FILE_APPEND);
    if (f) {
      f.print("[ ] ");
      f.println(server.arg("text"));
      f.close();
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleWebAddHomework() {
  if (server.hasArg("text") && server.arg("text").length() > 0) {
    DateTime now = rtc.now();
    int idx = guessHomeworkPeriodIndex(now.hour(), now.minute());
    const char* periodName = (idx == -1) ? "Unscheduled" : schedule[idx].name;
    File f = LittleFS.open("/homework.txt", FILE_APPEND);
    if (f) {
      f.print(periodName);
      f.print("|");
      f.println(server.arg("text"));
      f.close();
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleWebDeleteTodo() {
  if (server.hasArg("line") && server.arg("line").length() > 0) {
    deleteLineFromFile("/todo.txt", server.arg("line").c_str());
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleWebDeleteHomework() {
  if (server.hasArg("line") && server.arg("line").length() > 0) {
    deleteLineFromFile("/homework.txt", server.arg("line").c_str());
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// ---- Device controls on the web page, so the PDA can still be calibrated,
// updated or restarted when its touch isn't working. The heavy work runs in
// loop() after the browser has had its reply. ----
bool webCalRequested = false;
bool webOtaRequested = false;
unsigned long webRestartAtMs = 0;

void sendWebMessage(const char* msg) {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='8;url=/'><title>" + htmlEscape(deviceName) + "</title></head>";
  html += "<body style='font-family:sans-serif;max-width:500px;margin:40px auto;padding:0 16px;'>";
  html += "<p>" + String(msg) + "</p><p><a href='/'>Back</a></p></body></html>";
  server.send(200, "text/html", html);
}

void handleWebCalibrate() {
  webCalRequested = true;
  sendWebMessage("Touch calibration started -- tap the three targets on the PDA screen.");
}

void handleWebUpdate() {
  webOtaRequested = true;
  sendWebMessage("Checking for an update -- watch the PDA screen. If one is found it installs and restarts.");
}

void handleWebRestart() {
  webRestartAtMs = millis() + 500;
  sendWebMessage("Restarting the PDA...");
}

void runWebRequests() {
  if (webCalRequested) {
    webCalRequested = false;
    startTouchCalibration(SCREEN_HOME);
  }
  if (webOtaRequested) {
    webOtaRequested = false;
    otaState = OTA_IDLE;
    otaLatest[0] = '\0';
    otaMsg[0] = '\0';
    currentScreen = SCREEN_OTA;
    checkOtaVersion();
    if (otaState == OTA_AVAILABLE) performOtaUpdate();
  }
  if (webRestartAtMs && millis() > webRestartAtMs) ESP.restart();
}

// ============================================================
// TEXT HELPERS
// ============================================================
void printCentered(const GFXfont* font, int centerX, int topY, const char* text, uint16_t color) {
  tft.setFont(font);
  tft.setTextSize(1);
  tft.setTextColor(color);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(centerX - (int)(tw / 2) - x1, topY - y1);
  tft.print(text);
  tft.setFont();
}

void printFitted(const GFXfont* font, int centerX, int topY, const char* text, uint16_t color, int maxWidth) {
  tft.setFont(font);
  tft.setTextSize(1);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);

  if ((int)tw <= maxWidth) {
    tft.setTextColor(color);
    tft.setCursor(centerX - (int)(tw / 2) - x1, topY - y1);
    tft.print(text);
    tft.setFont();
    return;
  }

  tft.setFont();
  tft.setTextSize(1);
  int16_t cx1, cy1; uint16_t ctw, cth;
  tft.getTextBounds(text, 0, 0, &cx1, &cy1, &ctw, &cth);
  tft.setTextColor(color);
  tft.setCursor(centerX - (int)(ctw / 2), topY + 6);
  tft.print(text);
}

void drawTempWithDegree(const char* tempNum, int centerX, int topY, uint16_t color) {
  tft.setFont(&FreeSansBold24pt7b);
  tft.setTextSize(1);

  int16_t nx1, ny1; uint16_t numW, numH;
  tft.getTextBounds(tempNum, 0, 0, &nx1, &ny1, &numW, &numH);
  int16_t fx1, fy1; uint16_t fW, fH;
  tft.getTextBounds("F", 0, 0, &fx1, &fy1, &fW, &fH);

  int circleR = 4;
  int gapSmall = 4;
  int totalWidth = (int)numW + gapSmall + circleR * 2 + gapSmall + (int)fW;
  int startX = centerX - totalWidth / 2;

  tft.setTextColor(color);
  tft.setCursor(startX - nx1, topY - ny1);
  tft.print(tempNum);

  int circleCx = startX + (int)numW + gapSmall + circleR;
  int circleCy = topY + 6;
  tft.drawCircle(circleCx, circleCy, circleR, color);
  tft.drawCircle(circleCx, circleCy, circleR - 1, color);

  int fX = circleCx + circleR + gapSmall;
  tft.setCursor(fX - fx1, topY - fy1);
  tft.print("F");

  tft.setFont();
}

// ============================================================
// ICONS
// ============================================================
void iconCheckbox(int cx, int cy, uint16_t color) {
  tft.drawRoundRect(cx - 9, cy - 9, 18, 18, 4, color);
  tft.drawLine(cx - 5, cy,     cx - 1, cy + 4, color);
  tft.drawLine(cx - 5, cy + 1, cx - 1, cy + 5, color);
  tft.drawLine(cx - 1, cy + 4, cx + 6, cy - 5, color);
  tft.drawLine(cx - 1, cy + 5, cx + 6, cy - 4, color);
}
void iconPencil(int cx, int cy, uint16_t color) {
  tft.drawLine(cx - 6, cy + 8, cx + 5, cy - 7, color);
  tft.drawLine(cx - 5, cy + 8, cx + 6, cy - 6, color);
  tft.drawLine(cx - 7, cy + 7, cx - 5, cy + 9, color);
  tft.fillTriangle(cx + 4, cy - 7, cx + 7, cy - 5, cx + 6, cy - 4, color);
}
void iconBook(int cx, int cy, uint16_t color) {
  tft.drawLine(cx, cy - 6, cx, cy + 7, color);
  tft.drawLine(cx, cy - 6, cx - 10, cy - 2, color);
  tft.drawLine(cx - 10, cy - 2, cx - 10, cy + 6, color);
  tft.drawLine(cx - 10, cy + 6, cx, cy + 7, color);
  tft.drawLine(cx, cy - 6, cx + 10, cy - 2, color);
  tft.drawLine(cx + 10, cy - 2, cx + 10, cy + 6, color);
  tft.drawLine(cx + 10, cy + 6, cx, cy + 7, color);
}
void iconSun(int cx, int cy, uint16_t color) {
  tft.fillCircle(cx, cy, 5, color);
  for (int i = 0; i < 8; i++) {
    float angle = i * PI / 4;
    int x1 = cx + cos(angle) * 7, y1 = cy + sin(angle) * 7;
    int x2 = cx + cos(angle) * 11, y2 = cy + sin(angle) * 11;
    tft.drawLine(x1, y1, x2, y2, color);
  }
}
void iconGear(int cx, int cy, uint16_t color) {
  tft.drawCircle(cx, cy, 6, color);
  tft.fillCircle(cx, cy, 2, color);
  for (int i = 0; i < 6; i++) {
    float angle = i * PI / 3;
    int x1 = cx + cos(angle) * 6, y1 = cy + sin(angle) * 6;
    int x2 = cx + cos(angle) * 9, y2 = cy + sin(angle) * 9;
    tft.drawLine(x1, y1, x2, y2, color);
  }
}
void iconGrid(int cx, int cy, uint16_t color) {
  int spacing = 6;
  for (int row = -1; row <= 1; row++)
    for (int col = -1; col <= 1; col++)
      tft.fillCircle(cx + col * spacing, cy + row * spacing, 1, color);
}

void drawTileIcon(int i, int radius) {
  int cx = tileX[i] + tileW / 2;
  int cy = tileY[i] + 32;
  tft.fillRect(cx - 24, cy - 24, 48, 48, tileColors[i]);
  tft.fillCircle(cx, cy, radius, ILI9341_WHITE);
  switch (i) {
    case 1: iconCheckbox(cx, cy, tileColors[i]); break;
    case 2: iconPencil(cx, cy, tileColors[i]); break;
    case 3: iconBook(cx, cy, tileColors[i]); break;
    case 4: iconSun(cx, cy, tileColors[i]); break;
    case 5: iconGear(cx, cy, tileColors[i]); break;
  }
}

void updateHomeAnimations() {
  if (!homeAnimationsEnabled) return;
  // Was gated to once every 300ms, which made the icon "breathing" pulse
  // look choppy/steppy. Running it far more often (every ~20ms, ~50fps)
  // makes the same wave motion look smooth instead -- the CPU has plenty
  // of headroom for this, it just wasn't being asked to do it.
  if (millis() - lastAnimMs < 20) return;
  lastAnimMs = millis();
  for (int i = 1; i < 6; i++) {
    unsigned long period = 5200 + i * 900;
    unsigned long phase = i * 1400;
    float t = (float)((millis() + phase) % period) / (float)period;
    float wave = sin(t * 2 * PI);
    int radius = 20 + (int)(wave * 2);
    drawTileIcon(i, radius);
  }
}

// ============================================================
// SAFEGUARD: periodic display re-init while on Home/Lock only
// ============================================================
void maybeReinitDisplay(DateTime now) {
  if (millis() - lastReinitMs < REINIT_INTERVAL_MS) return;
  lastReinitMs = millis();
  if (currentScreen != SCREEN_HOME && currentScreen != SCREEN_LOCK) return;

  tft.begin();
  tft.setRotation(displayRotation);

  if (currentScreen == SCREEN_HOME) {
    drawHomeScreen(now);
  } else {
    drawLockScreen(now);
  }
}

// ============================================================
// SHARED: soft rounded button with a drop shadow
// ============================================================
void drawGlassRect(int x, int y, int w, int h, int r, uint16_t baseColor) {
  tft.fillRoundRect(x + 3, y + 4, w, h, r, COLOR_SHADOW);

  tft.fillRoundRect(x, y, w, h, r, baseColor);
  uint8_t rr = (baseColor >> 11) & 0x1F;
  uint8_t gg = (baseColor >> 5) & 0x3F;
  uint8_t bb = baseColor & 0x1F;
  uint8_t rd = rr > 5 ? rr - 5 : 0;
  uint8_t gd = gg > 10 ? gg - 10 : 0;
  uint8_t bd = bb > 5 ? bb - 5 : 0;
  uint16_t border = (rd << 11) | (gd << 5) | bd;
  tft.drawRoundRect(x, y, w, h, r, border);
}

void drawScreenFrame() {
  tft.drawRect(0, 0, 240, 320, COLOR_FRAME);
  tft.drawRect(1, 1, 238, 318, COLOR_FRAME);
}

// ============================================================
// SHARED: back bar
// ============================================================
void drawBackBar(const char* title) {
  tft.fillRect(0, 0, 240, 24, COLOR_STATUSBAR);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 8);
  tft.print("< Back");
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(title, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((240 - tw) / 2, 8);
  tft.print(title);
}
bool touchedBack(int x, int y) { return x < 110 && y < 32; }

// ============================================================
// SHARED: onscreen keyboard widget
// ============================================================
// Rows are centered on the 240px screen: 10 keys start at x=0, 9 at x=12,
// 8 at x=24, 7 at x=36, and so on (24px per key).
int kbRowStartX(const char* row) {
  return (240 - (int)strlen(row) * 24) / 2;
}

void drawKeyRow(const char* row, int y, int keyH, bool upperCase) {
  int len = strlen(row);
  int startX = kbRowStartX(row);
  for (int i = 0; i < len; i++) {
    int x = startX + i * 24;
    drawGlassRect(x, y, 22, keyH, 8, COLOR_SETTINGS);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(1);
    char c = row[i];
    if (upperCase) c = toupper(c);
    char label[2] = {c, '\0'};
    tft.setCursor(x + 7, y + keyH / 2 - 4);
    tft.print(label);
  }
}

char hitTestKeyRow(const char* row, int x, int y, int rowY, int keyH) {
  if (y < rowY || y > rowY + keyH) return 0;
  int startX = kbRowStartX(row);
  // Without this check a tap just left of the row rounded up to the first
  // key, because C divides negative numbers toward zero.
  if (x < startX) return 0;
  int idx = (x - startX) / 24;
  if (idx < 0 || idx >= (int)strlen(row)) return 0;
  return row[idx];
}

// Longest text each context can hold. WiFi passwords can be up to 63
// characters. A to-do is capped at 35 because the saved line is "[ ] " plus
// the text, and the list reader only holds 39 characters, so anything longer
// could not be ticked off or deleted afterwards.
int kbMaxLen() {
  if (kbContext == KB_CONTEXT_WIFI_PASSWORD) return 63;
  if (kbContext == KB_CONTEXT_ADD_TODO) return 35;
  if (kbContext == KB_CONTEXT_DEVICE_NAME) return DEVICE_NAME_MAX;
  return 36;
}

void drawKeyboardScreen() {
  tft.fillScreen(COLOR_BG);
  const char* title = "Add Task";
  if (kbContext == KB_CONTEXT_ADD_HOMEWORK) title = "Add Homework";
  if (kbContext == KB_CONTEXT_WIFI_PASSWORD) title = "WiFi Password";
  if (kbContext == KB_CONTEXT_DEVICE_NAME) title = "Name Your Device";
  drawBackBar(title);

  tft.fillRoundRect(10, 30, 220, 40, 12, COLOR_SURFACE);
  tft.drawRoundRect(10, 30, 220, 40, 12, COLOR_SUBTEXT);
  tft.setTextColor(COLOR_TEXT);
  tft.setTextSize(2);
  tft.setCursor(16, 42);
  // The box only fits 17 big characters, so once the text is longer, show
  // the end of it (the part being typed) instead of running off the screen.
  const int showMax = 17;
  const char* shown = kbBuffer;
  if (kbBufferLen > showMax) shown = kbBuffer + (kbBufferLen - showMax);
  tft.print(shown);

  bool upper = (kbMode == KB_MODE_ABC) && kbShift;
  drawKeyRow(kbGetRow1(), 76, 40, upper);
  drawKeyRow(kbGetRow2(), 120, 40, upper);
  drawKeyRow(kbGetRow3(), 164, 40, upper);

  // Page key: ABC -> 123 -> #+= -> ABC
  const char* pageLabel = "123";
  if (kbMode == KB_MODE_123) pageLabel = "#+=";
  else if (kbMode == KB_MODE_SYM) pageLabel = "ABC";
  drawGlassRect(8, 208, 70, 40, 12, COLOR_SETTINGS);
  printFitted(&FreeSansBold12pt7b, 8 + 35, 220, pageLabel, ILI9341_WHITE, 60);

  drawGlassRect(86, 208, 70, 40, 12, kbShift ? COLOR_ACCENT : COLOR_SUBTEXT);
  printFitted(&FreeSansBold12pt7b, 86 + 35, 220, "SHIFT", ILI9341_WHITE, 60);

  drawGlassRect(164, 208, 70, 40, 12, COLOR_SUBTEXT);
  printFitted(&FreeSansBold12pt7b, 164 + 35, 220, "DEL", ILI9341_WHITE, 60);

  drawGlassRect(4, 254, 140, 46, 14, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 4 + 140 / 2, 267, "SPACE", ILI9341_WHITE, 128);

  drawGlassRect(152, 254, 84, 46, 14, COLOR_HOMEWORK);
  printFitted(&FreeSansBold12pt7b, 152 + 84 / 2, 267, "DONE", ILI9341_WHITE, 74);
}

void handleKeyboardTouch(int x, int y) {
  char c = hitTestKeyRow(kbGetRow1(), x, y, 76, 40);
  if (!c) c = hitTestKeyRow(kbGetRow2(), x, y, 120, 40);
  if (!c) c = hitTestKeyRow(kbGetRow3(), x, y, 164, 40);
  if (c) {
    if (kbBufferLen < kbMaxLen()) {
      char insertChar = c;
      if (kbMode == KB_MODE_ABC && kbShift) insertChar = toupper(c);
      kbBuffer[kbBufferLen++] = insertChar;
      kbBuffer[kbBufferLen] = '\0';
      drawKeyboardScreen();
    }
    return;
  }

  if (x >= 8 && x <= 78 && y >= 208 && y <= 248) { // page key: ABC -> 123 -> #+= -> ABC
    if (kbMode == KB_MODE_ABC) kbMode = KB_MODE_123;
    else if (kbMode == KB_MODE_123) kbMode = KB_MODE_SYM;
    else kbMode = KB_MODE_ABC;
    drawKeyboardScreen();
    return;
  }
  if (x >= 86 && x <= 156 && y >= 208 && y <= 248) { // SHIFT
    kbShift = !kbShift;
    drawKeyboardScreen();
    return;
  }
  if (x >= 164 && x <= 234 && y >= 208 && y <= 248) { // DEL
    if (kbBufferLen > 0) {
      kbBufferLen--;
      kbBuffer[kbBufferLen] = '\0';
      drawKeyboardScreen();
    }
    return;
  }

  if (x >= 4 && x <= 144 && y >= 254 && y <= 300) { // SPACE
    if (kbBufferLen < kbMaxLen()) {
      kbBuffer[kbBufferLen++] = ' ';
      kbBuffer[kbBufferLen] = '\0';
      drawKeyboardScreen();
    }
    return;
  }

  if (x >= 152 && x <= 236 && y >= 254 && y <= 300) { // DONE
    if (kbContext == KB_CONTEXT_ADD_TODO) {
      if (kbBufferLen > 0) {
        char newTodo[44];
        snprintf(newTodo, sizeof(newTodo), "[ ] %s", kbBuffer);
        File f = LittleFS.open("/todo.txt", FILE_APPEND);
        if (f) { f.println(newTodo); f.close(); Serial.println("Task saved."); }
        else Serial.println("Failed to open todo.txt for writing.");
      }
      currentScreen = SCREEN_TODO;
      drawTodoScreen();
    } else if (kbContext == KB_CONTEXT_ADD_HOMEWORK) {
      if (kbBufferLen > 0) {
        strncpy(pendingHomeworkText, kbBuffer, sizeof(pendingHomeworkText) - 1);
        pendingHomeworkText[sizeof(pendingHomeworkText) - 1] = '\0';
        DateTime now = rtc.now();
        hwPeriodPickerIndex = guessHomeworkPeriodIndex(now.hour(), now.minute());
        currentScreen = SCREEN_HW_PERIOD;
        drawHwPeriodScreen();
      } else {
        currentScreen = SCREEN_HOMEWORK;
        drawHomeworkScreen();
      }
    } else if (kbContext == KB_CONTEXT_WIFI_PASSWORD) {
      WiFi.begin(wifiSelectedSSID, kbBuffer);
      addOrUpdateNetworkAtTop(wifiSelectedSSID, kbBuffer);
      wifiConnState = WIFI_CONN_CONNECTING;
      wifiConnectStartMs = millis();
      currentScreen = SCREEN_WIFI;
      drawWifiScreen();
    } else if (kbContext == KB_CONTEXT_DEVICE_NAME) {
      // trim spaces off both ends; keep the old name if nothing is left
      int start = 0, end = kbBufferLen;
      while (start < end && kbBuffer[start] == ' ') start++;
      while (end > start && kbBuffer[end - 1] == ' ') end--;
      if (end > start) {
        int n = end - start;
        if (n > DEVICE_NAME_MAX) n = DEVICE_NAME_MAX;
        memcpy(deviceName, kbBuffer + start, n);
        deviceName[n] = '\0';
        saveDeviceName();
      }
      leaveDeviceNameEntry();
    } else {
      currentScreen = SCREEN_TODO;
      drawTodoScreen();
    }
    return;
  }
}

// Opens the keyboard pre-filled with the current device name.
void startDeviceNameEntry() {
  strncpy(kbBuffer, deviceName, sizeof(kbBuffer) - 1);
  kbBuffer[sizeof(kbBuffer) - 1] = '\0';
  kbBufferLen = strlen(kbBuffer);
  kbMode = KB_MODE_ABC;
  kbShift = false;
  kbContext = KB_CONTEXT_DEVICE_NAME;
  currentScreen = SCREEN_KEYBOARD;
  drawKeyboardScreen();
}

// After naming (Done or Back): first-time setup carries on to Set Time,
// otherwise it's back to Settings.
void leaveDeviceNameEntry() {
  if (firstBootFlow) {
    DateTime now = rtc.now();
    editHour = now.hour();
    editMinute = now.minute();
    currentScreen = SCREEN_SET_TIME;
    drawSetTimeScreen();
  } else {
    currentScreen = SCREEN_SETTINGS;
    drawSettingsScreen();
  }
}

// ============================================================
// GRADIENT BACKGROUND + LOGO
// ============================================================
void drawGradientBG(uint16_t topColor, uint16_t bottomColor) {
  int tr = (topColor >> 11) & 0x1F, tg = (topColor >> 5) & 0x3F, tb = topColor & 0x1F;
  int br = (bottomColor >> 11) & 0x1F, bg = (bottomColor >> 5) & 0x3F, bb = bottomColor & 0x1F;
  for (int y = 0; y < 320; y++) {
    float f = (float)y / 319.0;
    int r = tr + (int)((br - tr) * f);
    int g = tg + (int)((bg - tg) * f);
    int b = tb + (int)((bb - tb) * f);
    uint16_t c = (r << 11) | (g << 5) | b;
    tft.drawFastHLine(0, y, 240, c);
  }
}

// Simple procedural wordmark: three concentric rings + "SynRG" text underneath.
void drawSynRGLogo(int cx, int cy, float scale, uint16_t ringColor, uint16_t textColor) {
  int r1 = (int)(34 * scale);
  int r2 = (int)(24 * scale);
  int r3 = (int)(14 * scale);
  tft.drawCircle(cx, cy, r1, ringColor);
  tft.drawCircle(cx, cy, r2, ringColor);
  tft.fillCircle(cx, cy, r3, ringColor);
  printCentered(&FreeSansBold12pt7b, cx, cy + r1 + (int)(20 * scale), "SynRG", textColor);
}

// ============================================================
// WELCOME SCREEN (first boot, or "Run First-Time Setup" from More)
// ============================================================
void drawWelcomeScreen() {
  uint16_t top = tft.color565(255, 214, 170);
  uint16_t bottom = tft.color565(230, 150, 200);
  drawGradientBG(top, bottom);

  // Dark text on the light gradient reads far better than white did.
  drawSynRGLogo(120, 110, 1.0, COLOR_TEXT, COLOR_TEXT);
  // 24pt "Hello, User" was ~248px wide on a 240px screen, and the one-line
  // tap hint was ~370px, so both ran off the edges. Smaller font for the
  // greeting and the hint split over two lines fixes both.
  printCentered(&FreeSansBold18pt7b, 120, 222, "Hello, User", COLOR_TEXT);
  printCentered(&FreeSans12pt7b, 120, 262, "Tap anywhere to", COLOR_SUBTEXT);
  printCentered(&FreeSans12pt7b, 120, 286, "set up your SynRG", COLOR_SUBTEXT);
}

void handleWelcomeTouch(int x, int y) {
  currentScreen = SCREEN_TOUCH_TEST;
  touchTestResult = 0;
  drawTouchTestScreen();
}

// ============================================================
// TOUCH ORIENTATION TEST
// ============================================================
void drawTouchTestScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Touch Check");

  tft.setTextColor(COLOR_TEXT);
  tft.setTextSize(1);
  tft.setCursor(14, 60);
  tft.println("Tap the green dot");
  tft.setCursor(14, 76);
  tft.println("in the corner below.");

  uint16_t testDotColor = tft.color565(40, 180, 90);

  if (touchTestResult == 0) {
    tft.fillCircle(TT_TARGET_X, TT_TARGET_Y, TT_TARGET_R, testDotColor);
    tft.drawCircle(TT_TARGET_X, TT_TARGET_Y, TT_TARGET_R + 3, COLOR_TEXT);
  } else if (touchTestResult == 1) {
    printCentered(&FreeSansBold12pt7b, 120, 140, "Touch looks good!", COLOR_HOMEWORK);
    drawGlassRect(60, 190, 120, 44, 14, COLOR_WEATHER);
    printFitted(&FreeSansBold12pt7b, 120, 204, "Continue", ILI9341_WHITE, 110);
  } else if (touchTestResult == 2) {
    printCentered(&FreeSansBold12pt7b, 120, 130, "Touch is mirrored", COLOR_ERROR);
    printCentered(&FreeSansBold12pt7b, 120, 154, "or upside-down.", COLOR_ERROR);
    printCentered(&FreeSans12pt7b, 120, 180, "Let whoever wired", COLOR_SUBTEXT);
    printCentered(&FreeSans12pt7b, 120, 198, "the screen know.", COLOR_SUBTEXT);
    drawGlassRect(60, 230, 120, 44, 14, COLOR_WEATHER);
    printFitted(&FreeSansBold12pt7b, 120, 244, "Continue", ILI9341_WHITE, 110);
  }

  // Always available on this screen: permanently flip the picture 180
  // degrees and save it, so it stays flipped on every future power-up.
  drawGlassRect(40, 280, 160, 32, 12, COLOR_SETTINGS);
  printFitted(&FreeSansBold12pt7b, 120, 293, "Flip Screen (saves)", ILI9341_WHITE, 150);
}

void handleTouchTestTouch(int x, int y) {
  if (x >= 40 && x <= 200 && y >= 280 && y <= 312) {
    displayRotation = (displayRotation + 2) % 4;
    saveDisplayRotation(displayRotation);
    tft.setRotation(displayRotation);
    touchTestResult = 0;
    drawTouchTestScreen();
    return;
  }

  if (touchTestResult == 0) {
    long dTarget = (long)(x - TT_TARGET_X) * (x - TT_TARGET_X) + (long)(y - TT_TARGET_Y) * (y - TT_TARGET_Y);
    int mirrorX = 240 - TT_TARGET_X;
    int mirrorY = 320 - TT_TARGET_Y;
    long dMirror = (long)(x - mirrorX) * (x - mirrorX) + (long)(y - mirrorY) * (y - mirrorY);

    if (dTarget <= (long)(TT_TARGET_R + 20) * (TT_TARGET_R + 20)) {
      touchTestResult = 1;
    } else if (dMirror < dTarget) {
      touchTestResult = 2;
    } else {
      touchTestResult = 1; // close enough, don't get the user stuck
    }
    drawTouchTestScreen();
    return;
  }

  // second tap = the Continue button, anywhere below the message is fine
  if (y > 180) {
    if (firstBootFlow) {
      startDeviceNameEntry();
    } else {
      currentScreen = SCREEN_HOME;
      drawHomeScreen(rtc.now());
      lastDrawnMinute = rtc.now().minute();
    }
  }
}

// ============================================================
// SHARED: live IP line on the Home and Lock screens. Shows the PDA's
// address (the one to type into a browser for the built-in web page), or
// the WiFi state when there isn't one. updateIpLine() redraws it whenever
// that text changes, so it stays live between the once-a-minute redraws.
// ============================================================
char shownIpText[32] = "";

void buildIpText(char* buf, size_t len) {
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(buf, len, "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  } else if (wifiConnState == WIFI_CONN_CONNECTING) {
    snprintf(buf, len, "WiFi: connecting...");
  } else {
    snprintf(buf, len, "WiFi: not connected");
  }
}

int ipLineY() { return currentScreen == SCREEN_LOCK ? 280 : 302; }

void drawIpLine() {
  buildIpText(shownIpText, sizeof(shownIpText));
  int y = ipLineY();
  tft.fillRect(4, y - 2, 232, 12, COLOR_BG);
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(COLOR_SUBTEXT);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(shownIpText, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((240 - tw) / 2, y);
  tft.print(shownIpText);
}

unsigned long lastIpCheckMs = 0;
void updateIpLine() {
  if (currentScreen != SCREEN_HOME && currentScreen != SCREEN_LOCK) return;
  if (millis() - lastIpCheckMs < 1000) return;
  lastIpCheckMs = millis();
  char now[32];
  buildIpText(now, sizeof(now));
  if (strcmp(now, shownIpText) != 0) drawIpLine();
}

// ============================================================
// UI: LOCK SCREEN
// ============================================================
void drawLockScreen(DateTime now) {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();

  drawSynRGLogo(120, 42, 0.42, COLOR_ACCENT, COLOR_SUBTEXT);

  char timeStr[9];
  formatTime12h(timeStr, now.hour(), now.minute());
  printCentered(&FreeSansBold24pt7b, 120, 90, timeStr, COLOR_TEXT);

  char dateStr[16];
  sprintf(dateStr, "%s, %02d/%02d", dayNames[now.dayOfTheWeek()], now.month(), now.day());
  printCentered(&FreeSans12pt7b, 120, 155, dateStr, COLOR_SUBTEXT);

  int idx = getCurrentPeriodIndex(now.hour(), now.minute());
  char statusStr[40];
  if (idx == -1) {
    sprintf(statusStr, "Out of school hours");
  } else {
    int nowMinutes = now.hour() * 60 + now.minute();
    int endMinutes = schedule[idx].endHour * 60 + schedule[idx].endMin;
    sprintf(statusStr, "%s - %d min left", schedule[idx].name, endMinutes - nowMinutes);
  }
  printFitted(&FreeSansBold12pt7b, 120, 195, statusStr, COLOR_ACCENT, 220);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_SUBTEXT);
  const char* hint = "Tap anywhere to unlock";
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(hint, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((240 - tw) / 2, 260);
  tft.print(hint);

  drawIpLine();
}

// ============================================================
// UI: HOME SCREEN
// ============================================================
void drawTileFrame(int x, int y, int w, int h, uint16_t color, const char* label) {
  drawGlassRect(x, y, w, h, 20, color);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(x + (w - tw) / 2, y + h - 16);
  tft.print(label);
}

// Small back arrow at the left of the Home status bar -- tapping it
// returns to the lock screen.
bool touchedHomeLockArrow(int x, int y) { return x < 36 && y < 28; }

void drawHomeStatusBar(DateTime now) {
  tft.fillRect(0, 0, 240, 24, COLOR_STATUSBAR);
  tft.drawRoundRect(4, 3, 24, 18, 4, COLOR_GRAY);
  tft.fillTriangle(10, 12, 16, 7, 16, 17, ILI9341_WHITE);
  tft.fillRect(16, 10, 6, 5, ILI9341_WHITE);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(36, 8);
  tft.print(dayNames[now.dayOfTheWeek()]);
  char dateStr[8];
  sprintf(dateStr, "%02d/%02d", now.month(), now.day());
  tft.setCursor(92, 8);
  tft.print(dateStr);
  char timeStr[9];
  formatTime12h(timeStr, now.hour(), now.minute());
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(timeStr, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(240 - 8 - tw, 8);
  tft.print(timeStr);
}

// ---- Clock widget: an analog face with hour, minute and a ticking second
// hand, plus the digital time with seconds underneath. The face is drawn
// once; each second only the hands and digits are erased and redrawn. ----
const int CLOCK_R = 28;
int clockShownSecond = -1;
int clockShownH = 0, clockShownM = 0, clockShownS = 0;

int clockCX() { return tileX[0] + tileW / 2; }
int clockCY() { return tileY[0] + 34; }

void clockHandEnd(float frac, int len, int &x, int &y) {
  float a = frac * 2 * PI - PI / 2;
  x = clockCX() + (int)(cos(a) * len);
  y = clockCY() + (int)(sin(a) * len);
}

void drawClockFace() {
  int cx = clockCX(), cy = clockCY();
  tft.fillCircle(cx, cy, CLOCK_R, COLOR_SURFACE);
  tft.drawCircle(cx, cy, CLOCK_R, COLOR_TEXT);
  for (int i = 0; i < 12; i++) {
    float a = i * PI / 6;
    int inner = (i % 3 == 0) ? CLOCK_R - 6 : CLOCK_R - 3;
    tft.drawLine(cx + cos(a) * inner, cy + sin(a) * inner,
                 cx + cos(a) * (CLOCK_R - 1), cy + sin(a) * (CLOCK_R - 1), COLOR_TEXT);
  }
}

void drawClockHands(int h, int m, int sec, bool erase) {
  int cx = clockCX(), cy = clockCY(), x, y;
  uint16_t handColor = erase ? COLOR_SURFACE : COLOR_TEXT;
  uint16_t secColor = erase ? COLOR_SURFACE : COLOR_ERROR;

  clockHandEnd(((h % 12) + m / 60.0) / 12.0, 13, x, y);       // hour (thick)
  tft.drawLine(cx, cy, x, y, handColor);
  tft.drawLine(cx + 1, cy, x + 1, y, handColor);
  tft.drawLine(cx, cy + 1, x, y + 1, handColor);

  clockHandEnd((m + sec / 60.0) / 60.0, 19, x, y);            // minute
  tft.drawLine(cx, cy, x, y, handColor);
  tft.drawLine(cx + 1, cy, x + 1, y, handColor);

  clockHandEnd(sec / 60.0, 21, x, y);                         // second
  tft.drawLine(cx, cy, x, y, secColor);

  if (!erase) tft.fillCircle(cx, cy, 2, COLOR_ERROR);
}

// Called every loop() on Home; does nothing unless the Clock widget is
// showing and the second has changed.
void updateClockWidget(DateTime now) {
  if (homeWidget != WIDGET_CLOCK) return;
  if (now.second() == clockShownSecond) return;

  if (clockShownSecond >= 0) drawClockHands(clockShownH, clockShownM, clockShownS, true);
  drawClockHands(now.hour(), now.minute(), now.second(), false);
  clockShownH = now.hour(); clockShownM = now.minute(); clockShownS = now.second();
  clockShownSecond = now.second();

  // digital time with seconds, under the face
  int dh = now.hour() % 12;
  if (dh == 0) dh = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d:%02d %s", dh, now.minute(), now.second(), now.hour() < 12 ? "AM" : "PM");
  int ty = tileY[0] + tileH - 14;
  tft.fillRect(tileX[0] + 6, ty - 1, tileW - 12, 10, tileColors[0]);
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(clockCX() - tw / 2, ty);
  tft.print(buf);
}

// Slot 0 (top-left) is a live widget rather than an app launcher. Which
// widget it shows is picked in Settings ("Widget: ..."); it's refreshed
// every minute along with the rest of the Home screen. Tapping it does
// nothing -- it's meant to be glanced at.
int countFileLines(const char* path) {
  if (!LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path);
  if (!f) return 0;
  int n = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) n++;
  }
  f.close();
  return n;
}

void drawWidgetTile() {
  DateTime now = rtc.now();
  drawGlassRect(tileX[0], tileY[0], tileW, tileH, 20, tileColors[0]);

  int x = tileX[0] + 10, y = tileY[0];
  int nowMinutes = now.hour() * 60 + now.minute();
  char buf[24];
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);

  switch (homeWidget) {
    case WIDGET_NEXT_CLASS: {
      int next = -1;
      for (int i = 0; i < numPeriods; i++) {
        int start = schedule[i].startHour * 60 + schedule[i].startMin;
        if (start > nowMinutes && (next == -1 || start < schedule[next].startHour * 60 + schedule[next].startMin)) next = i;
      }
      if (next == -1) {
        tft.setCursor(x, y + 30); tft.print("No more");
        tft.setCursor(x, y + 44); tft.print("classes");
      } else {
        tft.setCursor(x, y + 24); tft.print(schedule[next].name);
        formatTime12h(buf, schedule[next].startHour, schedule[next].startMin);
        tft.setCursor(x, y + 44); tft.print(buf);
      }
      break;
    }
    case WIDGET_CLOCK:
      drawClockFace();
      clockShownSecond = -1;     // force hands + digital time to draw now
      updateClockWidget(now);
      return;                    // the clock draws its own label (digital time)
    case WIDGET_WEATHER:
      printFitted(&FreeSansBold12pt7b, tileX[0] + tileW / 2, y + 22, weatherTemp, ILI9341_WHITE, tileW - 12);
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(1);
      snprintf(buf, sizeof(buf), "%.14s", weatherDesc);
      tft.setCursor(x, y + 48); tft.print(buf);
      break;
    case WIDGET_TODO: {
      int n = countFileLines("/todo.txt");
      snprintf(buf, sizeof(buf), "%d", n);
      printFitted(&FreeSansBold12pt7b, tileX[0] + tileW / 2, y + 22, buf, ILI9341_WHITE, tileW - 12);
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(1);
      tft.setCursor(x, y + 48); tft.print(n == 1 ? "item to do" : "items to do");
      break;
    }
    default: { // WIDGET_CLASS
      int idx = getCurrentPeriodIndex(now.hour(), now.minute());
      if (idx == -1) {
        tft.setCursor(x, y + 30); tft.print("No class");
        tft.setCursor(x, y + 44); tft.print("right now");
      } else {
        int endMinutes = schedule[idx].endHour * 60 + schedule[idx].endMin;
        tft.setCursor(x, y + 24); tft.print(schedule[idx].name);
        sprintf(buf, "%d min left", endMinutes - nowMinutes);
        tft.setCursor(x, y + 44); tft.print(buf);
      }
      break;
    }
  }

  tft.setCursor(x, y + tileH - 16);
  tft.print(widgetNames[homeWidget]);
}

void drawHomeScreen(DateTime now) {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawHomeStatusBar(now);

  drawWidgetTile();
  for (int i = 1; i < 6; i++) {
    drawTileFrame(tileX[i], tileY[i], tileW, tileH, tileColors[i], tileNames[i]);
    drawTileIcon(i, 20);
  }
  drawIpLine();
}

void handleHomeTouch(int x, int y) {
  if (touchedHomeLockArrow(x, y)) {
    DateTime now = rtc.now();
    currentScreen = SCREEN_LOCK;
    drawLockScreen(now);
    lastDrawnMinute = now.minute();
    return;
  }
  for (int i = 0; i < 6; i++) {
    if (x >= tileX[i] && x <= tileX[i] + tileW && y >= tileY[i] && y <= tileY[i] + tileH) {
      if (i == 0) {
        // Status widget -- not a launcher, tapping it does nothing for now.
      } else if (i == 1) {
        currentScreen = SCREEN_TODO;
        drawTodoScreen();
      } else if (i == 2) {
        currentScreen = SCREEN_NOTES;
        drawNotesScreen();
      } else if (i == 3) {
        currentScreen = SCREEN_HOMEWORK;
        drawHomeworkScreen();
      } else if (i == 4) {
        currentScreen = SCREEN_WEATHER;
        drawWeatherScreen();
      } else {
        settingsCategory = -1;
        currentScreen = SCREEN_SETTINGS;
        drawSettingsScreen();
      }
      return;
    }
  }
}

// ============================================================
// UI: TO-DO (with delete)
// ============================================================
bool touchedAddButton(int x, int y) { return x >= 20 && x <= 220 && y >= 270 && y <= 306; }

int touchedTodoDelete(int x, int y) {
  for (int i = 0; i < todoLineCount; i++) {
    if (x >= 204 && x <= 232 && y >= todoLineY[i] && y <= todoLineY[i] + 20) {
      return i;
    }
  }
  return -1;
}

int touchedTodoCheckbox(int x, int y) {
  for (int i = 0; i < todoLineCount; i++) {
    if (x >= 12 && x <= 34 && y >= todoLineY[i] - 2 && y <= todoLineY[i] + 20) {
      return i;
    }
  }
  return -1;
}

// A to-do line is stored on disk as "[x] text" (done) or "[ ] text" (not
// done). Older lines saved before checkboxes existed have no prefix at
// all -- those are just treated as not-done.
void toggleTodoLine(const char* oldLine) {
  bool completed = (strncmp(oldLine, "[x] ", 4) == 0);
  const char* rest = oldLine;
  if (strncmp(oldLine, "[x] ", 4) == 0 || strncmp(oldLine, "[ ] ", 4) == 0) rest = oldLine + 4;

  char newLine[44];
  snprintf(newLine, sizeof(newLine), completed ? "[ ] %s" : "[x] %s", rest);

  char buf[40];
  bool replaced = false;
  LittleFS.remove("/tmp_del.txt");
  File src = LittleFS.open("/todo.txt");
  File dst = LittleFS.open("/tmp_del.txt", FILE_WRITE);
  if (src && dst) {
    while (readLineFromFile(src, buf, sizeof(buf))) {
      if (!replaced && strcmp(buf, oldLine) == 0) {
        dst.println(newLine);
        replaced = true;
      } else if (strlen(buf) > 0) {
        dst.println(buf);
      }
    }
  }
  if (src) src.close();
  if (dst) dst.close();

  LittleFS.remove("/todo.txt");
  File tmp = LittleFS.open("/tmp_del.txt");
  File out = LittleFS.open("/todo.txt", FILE_WRITE);
  if (tmp && out) {
    while (readLineFromFile(tmp, buf, sizeof(buf))) {
      if (strlen(buf) > 0) out.println(buf);
    }
  }
  if (tmp) tmp.close();
  if (out) out.close();
  LittleFS.remove("/tmp_del.txt");
}

void drawTodoScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("To-Do");

  int y = 34;
  todoLineCount = 0;
  char line[40];
  File f = LittleFS.open("/todo.txt");
  if (f) {
    while (readLineFromFile(f, line, sizeof(line)) && todoLineCount < 8) {
      if (strlen(line) > 0) {
        strncpy(todoLines[todoLineCount], line, sizeof(todoLines[0]) - 1);
        todoLines[todoLineCount][sizeof(todoLines[0]) - 1] = '\0';
        todoLineY[todoLineCount] = y;

        bool completed = (strncmp(line, "[x] ", 4) == 0);
        const char* displayText = line;
        if (strncmp(line, "[x] ", 4) == 0 || strncmp(line, "[ ] ", 4) == 0) displayText = line + 4;

        tft.drawRoundRect(12, y + 2, 16, 16, 3, COLOR_SUBTEXT);
        if (completed) {
          tft.drawLine(15, y + 10, 19, y + 15, COLOR_ACCENT);
          tft.drawLine(19, y + 15, 25, y + 5, COLOR_ACCENT);
        }

        tft.setTextColor(completed ? COLOR_SUBTEXT : COLOR_TEXT);
        tft.setTextSize(1);
        tft.setCursor(36, y + 8);
        tft.print(displayText);

        tft.setTextColor(COLOR_SUBTEXT);
        tft.setCursor(212, y + 8);
        tft.print("x");

        todoLineCount++;
        y += 26;
      }
    }
    f.close();
  }
  if (todoLineCount == 0) {
    tft.setTextColor(COLOR_SUBTEXT);
    tft.setTextSize(1);
    tft.setCursor(16, y + 8);
    tft.print("No tasks yet");
  }

  drawGlassRect(20, 270, 200, 36, 18, COLOR_TODO);
  printFitted(&FreeSansBold12pt7b, 120, 280, "+ Add Task", ILI9341_WHITE, 190);
}

// ============================================================
// UI: HOMEWORK (with delete, auto-sorted by period + Unscheduled)
// ============================================================
int touchedHomeworkDelete(int x, int y) {
  for (int i = 0; i < hwLineCount; i++) {
    if (x >= 204 && x <= 232 && y >= hwLineY[i] - 4 && y <= hwLineY[i] + 14) {
      return i;
    }
  }
  return -1;
}

void drawHomeworkScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Homework");

  int y = 30;
  int linesDrawn = 0;
  const int maxLines = 15;
  char line[60];
  char rawLine[60];
  hwLineCount = 0;

  for (int p = 0; p < numPeriods && linesDrawn < maxLines; p++) {
    bool headerDrawn = false;
    File f = LittleFS.open("/homework.txt");
    if (f) {
      while (readLineFromFile(f, line, sizeof(line)) && linesDrawn < maxLines) {
        if (strlen(line) == 0) continue;
        strncpy(rawLine, line, sizeof(rawLine) - 1);
        rawLine[sizeof(rawLine) - 1] = '\0';

        char* sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';
        char* periodPart = line;
        char* textPart = sep + 1;
        if (strcmp(periodPart, schedule[p].name) == 0) {
          if (!headerDrawn) {
            tft.setTextColor(COLOR_ACCENT);
            tft.setTextSize(1);
            tft.setCursor(12, y);
            tft.print(schedule[p].name);
            y += 14;
            headerDrawn = true;
            linesDrawn++;
          }
          tft.setTextColor(COLOR_TEXT);
          tft.setTextSize(1);
          tft.setCursor(20, y);
          tft.print("- ");
          tft.print(textPart);

          if (hwLineCount < 8) {
            strncpy(hwLines[hwLineCount], rawLine, sizeof(hwLines[0]) - 1);
            hwLines[hwLineCount][sizeof(hwLines[0]) - 1] = '\0';
            hwLineY[hwLineCount] = y;
            tft.setTextColor(COLOR_SUBTEXT);
            tft.setCursor(212, y);
            tft.print("x");
            hwLineCount++;
          }

          y += 13;
          linesDrawn++;
        }
      }
      f.close();
    }
    if (headerDrawn) y += 4;
  }

  // Unscheduled bucket -- entries logged outside school hours
  {
    bool headerDrawn = false;
    File f = LittleFS.open("/homework.txt");
    if (f) {
      while (readLineFromFile(f, line, sizeof(line)) && linesDrawn < maxLines) {
        if (strlen(line) == 0) continue;
        strncpy(rawLine, line, sizeof(rawLine) - 1);
        rawLine[sizeof(rawLine) - 1] = '\0';

        char* sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';
        char* periodPart = line;
        char* textPart = sep + 1;
        if (strcmp(periodPart, "Unscheduled") == 0) {
          if (!headerDrawn) {
            tft.setTextColor(COLOR_ACCENT);
            tft.setTextSize(1);
            tft.setCursor(12, y);
            tft.print("Unscheduled");
            y += 14;
            headerDrawn = true;
            linesDrawn++;
          }
          tft.setTextColor(COLOR_TEXT);
          tft.setTextSize(1);
          tft.setCursor(20, y);
          tft.print("- ");
          tft.print(textPart);

          if (hwLineCount < 8) {
            strncpy(hwLines[hwLineCount], rawLine, sizeof(hwLines[0]) - 1);
            hwLines[hwLineCount][sizeof(hwLines[0]) - 1] = '\0';
            hwLineY[hwLineCount] = y;
            tft.setTextColor(COLOR_SUBTEXT);
            tft.setCursor(212, y);
            tft.print("x");
            hwLineCount++;
          }

          y += 13;
          linesDrawn++;
        }
      }
      f.close();
    }
    if (headerDrawn) y += 4;
  }

  if (linesDrawn == 0) {
    tft.setTextColor(COLOR_SUBTEXT);
    tft.setTextSize(1);
    tft.setCursor(16, y + 8);
    tft.print("Nothing logged yet");
  }

  drawGlassRect(20, 270, 200, 36, 18, COLOR_HOMEWORK);
  printFitted(&FreeSansBold12pt7b, 120, 280, "+ Add Homework", ILI9341_WHITE, 190);
}

// ============================================================
// UI: HOMEWORK PERIOD PICKER -- lets you pick which class period a new
// homework item belongs to, instead of always trusting the clock. Handy
// during the gap between classes, when it's genuinely ambiguous which
// class you're heading to.
// ============================================================
// Best-guess default: the period you're in right now, or -- if you're in
// the gap between classes -- whichever period starts within the next 15
// minutes (so it guesses the *next* class during a change-of-classes
// buffer instead of just shrugging and saying "Unscheduled"). You can
// always override the guess with the < / > arrows regardless.
int guessHomeworkPeriodIndex(int hour, int minute) {
  int idx = getCurrentPeriodIndex(hour, minute);
  if (idx != -1) return idx;

  int nowMinutes = hour * 60 + minute;
  int best = -1;
  int bestDelta = 9999;
  for (int i = 0; i < numPeriods; i++) {
    int startMinutes = schedule[i].startHour * 60 + schedule[i].startMin;
    int delta = startMinutes - nowMinutes;
    if (delta >= 0 && delta <= 15 && delta < bestDelta) {
      best = i;
      bestDelta = delta;
    }
  }
  return best;
}

void drawHwPeriodScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Log For...");

  printCentered(&FreeSans12pt7b, 120, 70, "Which class period is", COLOR_SUBTEXT);
  printCentered(&FreeSans12pt7b, 120, 92, "this homework for?", COLOR_SUBTEXT);

  const char* label = (hwPeriodPickerIndex == -1) ? "Unscheduled" : schedule[hwPeriodPickerIndex].name;

  drawGlassRect(12, 130, 40, 50, 16, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 32, 148, "<", ILI9341_WHITE, 30);

  drawGlassRect(56, 130, 128, 50, 16, COLOR_SETTINGS);
  printFitted(&FreeSansBold12pt7b, 120, 148, label, ILI9341_WHITE, 118);

  drawGlassRect(188, 130, 40, 50, 16, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 208, 148, ">", ILI9341_WHITE, 30);

  drawGlassRect(60, 220, 120, 44, 18, COLOR_HOMEWORK);
  printFitted(&FreeSansBold12pt7b, 120, 234, "Save", ILI9341_WHITE, 110);
}

void handleHwPeriodTouch(int x, int y) {
  if (x >= 12 && x <= 52 && y >= 130 && y <= 180) {
    hwPeriodPickerIndex--;
    if (hwPeriodPickerIndex < -1) hwPeriodPickerIndex = numPeriods - 1;
    drawHwPeriodScreen();
    return;
  }
  if (x >= 188 && x <= 228 && y >= 130 && y <= 180) {
    hwPeriodPickerIndex++;
    if (hwPeriodPickerIndex >= numPeriods) hwPeriodPickerIndex = -1;
    drawHwPeriodScreen();
    return;
  }
  if (x >= 60 && x <= 180 && y >= 220 && y <= 264) {
    const char* periodName = (hwPeriodPickerIndex == -1) ? "Unscheduled" : schedule[hwPeriodPickerIndex].name;
    File f = LittleFS.open("/homework.txt", FILE_APPEND);
    if (f) {
      f.print(periodName);
      f.print("|");
      f.println(pendingHomeworkText);
      f.close();
      Serial.println("Homework saved.");
    } else {
      Serial.println("Failed to open homework.txt for writing.");
    }
    currentScreen = SCREEN_HOMEWORK;
    drawHomeworkScreen();
    return;
  }
}

// ============================================================
// UI: NOTES (live drawing canvas, with Save/Load, noise-filtered)
// ============================================================
bool touchedNotesClear(int x, int y) { return x >= 4 && x <= 114 && y >= 32 && y <= 54; }
bool touchedNotesSave(int x, int y) { return x >= 126 && x <= 236 && y >= 32 && y <= 54; }

void drawNotesCanvasArea() {
  tft.fillRect(0, 58, 240, 320 - 58, COLOR_SURFACE);
}

void saveNotesToFile() {
  LittleFS.remove("/note1.txt");
  File f = LittleFS.open("/note1.txt", FILE_WRITE);
  if (f) {
    for (int i = 0; i < notePointCount; i++) {
      f.print(noteNewStroke[i] ? "M," : "L,");
      f.print(noteX[i]);
      f.print(",");
      f.println(noteY[i]);
    }
    f.close();
    Serial.println("Note saved.");
  } else {
    Serial.println("Failed to save note.");
  }
}

void loadNotesFromFile() {
  notePointCount = 0;
  notesPrevX = -1;
  notesPrevY = -1;
  if (!LittleFS.exists("/note1.txt")) return;

  char line[24];
  File f = LittleFS.open("/note1.txt");
  if (!f) return;
  while (readLineFromFile(f, line, sizeof(line)) && notePointCount < 500) {
    if (strlen(line) < 3) continue;
    char cmd = line[0];
    char* rest = line + 2;
    char* commaPos = strchr(rest, ',');
    if (!commaPos) continue;
    *commaPos = '\0';
    int px = atoi(rest);
    int py = atoi(commaPos + 1);

    bool isNew = (cmd == 'M');
    if (isNew || notesPrevX == -1) {
      tft.fillCircle(px, py, 1, COLOR_TEXT);
    } else {
      tft.drawLine(notesPrevX, notesPrevY, px, py, COLOR_TEXT);
    }
    noteX[notePointCount] = px;
    noteY[notePointCount] = py;
    noteNewStroke[notePointCount] = isNew;
    notePointCount++;
    notesPrevX = px;
    notesPrevY = py;
  }
  f.close();
  notesPrevX = -1;
  notesPrevY = -1;
}

void drawNotesClearButton() {
  drawGlassRect(4, 32, 110, 22, 10, COLOR_SUBTEXT);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(30, 39);
  tft.print("Clear");
}

// pressed=true draws a flat, darker "pushed in" look (no drop shadow) for
// a brief moment when the button is tapped, before the normal raised
// look is redrawn -- a simple press animation for touch feedback.
void drawNotesSaveButton(bool pressed) {
  if (pressed) {
    tft.fillRoundRect(126, 32, 110, 22, 10, COLOR_ACCENT);
  } else {
    drawGlassRect(126, 32, 110, 22, 10, COLOR_HOMEWORK);
  }
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(155, 39);
  tft.print("Save");
}

void drawNotesScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Notes");

  drawNotesClearButton();
  drawNotesSaveButton(false);

  drawNotesCanvasArea();
  loadNotesFromFile();
}

void handleNotesDraw(int x, int y, bool isNewStroke) {
  if (y < 58) return;

  if (!isNewStroke && notesPrevX != -1) {
    int dx = x - notesPrevX;
    int dy = y - notesPrevY;
    int distSq = dx * dx + dy * dy;
    if (distSq > 2500) {
      return;
    }
  }

  if (isNewStroke || notesPrevX == -1) {
    tft.fillCircle(x, y, 1, COLOR_TEXT);
  } else {
    tft.drawLine(notesPrevX, notesPrevY, x, y, COLOR_TEXT);
  }
  if (notePointCount < 500) {
    noteX[notePointCount] = x;
    noteY[notePointCount] = y;
    noteNewStroke[notePointCount] = (isNewStroke || notesPrevX == -1);
    notePointCount++;
  }
  notesPrevX = x;
  notesPrevY = y;
}

// ============================================================
// UI: WIFI
// ============================================================
bool touchedWifiScan(int x, int y) { return x >= 20 && x <= 160 && y >= 30 && y <= 62; }
bool touchedWifiSavedNav(int x, int y) { return x >= 166 && x <= 220 && y >= 30 && y <= 62; }

int touchedWifiSSID(int x, int y) {
  for (int i = 0; i < wifiFoundCount; i++) {
    int rowY = 94 + i * 24;
    if (x >= 10 && x <= 230 && y >= rowY && y <= rowY + 22) return i;
  }
  return -1;
}

void drawWifiScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("WiFi");

  drawGlassRect(20, 30, 140, 32, 16, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 90, 40, "Scan", ILI9341_WHITE, 130);

  drawGlassRect(166, 30, 54, 32, 16, COLOR_SETTINGS);
  printFitted(&FreeSansBold12pt7b, 193, 40, "Saved", ILI9341_WHITE, 48);

  const char* statusText = "Idle";
  uint16_t statusColor = COLOR_SUBTEXT;
  if (wifiConnState == WIFI_CONN_CONNECTING) { statusText = "Connecting..."; statusColor = COLOR_WEATHER; }
  else if (wifiConnState == WIFI_CONN_CONNECTED) { statusText = wifiStatusMsg; statusColor = COLOR_HOMEWORK; }
  else if (wifiConnState == WIFI_CONN_FAILED) { statusText = "Connection failed"; statusColor = COLOR_ERROR; }

  tft.setTextSize(1);
  tft.setTextColor(statusColor);
  tft.setCursor(12, 72);
  tft.print(statusText);

  for (int i = 0; i < wifiFoundCount; i++) {
    int rowY = 94 + i * 24;
    drawGlassRect(10, rowY, 220, 22, 10, COLOR_SETTINGS);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(1);
    tft.setCursor(18, rowY + 7);
    tft.print(wifiSSIDs[i]);
  }
  if (wifiFoundCount == 0) {
    tft.setTextColor(COLOR_SUBTEXT);
    tft.setTextSize(1);
    tft.setCursor(12, 96);
    tft.print("Tap Scan to find networks");
  }
}

void handleWifiTouch(int x, int y) {
  if (touchedWifiScan(x, y)) {
    tft.fillRect(0, 66, 240, 24, COLOR_BG);
    tft.setTextColor(COLOR_SUBTEXT);
    tft.setTextSize(1);
    tft.setCursor(12, 72);
    tft.print("Scanning...");

    int found = WiFi.scanNetworks();
    if (found < 0) found = 0;
    if (found > 10) found = 10;
    wifiFoundCount = found;
    for (int i = 0; i < wifiFoundCount; i++) {
      strncpy(wifiSSIDs[i], WiFi.SSID(i).c_str(), 32);
      wifiSSIDs[i][32] = '\0';
    }
    drawWifiScreen();
    return;
  }

  if (touchedWifiSavedNav(x, y)) {
    currentScreen = SCREEN_WIFI_SAVED;
    drawWifiSavedScreen();
    return;
  }

  int idx = touchedWifiSSID(x, y);
  if (idx != -1) {
    strncpy(wifiSelectedSSID, wifiSSIDs[idx], 32);
    wifiSelectedSSID[32] = '\0';
    kbBuffer[0] = '\0';
    kbBufferLen = 0;
    kbMode = KB_MODE_ABC;
    kbShift = false;
    kbContext = KB_CONTEXT_WIFI_PASSWORD;
    currentScreen = SCREEN_KEYBOARD;
    drawKeyboardScreen();
    return;
  }
}

// ============================================================
// UI: WIFI SAVED (priority list: promote / delete)
// ============================================================
bool touchedSavedPromote(int i, int x, int y) {
  int rowY = 34 + i * 46;
  return x >= 154 && x <= 186 && y >= rowY + 3 && y <= rowY + 35;
}
bool touchedSavedDelete(int i, int x, int y) {
  int rowY = 34 + i * 46;
  return x >= 190 && x <= 222 && y >= rowY + 3 && y <= rowY + 35;
}

void drawWifiSavedScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Saved Networks");

  savedNetCount = loadSavedNetworks(savedNetSsidsCache, savedNetPassesCache, MAX_SAVED_NETWORKS);

  if (savedNetCount == 0) {
    printCentered(&FreeSans12pt7b, 120, 100, "No saved networks", COLOR_SUBTEXT);
    return;
  }

  for (int i = 0; i < savedNetCount; i++) {
    int rowY = 34 + i * 46;
    drawGlassRect(10, rowY, 220, 38, 12, i == 0 ? COLOR_ACCENT : COLOR_SETTINGS);
    char label[40];
    sprintf(label, "%d. %s", i + 1, savedNetSsidsCache[i]);
    printFitted(&FreeSansBold12pt7b, 82, rowY + 12, label, ILI9341_WHITE, 128);

    drawGlassRect(154, rowY + 3, 32, 32, 8, COLOR_HOMEWORK);
    printFitted(&FreeSansBold12pt7b, 170, rowY + 12, "^", ILI9341_WHITE, 26);

    drawGlassRect(190, rowY + 3, 32, 32, 8, COLOR_ERROR);
    printFitted(&FreeSansBold12pt7b, 206, rowY + 12, "x", ILI9341_WHITE, 26);
  }
}

void handleWifiSavedTouch(int x, int y) {
  for (int i = 0; i < savedNetCount; i++) {
    if (touchedSavedPromote(i, x, y)) {
      promoteNetworkToTop(i);
      drawWifiSavedScreen();
      return;
    }
    if (touchedSavedDelete(i, x, y)) {
      deleteSavedNetwork(i);
      drawWifiSavedScreen();
      return;
    }
  }
}

// ============================================================
// UI: WEATHER
// ============================================================
const char* weatherCodeToDesc(int code) {
  if (code == 0) return "Clear";
  if (code == 1 || code == 2) return "Partly Cloudy";
  if (code == 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Rain Showers";
  if (code >= 85 && code <= 86) return "Snow Showers";
  if (code >= 95 && code <= 99) return "Thunderstorm";
  return "Unknown";
}

bool touchedWeatherRefresh(int x, int y) { return x >= 70 && x <= 170 && y >= 260 && y <= 300; }

void drawWeatherScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Weather");

  printCentered(&FreeSans12pt7b, 120, 80, WEATHER_LOCATION_LABEL, COLOR_SUBTEXT);

  if (weatherFetching) {
    printCentered(&FreeSans12pt7b, 120, 150, "Fetching...", COLOR_SUBTEXT);
  } else {
    drawTempWithDegree(weatherTemp, 120, 120, COLOR_TEXT);
    printCentered(&FreeSans12pt7b, 120, 185, weatherDesc, COLOR_SUBTEXT);
  }

  drawGlassRect(70, 260, 100, 40, 18, COLOR_WEATHER);
  printFitted(&FreeSansBold12pt7b, 120, 272, "Refresh", ILI9341_WHITE, 90);
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    strcpy(weatherDesc, "Connect WiFi first");
    strcpy(weatherTemp, "--");
    drawWeatherScreen();
    return;
  }

  weatherFetching = true;
  drawWeatherScreen();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LAT "&longitude=" WEATHER_LON "&current_weather=true&temperature_unit=fahrenheit";
  http.begin(client, url);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      float t = doc["current_weather"]["temperature"];
      int wcode = doc["current_weather"]["weathercode"];
      sprintf(weatherTemp, "%.0f", t);
      strncpy(weatherDesc, weatherCodeToDesc(wcode), sizeof(weatherDesc) - 1);
      weatherDesc[sizeof(weatherDesc) - 1] = '\0';
    } else {
      strcpy(weatherDesc, "Parse error");
      strcpy(weatherTemp, "--");
    }
  } else {
    strcpy(weatherDesc, "Fetch failed");
    strcpy(weatherTemp, "--");
  }
  http.end();

  weatherFetching = false;
  drawWeatherScreen();
}

// ============================================================
// UI: SNAKE GAME
// ============================================================
// Layout: a back bar + score line up top, then a big play field that
// takes up the rest of the screen. Controlled entirely by swiping on
// the field -- swipe up/down/left/right to steer, no on-screen buttons.
#define SNAKE_CELL 8
const int snakeGridCols = 26;
const int snakeGridRows = 31;
const int snakeFieldW = snakeGridCols * SNAKE_CELL;  // 208
const int snakeFieldH = snakeGridRows * SNAKE_CELL;  // 248
const int snakeFieldX = (240 - snakeFieldW) / 2;      // 16
const int snakeFieldY = 46;

#define SNAKE_MAX_LEN 300
int snakeX[SNAKE_MAX_LEN], snakeY[SNAKE_MAX_LEN];
int snakeLen = 3;
SnakeDir snakeDir = SNAKE_RIGHT;
SnakeDir snakeNextDir = SNAKE_RIGHT;
int snakeFoodX = 0, snakeFoodY = 0;
bool snakeGameOver = false;
unsigned long snakeLastTickMs = 0;
const unsigned long SNAKE_TICK_MS = 140;
int snakeScore = 0;

// ---- Swipe tracking: a swipe is measured from where the finger first
// touched down on the field to how far it has dragged since. Once it
// clears the threshold in some direction, that swipe "fires" once and
// won't fire again until the finger lifts and touches down again. ----
int snakeSwipeStartX = -1;
int snakeSwipeStartY = -1;
bool snakeSwipeFired = false;
const int SNAKE_SWIPE_THRESHOLD = 18;

void snakePlaceFood() {
  bool ok = false;
  while (!ok) {
    snakeFoodX = random(0, snakeGridCols);
    snakeFoodY = random(0, snakeGridRows);
    ok = true;
    for (int i = 0; i < snakeLen; i++) {
      if (snakeX[i] == snakeFoodX && snakeY[i] == snakeFoodY) { ok = false; break; }
    }
  }
}

void snakeInitGame() {
  snakeLen = 3;
  int startX = snakeGridCols / 2;
  int startY = snakeGridRows / 2;
  for (int i = 0; i < snakeLen; i++) {
    snakeX[i] = startX - i;
    snakeY[i] = startY;
  }
  snakeDir = SNAKE_RIGHT;
  snakeNextDir = SNAKE_RIGHT;
  snakeGameOver = false;
  snakeScore = 0;
  snakeLastTickMs = millis();
  snakePlaceFood();
}

void drawSnakeScore() {
  tft.fillRect(0, 24, 240, 16, COLOR_BG);
  char buf[24];
  sprintf(buf, "Score: %d", snakeScore);
  tft.setTextColor(COLOR_SUBTEXT);
  tft.setTextSize(1);
  tft.setCursor(94, 30);
  tft.print(buf);
}

void drawSnakeField() {
  tft.fillRect(snakeFieldX, snakeFieldY, snakeFieldW, snakeFieldH, COLOR_SURFACE);
  tft.drawRect(snakeFieldX - 1, snakeFieldY - 1, snakeFieldW + 2, snakeFieldH + 2, COLOR_SUBTEXT);

  tft.fillRect(snakeFieldX + snakeFoodX * SNAKE_CELL + 1, snakeFieldY + snakeFoodY * SNAKE_CELL + 1,
               SNAKE_CELL - 2, SNAKE_CELL - 2, COLOR_ERROR);

  for (int i = 0; i < snakeLen; i++) {
    uint16_t c = (i == 0) ? COLOR_HOMEWORK : COLOR_ACCENT;
    tft.fillRect(snakeFieldX + snakeX[i] * SNAKE_CELL + 1, snakeFieldY + snakeY[i] * SNAKE_CELL + 1,
                 SNAKE_CELL - 2, SNAKE_CELL - 2, c);
  }
}

void drawSnakeGameOverOverlay() {
  tft.fillRoundRect(30, 120, 180, 90, 16, COLOR_ERROR);
  printCentered(&FreeSansBold12pt7b, 120, 145, "Game Over", ILI9341_WHITE);
  char buf[24];
  sprintf(buf, "Score: %d", snakeScore);
  printCentered(&FreeSans12pt7b, 120, 172, buf, ILI9341_WHITE);
  printCentered(&FreeSans12pt7b, 120, 195, "Tap field to retry", ILI9341_WHITE);
}

void drawSnakeScreen() {
  tft.fillScreen(COLOR_BG);
  drawBackBar("Snake");
  drawSnakeScore();
  drawSnakeField();
  if (!snakeGameOver) {
    printCentered(&FreeSans12pt7b, 120, snakeFieldY + snakeFieldH + 14, "Swipe field to steer", COLOR_SUBTEXT);
  } else {
    drawSnakeGameOverOverlay();
  }
}

bool touchedSnakeField(int x, int y) {
  return x >= snakeFieldX && x <= snakeFieldX + snakeFieldW && y >= snakeFieldY && y <= snakeFieldY + snakeFieldH;
}

void snakeApplyDirection(SnakeDir d) {
  // Ignore a swipe that would reverse the snake straight into its own body.
  if ((snakeDir == SNAKE_UP && d == SNAKE_DOWN) ||
      (snakeDir == SNAKE_DOWN && d == SNAKE_UP) ||
      (snakeDir == SNAKE_LEFT && d == SNAKE_RIGHT) ||
      (snakeDir == SNAKE_RIGHT && d == SNAKE_LEFT)) {
    return;
  }
  snakeNextDir = d;
}

void handleSnakeTouch(int x, int y) {
  if (snakeGameOver) {
    if (touchedSnakeField(x, y)) {
      snakeInitGame();
      drawSnakeScreen();
    }
  }
}

// Called every loop iteration while a finger is down on the Snake screen
// (not just on first touch), so it can measure how far the finger has
// dragged since it first touched down. isNewTouch is true only on the
// very first call of a given touch (finger just went down); every call
// after that, while the same finger is still down, passes false.
void handleSnakeSwipe(int x, int y, bool isNewTouch) {
  if (isNewTouch) {
    snakeSwipeStartX = x;
    snakeSwipeStartY = y;
    snakeSwipeFired = false;
    return;
  }

  if (snakeSwipeFired || snakeSwipeStartX == -1) return;

  int dx = x - snakeSwipeStartX;
  int dy = y - snakeSwipeStartY;

  if (abs(dx) < SNAKE_SWIPE_THRESHOLD && abs(dy) < SNAKE_SWIPE_THRESHOLD) return;

  if (abs(dx) > abs(dy)) {
    snakeApplyDirection(dx > 0 ? SNAKE_RIGHT : SNAKE_LEFT);
  } else {
    snakeApplyDirection(dy > 0 ? SNAKE_DOWN : SNAKE_UP);
  }
  snakeSwipeFired = true;
}

void updateSnakeGame() {
  if (snakeGameOver) return;
  if (millis() - snakeLastTickMs < SNAKE_TICK_MS) return;
  snakeLastTickMs = millis();

  snakeDir = snakeNextDir;
  int nx = snakeX[0];
  int ny = snakeY[0];
  if (snakeDir == SNAKE_UP) ny--;
  else if (snakeDir == SNAKE_DOWN) ny++;
  else if (snakeDir == SNAKE_LEFT) nx--;
  else nx++;

  bool crashed = (nx < 0 || nx >= snakeGridCols || ny < 0 || ny >= snakeGridRows);
  for (int i = 0; i < snakeLen && !crashed; i++) {
    if (snakeX[i] == nx && snakeY[i] == ny) crashed = true;
  }
  if (crashed) {
    snakeGameOver = true;
    drawSnakeGameOverOverlay();
    return;
  }

  bool ate = (nx == snakeFoodX && ny == snakeFoodY);
  if (ate && snakeLen < SNAKE_MAX_LEN - 1) snakeLen++;

  for (int i = snakeLen - 1; i > 0; i--) {
    snakeX[i] = snakeX[i - 1];
    snakeY[i] = snakeY[i - 1];
  }
  snakeX[0] = nx;
  snakeY[0] = ny;

  if (ate) {
    snakeScore++;
    snakePlaceFood();
    drawSnakeScore();
  }

  drawSnakeField();
}

// ============================================================
// SETTINGS/MORE ACTIONS -- shared by the Settings list below
// ============================================================
void performMoreAction(MoreAction action) {
  switch (action) {
    case MORE_ACTION_CLEAR_TODO:     LittleFS.remove("/todo.txt"); break;
    case MORE_ACTION_CLEAR_HOMEWORK: LittleFS.remove("/homework.txt"); break;
    case MORE_ACTION_CLEAR_NOTES:    LittleFS.remove("/note1.txt"); break;
    default: break;
  }
}

const char* moreActionLabel(MoreAction a) {
  switch (a) {
    case MORE_ACTION_CLEAR_TODO: return "the To-Do list";
    case MORE_ACTION_CLEAR_HOMEWORK: return "the Homework log";
    case MORE_ACTION_CLEAR_NOTES: return "your Notes";
    default: return "this";
  }
}

void drawMoreConfirmScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Confirm");

  char msg[48];
  sprintf(msg, "Clear %s?", moreActionLabel(pendingConfirmAction));
  printFitted(&FreeSansBold12pt7b, 120, 90, msg, COLOR_TEXT, 220);
  printCentered(&FreeSans12pt7b, 120, 130, "Cannot be undone.", COLOR_SUBTEXT);

  drawGlassRect(30, 200, 80, 44, 18, COLOR_SUBTEXT);
  printFitted(&FreeSansBold12pt7b, 70, 214, "No", ILI9341_WHITE, 70);

  drawGlassRect(130, 200, 80, 44, 18, COLOR_ERROR);
  printFitted(&FreeSansBold12pt7b, 170, 214, "Yes", ILI9341_WHITE, 70);
}

void handleMoreConfirmTouch(int x, int y) {
  if (x >= 30 && x <= 110 && y >= 200 && y <= 244) { // No
    currentScreen = SCREEN_SETTINGS;
    drawSettingsScreen();
    return;
  }
  if (x >= 130 && x <= 210 && y >= 200 && y <= 244) { // Yes
    performMoreAction(pendingConfirmAction);
    currentScreen = SCREEN_SETTINGS;
    drawSettingsScreen();
    return;
  }
}

void drawMoreAboutScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("About");

  tft.setTextColor(COLOR_TEXT);
  tft.setTextSize(1);
  tft.setCursor(20, 50);
  tft.print("Firmware: ");
  tft.print(FIRMWARE_VERSION);

  tft.setCursor(20, 80);
  tft.print("Storage: ");
  tft.setTextColor(storageOK ? COLOR_HOMEWORK : COLOR_ERROR);
  tft.print(storageOK ? "OK" : "Not Found");

  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(20, 110);
  tft.print("RTC: ");
  tft.setTextColor(rtcOK ? COLOR_HOMEWORK : COLOR_ERROR);
  tft.print(rtcOK ? "OK" : "Not Found");

  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(20, 140);
  tft.print("WiFi: ");
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  tft.setTextColor(wifiUp ? COLOR_HOMEWORK : COLOR_SUBTEXT);
  tft.print(wifiUp ? "Connected" : "Not connected");

  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(20, 200);
  tft.print("Name: ");
  tft.print(deviceName);

  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(20, 170);
  tft.print("Time sync: ");
  if (rtcEverSynced) {
    char buf[24];
    char t12[9];
    formatTime12h(t12, lastRtcSync.hour(), lastRtcSync.minute());
    snprintf(buf, sizeof(buf), "%02d/%02d %s", lastRtcSync.month(), lastRtcSync.day(), t12);
    tft.setTextColor(COLOR_HOMEWORK);
    tft.print(buf);
  } else {
    tft.setTextColor(COLOR_SUBTEXT);
    tft.print(wifiUp ? "Waiting..." : "Needs WiFi");
  }
}

// ============================================================
// UI: FIRMWARE UPDATE (over the air, from GitHub)
// ============================================================
// Flow: tap "Check" -> the PDA reads version.txt from the GitHub repo and
// compares it to FIRMWARE_VERSION. If they differ, an "Install" button
// appears. Install downloads firmware.bin, writes it to the spare app slot,
// and restarts into it. Nothing is ever installed without a tap.
// Needs a partition scheme with two app slots (see the notes that came with
// this file). Saved notes, tasks and WiFi networks are kept across updates.
void drawOtaScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Update");

  char line[40];
  snprintf(line, sizeof(line), "This PDA: %s", FIRMWARE_VERSION);
  printCentered(&FreeSans12pt7b, 120, 70, line, COLOR_TEXT);

  if (otaLatest[0]) {
    snprintf(line, sizeof(line), "On GitHub: %s", otaLatest);
    printFitted(&FreeSans12pt7b, 120, 100, line, COLOR_TEXT, 220);
  }

  const char* status = "";
  uint16_t statusColor = COLOR_SUBTEXT;
  switch (otaState) {
    case OTA_IDLE:        status = "Tap Check to look"; break;
    case OTA_CHECKING:    status = "Checking..."; break;
    case OTA_UP_TO_DATE:  status = "You are up to date"; statusColor = COLOR_HOMEWORK; break;
    case OTA_AVAILABLE:   status = "New version found!"; statusColor = COLOR_ACCENT; break;
    case OTA_DOWNLOADING: status = "Downloading..."; break;
    case OTA_FAILED:      status = otaMsg; statusColor = COLOR_ERROR; break;
    case OTA_NO_WIFI:     status = "Connect WiFi first"; statusColor = COLOR_ERROR; break;
  }
  printFitted(&FreeSansBold12pt7b, 120, 150, status, statusColor, 224);

  if (otaState == OTA_DOWNLOADING) {
    printCentered(&FreeSans12pt7b, 120, 235, "Do not turn off", COLOR_SUBTEXT);
    return;
  }

  bool install = (otaState == OTA_AVAILABLE);
  drawGlassRect(40, 200, 160, 46, 18, install ? COLOR_ACCENT : COLOR_BLUE);
  printFitted(&FreeSansBold12pt7b, 120, 214, install ? "Install" : "Check", ILI9341_WHITE, 140);
}

void drawOtaProgress(int pct) {
  tft.drawRoundRect(30, 180, 180, 18, 8, COLOR_TEXT);
  int w = (176 * pct) / 100;
  if (w > 0) tft.fillRoundRect(32, 182, w, 14, 6, COLOR_ACCENT);
}

void checkOtaVersion() {
  if (WiFi.status() != WL_CONNECTED) {
    otaState = OTA_NO_WIFI;
    drawOtaScreen();
    return;
  }

  otaState = OTA_CHECKING;
  otaLatest[0] = '\0';
  drawOtaScreen();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  // The changing number on the end stops GitHub's cache handing back an old copy.
  String url = String(OTA_VERSION_URL) + "?t=" + String(millis());
  http.begin(client, url);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    body.trim();
    if (body.length() > 0 && body.length() < (int)sizeof(otaLatest)) {
      strncpy(otaLatest, body.c_str(), sizeof(otaLatest) - 1);
      otaLatest[sizeof(otaLatest) - 1] = '\0';
      otaState = (strcmp(otaLatest, FIRMWARE_VERSION) == 0) ? OTA_UP_TO_DATE : OTA_AVAILABLE;
    } else {
      strcpy(otaMsg, "Bad version file");
      otaState = OTA_FAILED;
    }
  } else if (code == 404) {
    strcpy(otaMsg, "version.txt not found");
    otaState = OTA_FAILED;
  } else {
    snprintf(otaMsg, sizeof(otaMsg), "Check failed (%d)", code);
    otaState = OTA_FAILED;
  }
  http.end();
  drawOtaScreen();
}

void performOtaUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    otaState = OTA_NO_WIFI;
    drawOtaScreen();
    return;
  }

  otaState = OTA_DOWNLOADING;
  drawOtaScreen();
  drawOtaProgress(0);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  String url = String(OTA_FIRMWARE_URL) + "?t=" + String(millis());
  http.begin(client, url);
  int code = http.GET();

  if (code != 200) {
    snprintf(otaMsg, sizeof(otaMsg), code == 404 ? "firmware.bin not found" : "Download failed (%d)", code);
    otaState = OTA_FAILED;
    http.end();
    drawOtaScreen();
    return;
  }

  int total = http.getSize();
  if (total <= 0) {
    strcpy(otaMsg, "Unknown file size");
    otaState = OTA_FAILED;
    http.end();
    drawOtaScreen();
    return;
  }

  if (!Update.begin(total)) {
    // Almost always means the partition scheme has no second app slot.
    strcpy(otaMsg, "No room (partition?)");
    otaState = OTA_FAILED;
    http.end();
    drawOtaScreen();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int written = 0;
  int lastPct = -1;
  unsigned long lastDataMs = millis();
  bool ok = true;

  while (written < total) {
    int avail = stream->available();
    if (avail > 0) {
      int toRead = (avail > (int)sizeof(buf)) ? (int)sizeof(buf) : avail;
      if (toRead > total - written) toRead = total - written;
      int n = stream->readBytes(buf, toRead);
      if (n <= 0) { ok = false; break; }
      if (Update.write(buf, n) != (size_t)n) { ok = false; break; }
      written += n;
      lastDataMs = millis();
      int pct = (int)((long)written * 100 / total);
      if (pct != lastPct) {
        lastPct = pct;
        drawOtaProgress(pct);
      }
    } else {
      if (!http.connected() && stream->available() == 0) { ok = false; break; }
      if (millis() - lastDataMs > 20000) { ok = false; break; }
      delay(1);
    }
  }
  http.end();

  if (ok && written == total && Update.end(true) && Update.isFinished()) {
    tft.fillScreen(COLOR_BG);
    drawScreenFrame();
    printCentered(&FreeSansBold18pt7b, 120, 120, "Updated!", COLOR_ACCENT);
    printCentered(&FreeSans12pt7b, 120, 170, "Restarting...", COLOR_SUBTEXT);
    delay(1500);
    ESP.restart();
    return;
  }

  Update.abort();
  strcpy(otaMsg, "Update failed, try again");
  otaState = OTA_FAILED;
  drawOtaScreen();
}

void handleOtaTouch(int x, int y) {
  if (otaState == OTA_DOWNLOADING || otaState == OTA_CHECKING) return;
  if (x >= 40 && x <= 200 && y >= 200 && y <= 246) {
    if (otaState == OTA_AVAILABLE) performOtaUpdate();
    else checkOtaVersion();
  }
}

// ============================================================
// UI: SETTINGS -- two levels. The top level lists categories
// (Appearance, Time & Date, WiFi & Updates, Storage, System, Games);
// tapping one opens its buttons. Back goes up a level, then Home.
// ============================================================
uint16_t settingsCategoryColor(int c) {
  switch (c) {
    case 0: return COLOR_NOTES;
    case 1: return COLOR_TODO;
    case 2: return COLOR_BLUE;
    case 3: return COLOR_WEATHER;
    case 4: return COLOR_SETTINGS;
    default: return COLOR_HOMEWORK;
  }
}

const char* moreItemName(MoreAction a) {
  for (int i = 0; i < moreItemCount; i++)
    if (moreItems[i].action == a) return moreItems[i].name;
  return "";
}

uint16_t moreItemColor(MoreAction a) {
  if (a == MORE_ACTION_UPDATE) return COLOR_BLUE;
  if (a == MORE_ACTION_CLEAR_TODO || a == MORE_ACTION_CLEAR_HOMEWORK || a == MORE_ACTION_CLEAR_NOTES)
    return COLOR_ERROR;
  return COLOR_SETTINGS;
}

void drawSettingsScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();

  if (settingsCategory < 0) {
    drawBackBar("Settings");
    for (int c = 0; c < settingsCategoryCount; c++) {
      int rowY = 32 + c * 46;
      drawGlassRect(20, rowY, 200, 40, 18, settingsCategoryColor(c));
      printFitted(&FreeSansBold12pt7b, 114, rowY + 12, settingsCategories[c].name, ILI9341_WHITE, 170);
      tft.setFont();
      tft.setTextSize(2);
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(196, rowY + 13);
      tft.print(">");
      tft.setTextSize(1);
    }
    return;
  }

  SettingsCategory& cat = settingsCategories[settingsCategory];
  drawBackBar(cat.name);
  for (int i = 0; i < cat.count; i++) {
    int rowY = 40 + i * 52;
    MoreAction action = cat.items[i];
    drawGlassRect(20, rowY, 200, 44, 18, moreItemColor(action));

    char label[32];
    if (action == MORE_ACTION_ANIMATIONS) {
      snprintf(label, sizeof(label), "Animations: %s", homeAnimationsEnabled ? "On" : "Off");
    } else if (action == MORE_ACTION_WIDGET) {
      snprintf(label, sizeof(label), "Widget: %s", widgetNames[homeWidget]);
    } else if (action == MORE_ACTION_DEVICE_NAME) {
      snprintf(label, sizeof(label), "Name: %s", deviceName);
    } else if (action == MORE_ACTION_THEME) {
      snprintf(label, sizeof(label), "Theme: %s", currentTheme == THEME_CUSTOM ? "Custom" : themes[currentTheme].name);
    } else {
      snprintf(label, sizeof(label), "%s", moreItemName(action));
    }
    printFitted(&FreeSansBold12pt7b, 120, rowY + 14, label, ILI9341_WHITE, 188);
  }
}

void handleSettingsTouch(int x, int y) {
  if (settingsCategory < 0) {
    for (int c = 0; c < settingsCategoryCount; c++) {
      int rowY = 32 + c * 46;
      if (x >= 20 && x <= 220 && y >= rowY && y <= rowY + 40) {
        settingsCategory = c;
        drawSettingsScreen();
        return;
      }
    }
    return;
  }

  SettingsCategory& cat = settingsCategories[settingsCategory];
  for (int i = 0; i < cat.count; i++) {
    int rowY = 40 + i * 52;
    if (x >= 20 && x <= 220 && y >= rowY && y <= rowY + 44) {
      MoreAction action = cat.items[i];
      if (action == MORE_ACTION_SET_TIME) {
        DateTime now = rtc.now();
        editHour = now.hour();
        editMinute = now.minute();
        currentScreen = SCREEN_SET_TIME;
        drawSetTimeScreen();
      } else if (action == MORE_ACTION_SET_DATE) {
        DateTime now = rtc.now();
        editYear = now.year();
        editMonth = now.month();
        editDay = now.day();
        currentScreen = SCREEN_SET_DATE;
        drawSetDateScreen();
      } else if (action == MORE_ACTION_WIFI) {
        currentScreen = SCREEN_WIFI;
        drawWifiScreen();
      } else if (action == MORE_ACTION_ANIMATIONS) {
        homeAnimationsEnabled = !homeAnimationsEnabled;
        saveHomeAnimations(homeAnimationsEnabled);
        drawSettingsScreen();
      } else if (action == MORE_ACTION_WIDGET) {
        homeWidget = (homeWidget + 1) % WIDGET_COUNT;
        saveHomeWidget(homeWidget);
        drawSettingsScreen();
      } else if (action == MORE_ACTION_DEVICE_NAME) {
        startDeviceNameEntry();
      } else if (action == MORE_ACTION_TOUCH_CAL) {
        startTouchCalibration(SCREEN_SETTINGS);
      } else if (action == MORE_ACTION_THEME) {
        currentScreen = SCREEN_THEMES;
        drawThemesScreen();
      } else if (action == MORE_ACTION_ABOUT) {
        currentScreen = SCREEN_MORE_ABOUT;
        drawMoreAboutScreen();
      } else if (action == MORE_ACTION_UPDATE) {
        otaState = OTA_IDLE;
        otaLatest[0] = '\0';
        otaMsg[0] = '\0';
        currentScreen = SCREEN_OTA;
        drawOtaScreen();
      } else if (action == MORE_ACTION_RUN_SETUP) {
        // Test mode: re-run the welcome / touch-test / set-time / set-date
        // flow on demand, even though the RTC already has a valid time.
        firstBootFlow = true;
        touchTestResult = 0;
        currentScreen = SCREEN_WELCOME;
        drawWelcomeScreen();
      } else if (action == MORE_ACTION_SNAKE) {
        snakeInitGame();
        currentScreen = SCREEN_SNAKE;
        drawSnakeScreen();
      } else {
        pendingConfirmAction = action;
        currentScreen = SCREEN_MORE_CONFIRM;
        drawMoreConfirmScreen();
      }
      return;
    }
  }
}

// ============================================================
// UI: THEMES -- one row per theme with a little palette preview. Tapping
// a preset applies it everywhere immediately; "Custom" opens the builder.
// ============================================================
void drawThemeSwatches(int x, int y, RGB bg, RGB accent, RGB bar, RGB tile) {
  tft.fillRect(x,      y, 12, 20, rgb565(bg));
  tft.fillRect(x + 12, y, 12, 20, rgb565(bar));
  tft.fillRect(x + 24, y, 12, 20, rgb565(accent));
  tft.fillRect(x + 36, y, 12, 20, rgb565(tile));
  tft.drawRect(x - 1, y - 1, 50, 22, COLOR_SUBTEXT);
}

void drawThemesScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Theme");

  for (int i = 0; i < themeCount; i++) {
    int rowY = 30 + i * 40;
    bool selected = (i == currentTheme);
    drawGlassRect(12, rowY, 216, 34, 14, selected ? COLOR_ACCENT : COLOR_SURFACE);

    if (i == THEME_CUSTOM) {
      Theme base = themes[customBase == 1 ? 1 : 0];
      RGB a = customAccents[customAccent];
      drawThemeSwatches(22, rowY + 7, base.bg, a, darkenRGB(a, 40), base.todo);
    } else {
      drawThemeSwatches(22, rowY + 7, themes[i].bg, themes[i].accent, themes[i].statusbar, themes[i].todo);
    }

    const char* name = (i == THEME_CUSTOM) ? "Custom >" : themes[i].name;
    tft.setFont(&FreeSansBold12pt7b);
    tft.setTextSize(1);
    tft.setTextColor(selected ? ILI9341_WHITE : COLOR_TEXT);
    tft.setCursor(84, rowY + 24);
    tft.print(name);
    tft.setFont();
  }
}

void handleThemesTouch(int x, int y) {
  for (int i = 0; i < themeCount; i++) {
    int rowY = 30 + i * 40;
    if (x >= 12 && x <= 228 && y >= rowY && y <= rowY + 34) {
      if (i == THEME_CUSTOM) {
        currentTheme = THEME_CUSTOM;
        applyTheme(currentTheme);
        saveThemeSettings();
        currentScreen = SCREEN_THEME_CUSTOM;
        drawThemeCustomScreen();
      } else {
        currentTheme = i;
        applyTheme(currentTheme);
        saveThemeSettings();
        drawThemesScreen();
      }
      return;
    }
  }
}

// ============================================================
// UI: CUSTOM THEME -- pick a Light/Dark base and one of 8 accents.
// Changes apply live so you can see the result straight away.
// ============================================================
void drawThemeCustomScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Custom Theme");

  printCentered(&FreeSansBold12pt7b, 120, 36, "Base", COLOR_TEXT);
  for (int b = 0; b < 2; b++) {
    int bx = 20 + b * 104;
    bool sel = (customBase == b);
    drawGlassRect(bx, 64, 96, 40, 16, sel ? COLOR_ACCENT : COLOR_SURFACE);
    printFitted(&FreeSansBold12pt7b, bx + 48, 76, customBaseNames[b], sel ? ILI9341_WHITE : COLOR_TEXT, 88);
  }

  printCentered(&FreeSansBold12pt7b, 120, 120, "Accent", COLOR_TEXT);
  for (int i = 0; i < 8; i++) {
    int cx = 39 + (i % 4) * 54;
    int cy = 172 + (i / 4) * 54;
    tft.fillCircle(cx, cy, 20, rgb565(customAccents[i]));
    if (i == customAccent) {
      tft.drawCircle(cx, cy, 23, COLOR_TEXT);
      tft.drawCircle(cx, cy, 24, COLOR_TEXT);
    }
  }

  printCentered(&FreeSans9pt7b, 120, 290, "Changes apply instantly", COLOR_SUBTEXT);
}

void handleThemeCustomTouch(int x, int y) {
  for (int b = 0; b < 2; b++) {
    int bx = 20 + b * 104;
    if (x >= bx && x <= bx + 96 && y >= 64 && y <= 104) {
      customBase = b;
      currentTheme = THEME_CUSTOM;
      applyTheme(currentTheme);
      saveThemeSettings();
      drawThemeCustomScreen();
      return;
    }
  }
  for (int i = 0; i < 8; i++) {
    int cx = 39 + (i % 4) * 54;
    int cy = 172 + (i / 4) * 54;
    if (abs(x - cx) <= 26 && abs(y - cy) <= 26) {
      customAccent = i;
      currentTheme = THEME_CUSTOM;
      applyTheme(currentTheme);
      saveThemeSettings();
      drawThemeCustomScreen();
      return;
    }
  }
}

// ============================================================
// UI: SET TIME
// ============================================================
void drawSetTimeScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Set Time");

  char buf[9];
  formatTime12h(buf, editHour, editMinute);
  printCentered(&FreeSansBold18pt7b, 120, 65, buf, COLOR_TEXT);

  drawGlassRect(30, 110, 80, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 70, 122, "+", ILI9341_WHITE, 70);
  drawGlassRect(130, 110, 80, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 170, 122, "+", ILI9341_WHITE, 70);

  bool pm = editHour >= 12;
  drawGlassRect(70, 160, 100, 44, 18, pm ? COLOR_HOMEWORK : COLOR_WEATHER);
  printFitted(&FreeSansBold12pt7b, 120, 174, pm ? "PM" : "AM", ILI9341_WHITE, 90);

  drawGlassRect(30, 214, 80, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 70, 226, "-", ILI9341_WHITE, 70);
  drawGlassRect(130, 214, 80, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 170, 226, "-", ILI9341_WHITE, 70);

  drawGlassRect(70, 270, 100, 40, 18, COLOR_HOMEWORK);
  printFitted(&FreeSansBold12pt7b, 120, 282, "Save", ILI9341_WHITE, 90);
}

void handleSetTimeTouch(int x, int y) {
  if (x >= 30 && x <= 110 && y >= 110 && y <= 150) {
    bool pm = editHour >= 12;
    int h12 = hourTo12(editHour) % 12 + 1;
    editHour = hourTo24(h12, pm);
    drawSetTimeScreen(); return;
  }
  if (x >= 130 && x <= 210 && y >= 110 && y <= 150) {
    editMinute = (editMinute + 1) % 60;
    drawSetTimeScreen(); return;
  }
  if (x >= 70 && x <= 170 && y >= 160 && y <= 204) {
    bool pm = editHour >= 12;
    editHour = hourTo24(hourTo12(editHour), !pm);
    drawSetTimeScreen(); return;
  }
  if (x >= 30 && x <= 110 && y >= 214 && y <= 254) {
    bool pm = editHour >= 12;
    int h12 = hourTo12(editHour);
    h12 = (h12 == 1) ? 12 : h12 - 1;
    editHour = hourTo24(h12, pm);
    drawSetTimeScreen(); return;
  }
  if (x >= 130 && x <= 210 && y >= 214 && y <= 254) {
    editMinute = (editMinute + 59) % 60;
    drawSetTimeScreen(); return;
  }
  if (x >= 70 && x <= 170 && y >= 270 && y <= 310) {
    DateTime now = rtc.now();
    int yr = now.year() < 2020 ? 2026 : now.year();
    int mo = now.month() < 1 ? 1 : now.month();
    int dy = now.day() < 1 ? 1 : now.day();
    rtc.adjust(DateTime(yr, mo, dy, editHour, editMinute, 0));
    Serial.println("Time saved.");

    if (firstBootFlow) {
      DateTime updated = rtc.now();
      editYear = updated.year();
      editMonth = updated.month();
      editDay = updated.day();
      currentScreen = SCREEN_SET_DATE;
      drawSetDateScreen();
    } else {
      currentScreen = SCREEN_SETTINGS;
      drawSettingsScreen();
    }
    return;
  }
}

// ============================================================
// UI: SET DATE
// ============================================================
void drawSetDateScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Set Date");

  char buf[12];
  sprintf(buf, "%02d/%02d/%04d", editMonth, editDay, editYear);
  printCentered(&FreeSansBold18pt7b, 120, 62, buf, COLOR_TEXT);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_SUBTEXT);
  tft.setCursor(28, 118); tft.print("Month");
  tft.setCursor(108, 118); tft.print("Day");
  tft.setCursor(184, 118); tft.print("Year");

  drawGlassRect(8, 130, 64, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 40, 142, "+", ILI9341_WHITE, 56);
  drawGlassRect(88, 130, 64, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 120, 142, "+", ILI9341_WHITE, 56);
  drawGlassRect(168, 130, 64, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 200, 142, "+", ILI9341_WHITE, 56);

  drawGlassRect(8, 190, 64, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 40, 202, "-", ILI9341_WHITE, 56);
  drawGlassRect(88, 190, 64, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 120, 202, "-", ILI9341_WHITE, 56);
  drawGlassRect(168, 190, 64, 40, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 200, 202, "-", ILI9341_WHITE, 56);

  drawGlassRect(70, 250, 100, 40, 18, COLOR_HOMEWORK);
  printFitted(&FreeSansBold12pt7b, 120, 262, "Save", ILI9341_WHITE, 90);
}

void handleSetDateTouch(int x, int y) {
  if (x >= 8 && x <= 72 && y >= 130 && y <= 170) { editMonth = editMonth % 12 + 1; clampEditDay(); drawSetDateScreen(); return; }
  if (x >= 88 && x <= 152 && y >= 130 && y <= 170) { editDay = editDay % daysInMonth(editMonth, editYear) + 1; drawSetDateScreen(); return; }
  if (x >= 168 && x <= 232 && y >= 130 && y <= 170) { editYear++; clampEditDay(); drawSetDateScreen(); return; }

  if (x >= 8 && x <= 72 && y >= 190 && y <= 230) { editMonth = (editMonth == 1) ? 12 : editMonth - 1; clampEditDay(); drawSetDateScreen(); return; }
  if (x >= 88 && x <= 152 && y >= 190 && y <= 230) { editDay = (editDay == 1) ? daysInMonth(editMonth, editYear) : editDay - 1; drawSetDateScreen(); return; }
  if (x >= 168 && x <= 232 && y >= 190 && y <= 230) { if (editYear > 2000) { editYear--; clampEditDay(); } drawSetDateScreen(); return; }

  if (x >= 70 && x <= 170 && y >= 250 && y <= 290) {
    DateTime now = rtc.now();
    rtc.adjust(DateTime(editYear, editMonth, editDay, now.hour(), now.minute(), now.second()));
    Serial.println("Date saved.");

    if (firstBootFlow) {
      firstBootFlow = false;
      currentScreen = SCREEN_HOME;
      DateTime updated = rtc.now();
      drawHomeScreen(updated);
      lastDrawnMinute = updated.minute();
    } else {
      currentScreen = SCREEN_SETTINGS;
      drawSettingsScreen();
    }
    return;
  }
}

// ============================================================
// UI: TOUCH CALIBRATION -- tap three crosshairs (top-left, top-right,
// bottom-left). Uses raw touch readings, so it works even when touch is
// currently pointing the wrong way.
// ============================================================
int touchCalStep = 0;           // 0-2 = waiting for crosshair n, 3 = done, 4 = failed
TS_Point touchCalRaw[3];
Screen touchCalReturn = SCREEN_HOME;

const int calTargetX[3] = {CAL_X0, CAL_X1, CAL_X0};
const int calTargetY[3] = {CAL_Y0, CAL_Y0, CAL_Y1};

void drawCrosshair(int x, int y, uint16_t c) {
  tft.drawLine(x - 12, y, x + 12, y, c);
  tft.drawLine(x, y - 12, x, y + 12, c);
  tft.drawCircle(x, y, 7, c);
}

void drawTouchCalScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();

  if (touchCalStep < 3) {
    printCentered(&FreeSansBold12pt7b, 120, 120, "Calibrate Touch", COLOR_TEXT);
    char buf[32];
    snprintf(buf, sizeof(buf), "Tap the target (%d of 3)", touchCalStep + 1);
    printCentered(&FreeSans9pt7b, 120, 156, buf, COLOR_SUBTEXT);
    printCentered(&FreeSans9pt7b, 120, 180, "Use a fingernail or stylus", COLOR_SUBTEXT);
    drawCrosshair(calTargetX[touchCalStep], calTargetY[touchCalStep], COLOR_ERROR);
    return;
  }

  if (touchCalStep == 3) {
    printCentered(&FreeSansBold12pt7b, 120, 60, "Touch calibrated!", COLOR_HOMEWORK);
    printCentered(&FreeSans9pt7b, 120, 96, "Tap around to test it", COLOR_SUBTEXT);
  } else {
    printCentered(&FreeSansBold12pt7b, 120, 60, "That didn't work", COLOR_ERROR);
    printCentered(&FreeSans9pt7b, 120, 96, "Old touch settings kept", COLOR_SUBTEXT);
  }
  drawGlassRect(20, 250, 96, 44, 16, COLOR_SETTINGS);
  printFitted(&FreeSansBold12pt7b, 68, 264, "Redo", ILI9341_WHITE, 86);
  drawGlassRect(124, 250, 96, 44, 16, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 172, 264, "Done", ILI9341_WHITE, 86);
}

void startTouchCalibration(Screen returnTo) {
  touchCalReturn = returnTo;
  touchCalStep = 0;
  currentScreen = SCREEN_TOUCH_CAL;
  drawTouchCalScreen();
}

void leaveTouchCalibration() {
  if (touchCalReturn == SCREEN_SETTINGS) {
    currentScreen = SCREEN_SETTINGS;
    drawSettingsScreen();
  } else {
    currentScreen = SCREEN_HOME;
    drawHomeScreen(rtc.now());
    lastDrawnMinute = rtc.now().minute();
  }
}

// Average a few raw readings while the finger is down.
TS_Point readRawTouchAveraged() {
  long sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < 8 && ts.touched(); i++) {
    TS_Point p = ts.getPoint();
    sx += p.x; sy += p.y; n++;
    delay(5);
  }
  TS_Point out;
  if (n == 0) { out = ts.getPoint(); return out; }
  out.x = sx / n; out.y = sy / n; out.z = 1;
  return out;
}

void finishTouchCalibration() {
  TS_Point a = touchCalRaw[0], b = touchCalRaw[1], c = touchCalRaw[2];
  // a -> b moves only across the screen; whichever raw axis changed most
  // there is the screen's X axis.
  bool swap = abs(b.y - a.y) > abs(b.x - a.x);
  int x0 = swap ? a.y : a.x, x1 = swap ? b.y : b.x;
  int y0 = swap ? a.x : a.y, y1 = swap ? c.x : c.y;
  if (abs(x1 - x0) < 300 || abs(y1 - y0) < 300) {
    touchCalStep = 4;
    return;
  }
  touchCalSwap = swap;
  touchCalX0 = x0; touchCalX1 = x1; touchCalY0 = y0; touchCalY1 = y1;
  touchCalRotation = displayRotation;
  touchCalValid = true;
  saveTouchCal();
  touchCalStep = 3;
}

void handleTouchCalTouch(bool justTouched, bool touchedNow) {
  if (touchCalStep < 3) {
    if (!justTouched) return;
    touchCalRaw[touchCalStep] = readRawTouchAveraged();
    touchCalStep++;
    if (touchCalStep == 3) finishTouchCalibration();
    drawTouchCalScreen();
    return;
  }
  if (!touchedNow) return;
  TS_Point p = ts.getPoint();
  int x, y;
  mapTouchXY(p, x, y);
  if (justTouched && y >= 250 && y <= 294) {
    if (x >= 20 && x <= 116) { touchCalStep = 0; drawTouchCalScreen(); return; }
    if (x >= 124 && x <= 220) { leaveTouchCalibration(); return; }
  }
  if (y > 110 && y < 244) tft.fillCircle(x, y, 2, COLOR_ACCENT);   // test dots
}

// ============================================================
// SETUP + MAIN LOOP
// ============================================================
void setup() {
  Serial.begin(115200);

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(DISPLAY_ROTATION);

  ts.begin();
  ts.setRotation(0);

  Wire.begin(RTC_SDA, RTC_SCL);
  rtcOK = rtc.begin();
  if (!rtcOK) {
    Serial.println("Couldn't find RTC! Check wiring.");
  }
  delay(200);

  storageOK = LittleFS.begin(true);
  if (!storageOK) {
    Serial.println("Storage not found! Check Tools > Partition Scheme includes storage space.");
  } else {
    Serial.println("Storage ready.");
  }

  displayRotation = loadDisplayRotation();
  homeAnimationsEnabled = loadHomeAnimations();
  homeWidget = loadHomeWidget();
  loadDeviceName();
  loadTouchCal();
  tft.setRotation(displayRotation);

  char hostname[33];
  buildHostname(hostname, sizeof(hostname));
  WiFi.setHostname(hostname);
  WiFi.mode(WIFI_STA);
  startTimeSync();

  loadThemeSettings();
  applyTheme(currentTheme);

  connectToBestSavedNetwork();

  server.on("/", handleWebRoot);
  server.on("/addtodo", handleWebAddTodo);
  server.on("/addhomework", handleWebAddHomework);
  server.on("/deletetodo", handleWebDeleteTodo);
  server.on("/deletehomework", handleWebDeleteHomework);
  server.on("/calibrate", handleWebCalibrate);
  server.on("/update", handleWebUpdate);
  server.on("/restart", handleWebRestart);
  server.begin();

  DateTime now = rtc.now();
  if (now.year() < 2020) {
    firstBootFlow = true;
    touchTestResult = 0;
    currentScreen = SCREEN_WELCOME;
    drawWelcomeScreen();
  } else {
    currentScreen = SCREEN_LOCK;
    drawLockScreen(now);
  }
  lastDrawnMinute = now.minute();
}

// Touch coordinates need to flip along with the screen when "Flip Screen"
// is used on the Touch Check screen -- otherwise the picture rotates but
// the touch panel keeps reading in the old orientation. displayRotation
// only ever toggles between DISPLAY_ROTATION and its +2 (180-degree)
// partner, so a simple coordinate flip covers both states.
void mapTouchXY(TS_Point p, int &tx, int &ty) {
  if (touchCalValid) {
    int rx = touchCalSwap ? p.y : p.x;
    int ry = touchCalSwap ? p.x : p.y;
    tx = CAL_X0 + (long)(rx - touchCalX0) * (CAL_X1 - CAL_X0) / (touchCalX1 - touchCalX0);
    ty = CAL_Y0 + (long)(ry - touchCalY0) * (CAL_Y1 - CAL_Y0) / (touchCalY1 - touchCalY0);
    // calibrated with the picture one way up; if it has since been flipped,
    // flip the touch to match
    if (displayRotation != touchCalRotation) {
      tx = 239 - tx;
      ty = 319 - ty;
    }
    tx = constrain(tx, 0, 239);
    ty = constrain(ty, 0, 319);
    return;
  }
  int rawX = map(p.x, 200, 3800, 0, 240);
  int rawY = map(p.y, 200, 3800, 0, 320);
  if (displayRotation == DISPLAY_ROTATION) {
    tx = rawX;
    ty = rawY;
  } else {
    tx = 240 - rawX;
    ty = 320 - rawY;
  }
}

void loop() {
  DateTime now = rtc.now();

  server.handleClient();

  bool touchedNow = ts.touched();
  bool justTouched = touchedNow && !wasTouched;

  int touchX = -1, touchY = -1;
  if (justTouched) {
    TS_Point p = ts.getPoint();
    mapTouchXY(p, touchX, touchY);
  }

  if (currentScreen == SCREEN_NOTES) {
    if (touchedNow) {
      TS_Point p = ts.getPoint();
      int nx, ny;
      mapTouchXY(p, nx, ny);
      handleNotesDraw(nx, ny, justTouched);
    } else {
      notesPrevX = -1;
      notesPrevY = -1;
    }
  }

  if (currentScreen == SCREEN_HOME) {
    updateHomeAnimations();
    updateClockWidget(now);
  }

  maybeReinitDisplay(now);
  updateIpLine();
  applyNtpToRtc();

  // Emergency: hold anywhere for 6 seconds to recalibrate touch (not while
  // drawing a note or playing Snake, where long presses are normal).
  if (touchedNow && currentScreen != SCREEN_NOTES && currentScreen != SCREEN_SNAKE && currentScreen != SCREEN_TOUCH_CAL) {
    if (touchHoldStartMs == 0) touchHoldStartMs = millis();
    else if (millis() - touchHoldStartMs > 6000) {
      touchHoldStartMs = 0;
      startTouchCalibration(SCREEN_HOME);
    }
  } else if (!touchedNow) {
    touchHoldStartMs = 0;
  }

  runWebRequests();

  wasTouched = touchedNow;

  if (wifiConnState == WIFI_CONN_CONNECTING && millis() - lastWifiCheckMs >= 500) {
    lastWifiCheckMs = millis();
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      sprintf(wifiStatusMsg, "%s (%d.%d.%d.%d)", wifiSelectedSSID, ip[0], ip[1], ip[2], ip[3]);
      wifiConnState = WIFI_CONN_CONNECTED;
      if (currentScreen == SCREEN_WIFI) drawWifiScreen();
    } else if (millis() - wifiConnectStartMs > 10000) {
      wifiConnState = WIFI_CONN_FAILED;
      if (currentScreen == SCREEN_WIFI) drawWifiScreen();
    }
  }

  switch (currentScreen) {
    case SCREEN_WELCOME:
      if (justTouched) handleWelcomeTouch(touchX, touchY);
      break;

    case SCREEN_TOUCH_TEST:
      if (justTouched) handleTouchTestTouch(touchX, touchY);
      break;

    case SCREEN_LOCK:
      if (now.minute() != lastDrawnMinute) {
        lastDrawnMinute = now.minute();
        drawLockScreen(now);
      }
      if (justTouched) {
        currentScreen = SCREEN_HOME;
        drawHomeScreen(now);
        lastDrawnMinute = now.minute();
      }
      break;

    case SCREEN_HOME:
      if (now.minute() != lastDrawnMinute) {
        lastDrawnMinute = now.minute();
        drawHomeScreen(now);
      }
      if (justTouched) handleHomeTouch(touchX, touchY);
      break;

    case SCREEN_TODO:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_HOME;
          drawHomeScreen(now);
          lastDrawnMinute = now.minute();
        } else if (touchedAddButton(touchX, touchY)) {
          kbBuffer[0] = '\0';
          kbBufferLen = 0;
          kbMode = KB_MODE_ABC;
          kbShift = false;
          kbContext = KB_CONTEXT_ADD_TODO;
          currentScreen = SCREEN_KEYBOARD;
          drawKeyboardScreen();
        } else {
          int chkIdx = touchedTodoCheckbox(touchX, touchY);
          if (chkIdx != -1) {
            toggleTodoLine(todoLines[chkIdx]);
            drawTodoScreen();
          } else {
            int delIdx = touchedTodoDelete(touchX, touchY);
            if (delIdx != -1) {
              deleteLineFromFile("/todo.txt", todoLines[delIdx]);
              drawTodoScreen();
            }
          }
        }
      }
      break;

    case SCREEN_HOMEWORK:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_HOME;
          drawHomeScreen(now);
          lastDrawnMinute = now.minute();
        } else if (touchedAddButton(touchX, touchY)) {
          kbBuffer[0] = '\0';
          kbBufferLen = 0;
          kbMode = KB_MODE_ABC;
          kbShift = false;
          kbContext = KB_CONTEXT_ADD_HOMEWORK;
          currentScreen = SCREEN_KEYBOARD;
          drawKeyboardScreen();
        } else {
          int delIdx = touchedHomeworkDelete(touchX, touchY);
          if (delIdx != -1) {
            deleteLineFromFile("/homework.txt", hwLines[delIdx]);
            drawHomeworkScreen();
          }
        }
      }
      break;

    case SCREEN_KEYBOARD:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          if (kbContext == KB_CONTEXT_ADD_HOMEWORK) {
            currentScreen = SCREEN_HOMEWORK;
            drawHomeworkScreen();
          } else if (kbContext == KB_CONTEXT_WIFI_PASSWORD) {
            currentScreen = SCREEN_WIFI;
            drawWifiScreen();
          } else if (kbContext == KB_CONTEXT_DEVICE_NAME) {
            leaveDeviceNameEntry();
          } else {
            currentScreen = SCREEN_TODO;
            drawTodoScreen();
          }
        } else {
          handleKeyboardTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_NOTES:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_HOME;
          drawHomeScreen(now);
          lastDrawnMinute = now.minute();
        } else if (touchedNotesClear(touchX, touchY)) {
          drawNotesCanvasArea();
          notesPrevX = -1;
          notesPrevY = -1;
          notePointCount = 0;
        } else if (touchedNotesSave(touchX, touchY)) {
          drawNotesSaveButton(true);
          delay(100);
          saveNotesToFile();
          drawNotesSaveButton(false);
        }
      }
      break;

    case SCREEN_WIFI:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_SETTINGS;
          drawSettingsScreen();
        } else {
          handleWifiTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_WIFI_SAVED:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_WIFI;
          drawWifiScreen();
        } else {
          handleWifiSavedTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_WEATHER:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_HOME;
          drawHomeScreen(now);
          lastDrawnMinute = now.minute();
        } else if (touchedWeatherRefresh(touchX, touchY)) {
          fetchWeather();
        }
      }
      break;

    case SCREEN_MORE_CONFIRM:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_SETTINGS;
          drawSettingsScreen();
        } else {
          handleMoreConfirmTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_MORE_ABOUT:
      if (justTouched && touchedBack(touchX, touchY)) {
        currentScreen = SCREEN_SETTINGS;
        drawSettingsScreen();
      }
      break;

    case SCREEN_TOUCH_CAL:
      handleTouchCalTouch(justTouched, touchedNow);
      break;

    case SCREEN_THEMES:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_SETTINGS;
          drawSettingsScreen();
        } else {
          handleThemesTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_THEME_CUSTOM:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_THEMES;
          drawThemesScreen();
        } else {
          handleThemeCustomTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_SETTINGS:
      if (justTouched) {
        if (touchedBack(touchX, touchY) && settingsCategory >= 0) {
          settingsCategory = -1;
          drawSettingsScreen();
        } else if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_HOME;
          drawHomeScreen(now);
          lastDrawnMinute = now.minute();
        } else {
          handleSettingsTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_OTA:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_SETTINGS;
          drawSettingsScreen();
        } else {
          handleOtaTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_HW_PERIOD:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_HOMEWORK;
          drawHomeworkScreen();
        } else {
          handleHwPeriodTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_SET_TIME:
      if (justTouched) {
        if (touchedBack(touchX, touchY) && !firstBootFlow) {
          currentScreen = SCREEN_SETTINGS;
          drawSettingsScreen();
        } else if (!touchedBack(touchX, touchY)) {
          handleSetTimeTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_SET_DATE:
      if (justTouched) {
        if (touchedBack(touchX, touchY) && !firstBootFlow) {
          currentScreen = SCREEN_SETTINGS;
          drawSettingsScreen();
        } else if (!touchedBack(touchX, touchY)) {
          handleSetDateTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_SNAKE:
      if (justTouched && touchedBack(touchX, touchY)) {
        currentScreen = SCREEN_SETTINGS;
        drawSettingsScreen();
      } else if (snakeGameOver) {
        if (justTouched) handleSnakeTouch(touchX, touchY);
      } else if (touchedNow) {
        TS_Point p = ts.getPoint();
        int nx, ny;
        mapTouchXY(p, nx, ny);
        handleSnakeSwipe(nx, ny, justTouched);
      } else {
        snakeSwipeStartX = -1;
        snakeSwipeStartY = -1;
        snakeSwipeFired = false;
      }
      if (currentScreen == SCREEN_SNAKE) updateSnakeGame();
      break;

    case SCREEN_APP:
      break;
  }

  // Was delay(30) -- that alone was 30ms of dead time on every single pass
  // through loop(), capping everything (touch response, animation ticks,
  // Snake movement) at ~33 updates/sec no matter how fast the chip could
  // otherwise go. Dropped way down so the loop runs close to full speed;
  // a tiny delay is kept just to be a good citizen to the WiFi/RTC stack.
  delay(2);
}