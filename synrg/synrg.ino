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
#include <WiFiUdp.h>
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
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include "SynFonts.h"

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
// Messages types (see the MESSAGES block further down) -- up here so the
// Arduino IDE's auto-generated prototypes can see them.
#define DEVICE_NAME_MAX 20
#define MSG_TEXT_MAX 60
#define BUBBLE_CHARS 24        // characters per line in a chat bubble
#define BUBBLE_MAX_LINES 5
// A pressable button on screen (see drawGlassRect / flashPressedButton).
struct ButtonRect { int16_t x, y, w, h, r; uint16_t color; };
struct Contact { char name[DEVICE_NAME_MAX + 1]; char ip[16]; int unread; bool rtx; };   // rtx: they are in Jensen mode
struct MsgLine { char dir; char ip[16]; char when[9]; char text[MSG_TEXT_MAX + 1]; };

enum KeyboardContext { KB_CONTEXT_NONE, KB_CONTEXT_ADD_TODO, KB_CONTEXT_ADD_HOMEWORK, KB_CONTEXT_WIFI_PASSWORD, KB_CONTEXT_DEVICE_NAME, KB_CONTEXT_MSG_IP };
enum KeyboardMode { KB_MODE_ABC, KB_MODE_123, KB_MODE_SYM };
enum Screen { SCREEN_WELCOME, SCREEN_TOUCH_TEST, SCREEN_LOCK, SCREEN_HOME, SCREEN_APP, SCREEN_SETTINGS, SCREEN_SET_TIME, SCREEN_SET_DATE, SCREEN_TODO, SCREEN_HOMEWORK, SCREEN_KEYBOARD, SCREEN_NOTES, SCREEN_WIFI, SCREEN_WIFI_SAVED, SCREEN_WEATHER, SCREEN_MORE_CONFIRM, SCREEN_MORE_ABOUT, SCREEN_SNAKE, SCREEN_HW_PERIOD, SCREEN_OTA, SCREEN_THEMES, SCREEN_THEME_CUSTOM, SCREEN_TOUCH_CAL, SCREEN_MESSAGES, SCREEN_CHAT, SCREEN_MSG_REQUEST, SCREEN_NEARBY, SCREEN_NEWS, SCREEN_ARTICLE };
enum SnakeDir { SNAKE_UP, SNAKE_DOWN, SNAKE_LEFT, SNAKE_RIGHT };
enum MoreAction { MORE_ACTION_SET_TIME, MORE_ACTION_SET_DATE, MORE_ACTION_WIFI, MORE_ACTION_ABOUT, MORE_ACTION_UPDATE, MORE_ACTION_CLEAR_TODO, MORE_ACTION_CLEAR_HOMEWORK, MORE_ACTION_CLEAR_NOTES, MORE_ACTION_RUN_SETUP, MORE_ACTION_SNAKE, MORE_ACTION_ANIMATIONS, MORE_ACTION_WIDGET, MORE_ACTION_THEME, MORE_ACTION_DEVICE_NAME, MORE_ACTION_TOUCH_CAL };

// Theme palette types (see the THEMES block further down).
struct RGB { uint8_t r, g, b; };
struct Theme {
  const char* name;
  RGB bg, text, subtext, accent, statusbar, error, frame, shadow, surface;
  RGB todo, notes, homework, weather, settings, more;
};

#define FIRMWARE_VERSION "v3.8"

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
// "theme,customBase,customAccent,2" so it survives a power cycle. The
// trailing 2 marks the numbering used since v3.4 (when OLED was added in
// front of Custom); files without it are converted once on load. ----
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
  {"OLED",     {0,0,0},       {245,245,247}, {142,142,147}, {10,132,255},  {22,22,24},    {255,69,58},  {44,44,48},    {0,0,0},       {24,24,27},
               {10,132,255},  {191,90,242},  {48,209,88},   {255,159,10},  {99,99,102},   {255,55,95}},
};
const int THEME_OLED = 6;                    // position of "OLED" above; the default theme
const int presetThemeCount = sizeof(themes) / sizeof(themes[0]);
const int THEME_CUSTOM = presetThemeCount;   // index just past the presets
const int themeCount = presetThemeCount + 1;

// ---- Secret theme: a black-and-green tribute to Jensen Huang. It stays
// hidden until the firmware line on the About screen is tapped 7 times;
// after that it shows up at the bottom of the Theme list. Kept outside
// themes[] so the saved numbers of the normal themes never change. ----
const int THEME_SECRET = themeCount;         // index just past Custom
const Theme secretTheme =
  {"Jensen",   {8,10,8},      {236,240,232}, {150,165,140}, {118,185,0},   {34,54,0},     {230,80,80},  {52,66,40},    {0,0,0},       {26,30,24},
               {118,185,0},   {64,104,0},    {92,150,16},   {150,196,48},  {66,74,62},    {44,72,6}};
bool secretUnlocked = false;
int secretTapCount = 0;

void loadSecretUnlocked() { secretUnlocked = LittleFS.exists("/jensen.txt"); }
void saveSecretUnlocked() {
  File f = LittleFS.open("/jensen.txt", FILE_WRITE);
  if (f) { f.print(1); f.close(); }
}
int shownThemeCount() { return secretUnlocked ? themeCount + 1 : themeCount; }

const char* customBaseNames[2] = {"Light", "Dark"};
const RGB customAccents[8] = {
  {110,175,100}, {90,150,220}, {0,170,170}, {150,110,210},
  {230,110,160}, {230,90,80},  {240,150,60}, {200,170,40},
};

int currentTheme = THEME_OLED;
int customBase = 0;     // 0 = Light (Nook-based), 1 = Dark (Midnight-based)
int customAccent = 0;   // index into customAccents

void loadThemeSettings() {
  if (!LittleFS.exists("/theme.txt")) return;
  File f = LittleFS.open("/theme.txt");
  if (!f) return;
  char buf[24];
  int i = 0;
  while (f.available() && i < 23) buf[i++] = f.read();
  buf[i] = '\0';
  f.close();
  int t = 0, b = 0, a = 0, ver = 0;
  int got = sscanf(buf, "%d,%d,%d,%d", &t, &b, &a, &ver);
  loadSecretUnlocked();
  customBase = (b == 1) ? 1 : 0;
  customAccent = (a >= 0 && a < 8) ? a : 0;
  if (got < 4 || ver < 2) {
    // Saved before v3.4: the device moves to the new OLED look once. The old
    // choice (and any Custom colors) is still in the Theme list.
    currentTheme = THEME_OLED;
    saveThemeSettings();
    return;
  }
  currentTheme = (t >= 0 && t < shownThemeCount()) ? t : THEME_OLED;
}

// ---- Device name: chosen during first-time setup (or Settings > System >
// Device Name). Shown on the web page and used as the WiFi hostname.
// Saved to /name.txt. ----
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
  if (f) { f.printf("%d,%d,%d,2", currentTheme, customBase, customAccent); f.close(); }
}

// ---- The display. A thin layer over the ILI9341 driver that replaces the
// blocky built-in 6x8 font with SynSmall everywhere: code that asks for the
// "default" font (tft.setFont() with no argument) gets SynSmall at size 1
// and FreeSansBold9pt at size 2+, and setCursor() keeps meaning "top-left
// of the text" for those, exactly like the built-in font did. It also
// forgets the pressable-button list whenever the screen is wiped. ----
#define MAX_BUTTON_RECTS 72
ButtonRect buttonRects[MAX_BUTTON_RECTS];
int buttonRectCount = 0;

class SynDisplay : public Adafruit_ILI9341 {
  bool smallMode = true;
  uint8_t wantedSize = 1;
  int8_t yOff = 8;
  void applySmall() {
    Adafruit_GFX::setTextSize(1);
    if (wantedSize >= 2) { Adafruit_GFX::setFont(&FreeSansBold9pt7b); yOff = 13; }
    else { Adafruit_GFX::setFont(&SynSmall); yOff = 8; }
  }
 public:
  using Adafruit_ILI9341::Adafruit_ILI9341;
  void setFont(const GFXfont* f = NULL) {
    if (f) { smallMode = false; yOff = 0; Adafruit_GFX::setFont(f); Adafruit_GFX::setTextSize(wantedSize); }
    else { smallMode = true; applySmall(); }
  }
  void setTextSize(uint8_t sz) {
    wantedSize = sz;
    if (smallMode) applySmall(); else Adafruit_GFX::setTextSize(sz);
  }
  void setCursor(int16_t x, int16_t y) { Adafruit_GFX::setCursor(x, y + yOff); }
  void fillScreen(uint16_t color) { buttonRectCount = 0; Adafruit_ILI9341::fillScreen(color); }
};
SynDisplay tft(&SPI, TFT_DC, TFT_CS, TFT_RST);
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
uint16_t tileColors[7];

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
  } else if (idx == THEME_SECRET) {
    t = secretTheme;
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
  COLOR_BLUE      = (idx == THEME_SECRET) ? tft.color565(0, 150, 96)
                  : (idx == THEME_OLED) ? tft.color565(48, 176, 199)      // teal, so it differs from To-Do's blue
                  : tft.color565(90, 150, 220);
  COLOR_GRAY      = tft.color565(170, 170, 170);

  tileColors[0] = COLOR_MORE;      // widget tile background
  tileColors[1] = COLOR_TODO;
  tileColors[2] = COLOR_NOTES;
  tileColors[3] = COLOR_HOMEWORK;
  tileColors[4] = COLOR_WEATHER;
  tileColors[5] = COLOR_SETTINGS;
  tileColors[6] = COLOR_BLUE;      // Messages
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

// ---- Clock source. Everything reads the time through clockNow(), which
// uses the ESP32's own clock once it has been set (from internet time, or
// from the RTC at boot) because that always ticks. The RTC chip is the
// backup that keeps time while the PDA is off. If the RTC itself has
// stopped counting, About shows "Not ticking". ----
bool rtcOK = false;
bool rtcTicking = true;
uint32_t rtcCheckUnix = 0;
unsigned long rtcCheckMs = 0;
unsigned long lastNtpSyncMs = 0;

bool systemTimeValid() { return time(nullptr) > 1704067200; }   // after 2024-01-01

DateTime clockNow() {
  if (systemTimeValid()) {
    time_t n = time(nullptr);
    struct tm t;
    localtime_r(&n, &t);
    return DateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  }
  return rtc.now();
}

void setSystemTime(const DateTime& dt) {
  struct tm t = {};
  t.tm_year = dt.year() - 1900;
  t.tm_mon = dt.month() - 1;
  t.tm_mday = dt.day();
  t.tm_hour = dt.hour();
  t.tm_min = dt.minute();
  t.tm_sec = dt.second();
  t.tm_isdst = -1;                     // let the time zone rules decide DST
  struct timeval tv = { mktime(&t), 0 };
  settimeofday(&tv, nullptr);
}

// Used when the time is set by hand: update both clocks.
void setClock(const DateTime& dt) {
  rtc.adjust(dt);
  setSystemTime(dt);
  rtcCheckMs = 0;   // restart the RTC health check from the new time
  lastDrawnMinute = -1;
}

// At boot: make sure the RTC is counting, and start the ESP32 clock from it.
void initClockFromRtc() {
  if (!rtcOK) return;
  rtc.start();                         // clears the chip's STOP bit if it was set
  DateTime r = rtc.now();
  if (r.year() >= 2024 && r.year() < 2100) setSystemTime(r);
}

// Once a minute: check that the RTC is actually counting, and if internet
// time hasn't refreshed in 2 hours, re-align the ESP32 clock to the RTC.
void maintainClock() {
  if (!rtcOK) return;
  if (rtcCheckMs != 0 && millis() - rtcCheckMs < 60000) return;
  DateTime r = rtc.now();
  if (rtcCheckMs != 0) {
    long rtcDelta = (long)(r.unixtime() - rtcCheckUnix);
    long msDelta = (long)((millis() - rtcCheckMs) / 1000);
    rtcTicking = labs(rtcDelta - msDelta) <= 5;
    bool ntpStale = (lastNtpSyncMs == 0 || millis() - lastNtpSyncMs > 2UL * 3600UL * 1000UL);
    if (rtcTicking && ntpStale && r.year() >= 2024) setSystemTime(r);
  }
  rtcCheckUnix = r.unixtime();
  rtcCheckMs = millis();
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
  rtcCheckMs = 0;
  lastNtpSyncMs = millis();
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

// ---- Onscreen keyboard state (shared widget) ----
KeyboardContext kbContext = KB_CONTEXT_NONE;
// Three pages: letters, numbers + common symbols, and fancy symbols.
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
char weatherStatus[32] = "";          // note under the reading when a manual Refresh fails
unsigned long weatherNextTryMs = 0;   // when the weather may be auto-synced again (0 = as soon as possible)
int weatherCode = -1;                 // Open-Meteo weather code of the last fetch, -1 = none
int weatherHigh = -999, weatherLow = -999;   // today's high / low in F, -999 = unknown

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
// The wide widget sits under the status bar; below it are six app tiles in
// two columns. Tile 0 is the widget (it has its own WIDGET_* box).
const int WIDGET_X = 14, WIDGET_Y = 30, WIDGET_W = 212, WIDGET_H = 66;
const int tileW = 104, tileH = 56;
const int col0 = 14, col1 = 122;
const int row0 = 104, row1 = 166, row2 = 228;
#define TILE_COUNT 7
#define TILE_MESSAGES 6
#define TILE_ICON_R 15
const char* tileNames[TILE_COUNT] = {"", "To-Do", "Notes", "Homework", "Weather", "Settings", "ShareNote"};
int tileX[TILE_COUNT] = {WIDGET_X, col0, col1, col0, col1, col1, col0};
int tileY[TILE_COUNT] = {WIDGET_Y, row0, row0, row1, row1, row2, row2};

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
  DateTime now = clockNow();
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
  html += "<header><h1>" + name + "</h1><p id='clk'>" + String(timeStr) + "</p></header>";
  // Live clock: starts from the PDA's time, ticks every second in the
  // browser, and re-checks the PDA every 30 seconds.
  long secs = (long)now.hour() * 3600 + now.minute() * 60 + now.second();
  html += "<script>var t=" + String(secs) + ";";
  html += "function f(){var h=Math.floor(t/3600)%24,m=Math.floor(t/60)%60,s=t%60;";
  html += "document.getElementById('clk').textContent=(h%12||12)+':'+(m<10?'0':'')+m+':'+(s<10?'0':'')+s+(h<12?' AM':' PM');}";
  html += "setInterval(function(){t=(t+1)%86400;f();},1000);";
  html += "setInterval(function(){fetch('/time').then(function(r){return r.text();}).then(function(x){var v=parseInt(x);if(!isNaN(v)){t=v;f();}}).catch(function(){});},30000);f();</script>";

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
    DateTime now = clockNow();
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

// ============================================================
// MESSAGES -- AirDrop-style chat between SynRGs on the same WiFi.
//  * Messaging is off until you switch it on; while off, nobody can reach you.
//  * New Chat: type the other PDA's IP (shown on its Home/Lock screen). It
//    gets a pop-up "<name> wants to connect" with Accept / Decline.
//  * On Accept both PDAs save each other in Contacts (name + IP), and from
//    then on can text each other. Only contacts can send you messages.
// Wire protocol (plain HTTP on port 80):
//   /msg/request?from=NAME        ask to connect
//   /msg/accept?from=NAME         the other side said yes
//   /msg/send?from=NAME&text=TXT  a message (contacts only; 410 = not a contact)
// Finding people: "Find Nearby" broadcasts on UDP port 4210 and lists the
// SynRGs that answer (only those with Messaging on); or type an IP.
// Files: /msgon.txt (1/0), /contacts.txt (name|ip), /messages.txt
// (dir|ip|time|text, newest MSG_KEEP kept; dir i = received, o = sent,
// f = failed to send, tap to retry).
// ============================================================
#define MSG_KEEP 60
#define MAX_CONTACTS 8

bool messagingOn = false;

Contact contacts[MAX_CONTACTS];
int contactCount = 0;

MsgLine msgList[MSG_KEEP];
int msgCount = 0;

bool chatConfirmDelete = false;   // "Delete this contact?" pop-up showing
int chatContact = -1;              // contact open in the chat screen
char msgStatus[40] = "";
char msgOutgoingIp[16] = "";       // we asked this IP to connect, waiting for Accept
char newChatIp[16] = "";

// incoming connect request waiting for Accept/Decline
bool reqPending = false;
char reqName[DEVICE_NAME_MAX + 1] = "";
char reqIp[16] = "";
bool reqRtx = false;               // the PDA asking to connect is in Jensen mode
Screen reqReturnScreen = SCREEN_HOME;

// one-line notice shown on Home ("Hound Dog accepted", "New message from ...")
char msgBanner[40] = "";
bool newsTickerDirty = true;       // the Home news ticker text needs rebuilding

// chat screen state
bool chatTyping = false;           // keyboard open under the conversation
int chatScroll = 0;                // how many of the newest messages are scrolled past
bool chatCanScrollUp = false;
int contactScroll = 0;
#define CHAT_HIT_MAX 12
int chatHitMsg[CHAT_HIT_MAX], chatHitY0[CHAT_HIT_MAX], chatHitY1[CHAT_HIT_MAX];
int chatHitCount = 0;

// pop-down notification banner
bool bannerActive = false;
unsigned long bannerUntilMs = 0;
char bannerIp[16] = "";

// nearby discovery
#define DISCOVERY_PORT 4210
#define MAX_NEARBY 6
WiFiUDP discoveryUdp;
bool discoveryUdpOn = false;
Contact nearby[MAX_NEARBY];
int nearbyCount = 0;
bool nearbySearching = false;

int totalUnread() {
  int n = 0;
  for (int i = 0; i < contactCount; i++) n += contacts[i].unread;
  return n;
}

// Strip characters that would break the one-line-per-entry files.
void cleanMsgField(char* out, size_t len, const char* in) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j < len - 1; i++) {
    char c = in[i];
    if (c == '|' || c == '\n' || c == '\r') c = ' ';
    if ((unsigned char)c < 32 || (unsigned char)c > 126) continue;
    out[j++] = c;
  }
  out[j] = '\0';
}

void saveMessagingOn() {
  LittleFS.remove("/msgon.txt");
  File f = LittleFS.open("/msgon.txt", FILE_WRITE);
  if (f) { f.print(messagingOn ? 1 : 0); f.close(); }
}

void saveContacts() {
  LittleFS.remove("/contacts.txt");
  File f = LittleFS.open("/contacts.txt", FILE_WRITE);
  if (!f) return;
  for (int i = 0; i < contactCount; i++) f.printf("%s|%s|%d\n", contacts[i].name, contacts[i].ip, contacts[i].rtx ? 1 : 0);
  f.close();
}

int findContactByIp(const char* ip) {
  for (int i = 0; i < contactCount; i++) if (strcmp(contacts[i].ip, ip) == 0) return i;
  return -1;
}

// Adds (or renames) a contact; returns its index, or -1 if the list is full.
int addOrUpdateContact(const char* name, const char* ip) {
  int i = findContactByIp(ip);
  if (i < 0) {
    if (contactCount >= MAX_CONTACTS) return -1;
    i = contactCount++;
    contacts[i].unread = 0;
    contacts[i].rtx = false;
  }
  cleanMsgField(contacts[i].name, sizeof(contacts[i].name), name[0] ? name : "SynRG");
  cleanMsgField(contacts[i].ip, sizeof(contacts[i].ip), ip);
  saveContacts();
  return i;
}

void saveMessages() {
  LittleFS.remove("/messages.txt");
  File f = LittleFS.open("/messages.txt", FILE_WRITE);
  if (!f) return;
  for (int i = 0; i < msgCount; i++)
    f.printf("%c|%s|%s|%s\n", msgList[i].dir, msgList[i].ip, msgList[i].when, msgList[i].text);
  f.close();
}

void pushMessage(const MsgLine& m) {
  if (msgCount == MSG_KEEP) { memmove(&msgList[0], &msgList[1], sizeof(MsgLine) * (MSG_KEEP - 1)); msgCount--; }
  msgList[msgCount++] = m;
}

void addMessage(char dir, const char* ip, const char* text) {
  MsgLine m;
  m.dir = dir;
  cleanMsgField(m.ip, sizeof(m.ip), ip);
  cleanMsgField(m.text, sizeof(m.text), text);
  DateTime now = clockNow();
  formatTime12h(m.when, now.hour(), now.minute());
  pushMessage(m);
  saveMessages();
}

// split "a|b|c" in place; returns how many parts were found
int splitBars(char* line, char** parts, int maxParts) {
  int n = 0;
  parts[n++] = line;
  for (char* p = line; *p && n < maxParts; p++)
    if (*p == '|') { *p = '\0'; parts[n++] = p + 1; }
  return n;
}

void loadMessaging() {
  if (LittleFS.exists("/msgon.txt")) {
    File f = LittleFS.open("/msgon.txt");
    if (f) { messagingOn = (f.read() == '1'); f.close(); }
  }
  char line[120];
  char* parts[4];
  contactCount = 0;
  if (LittleFS.exists("/contacts.txt")) {
    File f = LittleFS.open("/contacts.txt");
    while (f && readLineFromFile(f, line, sizeof(line)) && contactCount < MAX_CONTACTS) {
      int got = splitBars(line, parts, 3);
      if (got < 2) continue;
      Contact& c = contacts[contactCount++];
      cleanMsgField(c.name, sizeof(c.name), parts[0]);
      cleanMsgField(c.ip, sizeof(c.ip), parts[1]);
      c.unread = 0;
      c.rtx = (got >= 3 && parts[2][0] == '1');
    }
    if (f) f.close();
  }
  msgCount = 0;
  if (LittleFS.exists("/messages.txt")) {
    File f = LittleFS.open("/messages.txt");
    while (f && readLineFromFile(f, line, sizeof(line))) {
      if (splitBars(line, parts, 4) < 4) continue;
      MsgLine m;
      m.dir = parts[0][0];
      cleanMsgField(m.ip, sizeof(m.ip), parts[1]);
      cleanMsgField(m.when, sizeof(m.when), parts[2]);
      cleanMsgField(m.text, sizeof(m.text), parts[3]);
      pushMessage(m);
    }
    if (f) f.close();
  }
}

String urlEncode(const char* in) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (; *in; in++) {
    unsigned char c = *in;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

// Added to everything sent to another SynRG: "&rtx=1" while this PDA is in
// Jensen mode, so its picture on their side becomes the green RTX badge.
String rtxTag() { return String(currentTheme == THEME_SECRET ? "&rtx=1" : "&rtx=0"); }

// A peer just told us (or not) whether it is in Jensen mode. PDAs on older
// firmware don't say, and their picture is left as it was.
void noteContactRtx(int idx) {
  if (idx < 0 || !server.hasArg("rtx")) return;
  bool rtx = (server.arg("rtx") == "1");
  if (contacts[idx].rtx != rtx) { contacts[idx].rtx = rtx; saveContacts(); }
}

// Calls another SynRG; returns the HTTP code (200 = ok, 403 = they have
// messaging off / we're not their contact, <0 = couldn't reach them).
int callPeer(const char* ip, const String& pathAndQuery) {
  if (WiFi.status() != WL_CONNECTED) return -100;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(client, String("http://") + ip + pathAndQuery);
  int code = http.GET();
  http.end();
  return code;
}

void describePeerError(int code, const char* doing) {
  if (code == -100) snprintf(msgStatus, sizeof(msgStatus), "No WiFi");
  else if (code == 403) snprintf(msgStatus, sizeof(msgStatus), "They have ShareNote off");
  else if (code == 410) snprintf(msgStatus, sizeof(msgStatus), "You're not in their contacts");
  else if (code == 404) snprintf(msgStatus, sizeof(msgStatus), "Not a SynRG with ShareNote");
  else if (code < 0) snprintf(msgStatus, sizeof(msgStatus), "Couldn't reach that IP");
  else snprintf(msgStatus, sizeof(msgStatus), "%s failed (%d)", doing, code);
}

void sendConnectRequest(const char* ip) {
  IPAddress target;
  if (!target.fromString(ip)) { strcpy(msgStatus, "That IP doesn't look right"); return; }
  if (WiFi.status() == WL_CONNECTED && target == WiFi.localIP()) { strcpy(msgStatus, "That's this PDA's own IP"); return; }
  int existing = findContactByIp(ip);
  if (existing >= 0) { openChat(existing); return; }
  strcpy(msgStatus, "Asking to connect...");
  drawMessagesScreen();
  int code = callPeer(ip, "/msg/request?from=" + urlEncode(deviceName) + rtxTag());
  if (code == 200) {
    strncpy(msgOutgoingIp, ip, sizeof(msgOutgoingIp) - 1);
    msgOutgoingIp[sizeof(msgOutgoingIp) - 1] = '\0';
    strcpy(msgStatus, "Request sent - waiting for them");
  } else {
    describePeerError(code, "Request");
  }
}

void sendChatMessage(const char* text) {
  if (chatContact < 0) return;
  Contact& c = contacts[chatContact];
  int code = callPeer(c.ip, "/msg/send?from=" + urlEncode(deviceName) + rtxTag() + "&text=" + urlEncode(text));
  // delivered -> 'o' (gets a tick); otherwise kept as 'f' so it can be retried
  addMessage(code == 200 ? 'o' : 'f', c.ip, text);
}

// ---- incoming (web server handlers) ----
bool canShowPopupNow() {
  return currentScreen == SCREEN_HOME || currentScreen == SCREEN_LOCK ||
         currentScreen == SCREEN_MESSAGES || (currentScreen == SCREEN_CHAT && !chatTyping) ||
         currentScreen == SCREEN_SETTINGS || currentScreen == SCREEN_NEARBY;
}

void refreshAfterIncoming() {
  if (currentScreen == SCREEN_HOME) drawHomeScreen(clockNow());
  else if (currentScreen == SCREEN_LOCK) drawLockScreen(clockNow());
  else if (currentScreen == SCREEN_MESSAGES) drawMessagesScreen();
  else if (currentScreen == SCREEN_CHAT) drawChatScreen();
}

void handleMsgRequest() {
  if (!messagingOn) { server.send(403, "text/plain", "messaging off"); return; }
  String from = server.hasArg("from") ? server.arg("from") : String("SynRG");
  String ip = server.client().remoteIP().toString();
  int existing = findContactByIp(ip.c_str());
  if (existing >= 0) {                        // already friends: just say yes again
    noteContactRtx(existing);
    server.send(200, "text/plain", "ok");
    callPeer(ip.c_str(), "/msg/accept?from=" + urlEncode(deviceName) + rtxTag());
    return;
  }
  server.send(200, "text/plain", "pending");
  cleanMsgField(reqName, sizeof(reqName), from.length() ? from.c_str() : "SynRG");
  cleanMsgField(reqIp, sizeof(reqIp), ip.c_str());
  reqRtx = server.hasArg("rtx") && server.arg("rtx") == "1";
  reqPending = true;
  if (canShowPopupNow()) showConnectPopup();
}

void handleMsgAccept() {
  String ip = server.client().remoteIP().toString();
  if (strcmp(ip.c_str(), msgOutgoingIp) != 0 && findContactByIp(ip.c_str()) < 0) {
    server.send(403, "text/plain", "no request");
    return;
  }
  String from = server.hasArg("from") ? server.arg("from") : String("SynRG");
  server.send(200, "text/plain", "ok");
  int idx = addOrUpdateContact(from.c_str(), ip.c_str());
  noteContactRtx(idx);
  msgOutgoingIp[0] = '\0';
  if (idx >= 0) {
    snprintf(msgBanner, sizeof(msgBanner), "%s accepted!", contacts[idx].name);
    snprintf(msgStatus, sizeof(msgStatus), "%s accepted!", contacts[idx].name);
  }
  refreshAfterIncoming();
  if (idx >= 0) showBanner(contacts[idx].name, contacts[idx].ip, "accepted your request");
}

void handleMsgSend() {
  String ip = server.client().remoteIP().toString();
  int idx = findContactByIp(ip.c_str());
  if (!messagingOn) { server.send(403, "text/plain", "messaging off"); return; }
  if (idx < 0) { server.send(410, "text/plain", "not a contact"); return; }
  if (!server.hasArg("text") || server.arg("text").length() == 0) { server.send(400, "text/plain", "missing text"); return; }
  if (server.hasArg("from") && server.arg("from").length())     // they may have renamed their PDA
    addOrUpdateContact(server.arg("from").c_str(), ip.c_str());
  noteContactRtx(idx);
  addMessage('i', ip.c_str(), server.arg("text").c_str());
  server.send(200, "text/plain", "ok");
  bool reading = (currentScreen == SCREEN_CHAT && chatContact == idx);
  if (reading) {
    chatScroll = 0;
  } else {
    contacts[idx].unread++;
    snprintf(msgBanner, sizeof(msgBanner), "New message from %s", contacts[idx].name);
  }
  refreshAfterIncoming();
  if (!reading) showBanner(contacts[idx].name, contacts[idx].ip, msgList[msgCount - 1].text);
}

// ---- connect pop-up ----
void showConnectPopup() {
  if (currentScreen != SCREEN_MSG_REQUEST) reqReturnScreen = currentScreen;
  currentScreen = SCREEN_MSG_REQUEST;
  tft.fillRoundRect(16, 70, 208, 170, 18, COLOR_SHADOW);
  tft.fillRoundRect(14, 66, 208, 170, 18, COLOR_SURFACE);
  tft.drawRoundRect(14, 66, 208, 170, 18, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 118, 84, reqName, COLOR_TEXT, 196);
  printCentered(&FreeSans9pt7b, 118, 114, "wants to connect", COLOR_TEXT);
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(COLOR_SUBTEXT);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(reqIp, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(118 - tw / 2, 140);
  tft.print(reqIp);
  drawGlassRect(26, 176, 88, 44, 16, COLOR_ERROR);
  printFitted(&FreeSansBold12pt7b, 70, 190, "Decline", ILI9341_WHITE, 80);
  drawGlassRect(122, 176, 88, 44, 16, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 166, 190, "Accept", ILI9341_WHITE, 80);
}

void handleConnectPopupTouch(int x, int y) {
  if (y < 176 || y > 220) return;
  if (x >= 26 && x <= 114) {                      // Decline
    reqPending = false;
    currentScreen = reqReturnScreen;
    if (currentScreen == SCREEN_MESSAGES) drawMessagesScreen();
    else if (currentScreen == SCREEN_CHAT) drawChatScreen();
    else if (currentScreen == SCREEN_SETTINGS) drawSettingsScreen();
    else if (currentScreen == SCREEN_NEARBY) drawNearbyScreen();
    else if (currentScreen == SCREEN_LOCK) drawLockScreen(clockNow());
    else { currentScreen = SCREEN_HOME; drawHomeScreen(clockNow()); }
    lastDrawnMinute = clockNow().minute();
    return;
  }
  if (x >= 122 && x <= 210) {                     // Accept
    reqPending = false;
    int idx = addOrUpdateContact(reqName, reqIp);
    if (idx >= 0 && contacts[idx].rtx != reqRtx) { contacts[idx].rtx = reqRtx; saveContacts(); }
    callPeer(reqIp, "/msg/accept?from=" + urlEncode(deviceName) + rtxTag());
    if (idx >= 0) openChat(idx);
    else { strcpy(msgStatus, "Contacts full (8)"); openMessages(); }
  }
}

// ---- look & feel helpers: avatars, last-message previews ----
uint16_t avatarColor(const char* name) {
  unsigned h = 0;
  for (const char* p = name; *p; p++) h = h * 31 + (unsigned char)*p;
  return rgb565(customAccents[h % 8]);
}

// Colored circle with the contact's first letter.
void drawAvatar(int cx, int cy, int r, const char* name) {
  tft.fillCircle(cx, cy, r, avatarColor(name));
  char letter = name[0] ? toupper((unsigned char)name[0]) : '?';
  tft.setFont();
  tft.setTextColor(ILI9341_WHITE);
  if (r >= 12) { tft.setTextSize(2); tft.setCursor(cx - 5, cy - 7); }
  else { tft.setTextSize(1); tft.setCursor(cx - 2, cy - 3); }
  tft.print(letter);
  tft.setTextSize(1);
}

// A contact's picture: their colored initial, or -- if their PDA is in
// Jensen mode -- a green badge that says RTX.
void drawContactAvatar(int cx, int cy, int r, const Contact& c) {
  if (!c.rtx) { drawAvatar(cx, cy, r, c.name); return; }
  tft.fillCircle(cx, cy, r, tft.color565(118, 185, 0));
  tft.drawCircle(cx, cy, r, ILI9341_BLACK);
  tft.setFont(&SynSmallBold);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_BLACK);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds("RTX", 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(cx - (int)tw / 2 - x1, cy + 4);
  tft.print("RTX");
  tft.setFont();
}

// index in msgList of the newest message with this IP, or -1
int lastMessageFor(const char* ip) {
  for (int i = msgCount - 1; i >= 0; i--) if (strcmp(msgList[i].ip, ip) == 0) return i;
  return -1;
}

// ---- notification banner: slides over the top of most screens for a few
// seconds; tapping it opens that chat. ----
bool bannerOkHere() {
  switch (currentScreen) {
    case SCREEN_LOCK: case SCREEN_HOME: case SCREEN_SETTINGS: case SCREEN_TODO:
    case SCREEN_HOMEWORK: case SCREEN_MESSAGES: case SCREEN_CHAT: case SCREEN_NEARBY: case SCREEN_NEWS:
    case SCREEN_THEMES: case SCREEN_THEME_CUSTOM: case SCREEN_MORE_ABOUT:
    case SCREEN_KEYBOARD: case SCREEN_WEATHER: case SCREEN_SET_TIME: case SCREEN_SET_DATE:
      return true;
    default:
      return false;   // Notes, Snake, updates etc. are never drawn over
  }
}

void redrawCurrentScreen() {
  switch (currentScreen) {
    case SCREEN_LOCK:         drawLockScreen(clockNow()); break;
    case SCREEN_HOME:         drawHomeScreen(clockNow()); break;
    case SCREEN_SETTINGS:     drawSettingsScreen(); break;
    case SCREEN_TODO:         drawTodoScreen(); break;
    case SCREEN_HOMEWORK:     drawHomeworkScreen(); break;
    case SCREEN_MESSAGES:     drawMessagesScreen(); break;
    case SCREEN_CHAT:         drawChatScreen(); break;
    case SCREEN_NEARBY:       drawNearbyScreen(); break;
    case SCREEN_NEWS:         drawNewsScreen(); break;
    case SCREEN_THEMES:       drawThemesScreen(); break;
    case SCREEN_THEME_CUSTOM: drawThemeCustomScreen(); break;
    case SCREEN_MORE_ABOUT:   drawMoreAboutScreen(); break;
    case SCREEN_KEYBOARD:     drawKeyboardScreen(); break;
    case SCREEN_WEATHER:      drawWeatherScreen(); break;
    case SCREEN_SET_TIME:     drawSetTimeScreen(); break;
    case SCREEN_SET_DATE:     drawSetDateScreen(); break;
    default: break;
  }
}

void showBanner(const char* name, const char* ip, const char* line) {
  if (!bannerOkHere()) return;
  strncpy(bannerIp, ip, sizeof(bannerIp) - 1);
  bannerIp[sizeof(bannerIp) - 1] = '\0';
  bannerActive = true;
  bannerUntilMs = millis() + 4000;

  // kept above y=41 so Home's ticking clock and icon animations don't draw over it
  tft.fillRoundRect(4, 2, 232, 38, 12, COLOR_SURFACE);
  tft.drawRoundRect(4, 2, 232, 38, 12, COLOR_ACCENT);
  tft.drawRoundRect(5, 3, 230, 36, 11, COLOR_ACCENT);
  int bannerContact = findContactByIp(ip);
  if (bannerContact >= 0) drawContactAvatar(24, 21, 13, contacts[bannerContact]);
  else drawAvatar(24, 21, 13, name);
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(46, 9);
  tft.print(name);
  char shown[30];
  snprintf(shown, sizeof(shown), "%s", line);
  tft.setTextColor(COLOR_SUBTEXT);
  tft.setCursor(46, 24);
  tft.print(shown);
}

// Called every loop(): hides the banner when its time is up.
void updateBanner() {
  if (!bannerActive || (long)(millis() - bannerUntilMs) < 0) return;
  bannerActive = false;
  if (bannerOkHere()) redrawCurrentScreen();
}

// A tap on the banner: open that conversation.
void openBannerChat() {
  bannerActive = false;
  msgBanner[0] = '\0';
  int idx = findContactByIp(bannerIp);
  if (idx >= 0) openChat(idx);
  else openMessages();
}

// ---- nearby discovery (like AirDrop): PDAs with Messaging on answer a
// UDP broadcast "SYNRG?" on port 4210 with "SYNRG!<name>". ----
void ensureDiscoveryUdp() {
  if (discoveryUdpOn || WiFi.status() != WL_CONNECTED) return;
  discoveryUdpOn = discoveryUdp.begin(DISCOVERY_PORT);
}

void sendDiscoveryQuery() {
  IPAddress ip = WiFi.localIP(), mask = WiFi.subnetMask(), bcast;
  for (int i = 0; i < 4; i++) bcast[i] = ip[i] | (uint8_t)~mask[i];
  discoveryUdp.beginPacket(bcast, DISCOVERY_PORT);
  discoveryUdp.print("SYNRG?");
  discoveryUdp.endPacket();
}

// Reads one waiting discovery packet. Answers queries (when Messaging is
// on); when `collect` is set, adds replies to the nearby list.
void pollDiscovery(bool collect) {
  ensureDiscoveryUdp();
  if (!discoveryUdpOn) return;
  int size = discoveryUdp.parsePacket();
  if (size <= 0) return;
  char buf[40];
  int n = discoveryUdp.read(buf, sizeof(buf) - 1);
  if (n < 0) n = 0;
  buf[n] = '\0';
  IPAddress from = discoveryUdp.remoteIP();
  if (from == WiFi.localIP()) return;                       // our own broadcast

  if (strcmp(buf, "SYNRG?") == 0) {
    if (!messagingOn) return;
    discoveryUdp.beginPacket(from, discoveryUdp.remotePort());
    discoveryUdp.print("SYNRG!");
    discoveryUdp.print(deviceName);
    discoveryUdp.endPacket();
  } else if (collect && strncmp(buf, "SYNRG!", 6) == 0) {
    String ipStr = from.toString();
    for (int i = 0; i < nearbyCount; i++) if (strcmp(nearby[i].ip, ipStr.c_str()) == 0) return;
    if (nearbyCount >= MAX_NEARBY) return;
    cleanMsgField(nearby[nearbyCount].name, sizeof(nearby[nearbyCount].name), buf[6] ? buf + 6 : "SynRG");
    cleanMsgField(nearby[nearbyCount].ip, sizeof(nearby[nearbyCount].ip), ipStr.c_str());
    nearby[nearbyCount].unread = 0;
    nearby[nearbyCount].rtx = false;
    nearbyCount++;
  }
}

void scanNearby() {
  nearbyCount = 0;
  nearbySearching = true;
  drawNearbyScreen();
  ensureDiscoveryUdp();
  if (discoveryUdpOn) {
    unsigned long start = millis();
    int sent = 0;
    while (millis() - start < 1800) {
      if (sent < 3 && millis() - start >= (unsigned long)sent * 500) { sendDiscoveryQuery(); sent++; }
      pollDiscovery(true);
      delay(10);
    }
  }
  nearbySearching = false;
  drawNearbyScreen();
}

void openNearby() {
  currentScreen = SCREEN_NEARBY;
  scanNearby();
}

void drawNearbyScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Nearby");

  if (nearbySearching) {
    printCentered(&FreeSansBold12pt7b, 120, 120, "Searching...", COLOR_TEXT);
    printCentered(&FreeSans9pt7b, 120, 156, "Looking for SynRGs", COLOR_SUBTEXT);
    printCentered(&FreeSans9pt7b, 120, 176, "with ShareNote on", COLOR_SUBTEXT);
    return;
  }
  if (nearbyCount == 0) {
    printCentered(&FreeSansBold12pt7b, 120, 100, "Nobody found", COLOR_TEXT);
    printCentered(&FreeSans9pt7b, 120, 136, "They need ShareNote on", COLOR_SUBTEXT);
    printCentered(&FreeSans9pt7b, 120, 156, "and the same WiFi.", COLOR_SUBTEXT);
    printCentered(&FreeSans9pt7b, 120, 186, "Or go back and use By IP.", COLOR_SUBTEXT);
  }
  for (int i = 0; i < nearbyCount; i++) {
    int y = 32 + i * 40;
    drawGlassRect(8, y, 224, 34, 12, COLOR_SURFACE);
    drawAvatar(26, y + 17, 13, nearby[i].name);
    tft.setTextColor(COLOR_TEXT);
    tft.setCursor(46, y + 7);
    tft.print(nearby[i].name);
    tft.setTextColor(COLOR_SUBTEXT);
    tft.setCursor(46, y + 20);
    tft.print(nearby[i].ip);
    if (findContactByIp(nearby[i].ip) >= 0) { tft.setCursor(180, y + 20); tft.print("contact"); }
  }
  drawGlassRect(20, 278, 200, 34, 16, COLOR_BLUE);
  printFitted(&FreeSansBold12pt7b, 120, 287, "Search Again", ILI9341_WHITE, 188);
}

void handleNearbyTouch(int x, int y) {
  if (x >= 20 && x <= 220 && y >= 278 && y <= 312) { scanNearby(); return; }
  for (int i = 0; i < nearbyCount; i++) {
    int ry = 32 + i * 40;
    if (y >= ry && y <= ry + 34) {
      char ip[16];
      strcpy(ip, nearby[i].ip);
      openMessages();
      sendConnectRequest(ip);                 // opens the chat if already a contact
      if (currentScreen == SCREEN_MESSAGES) drawMessagesScreen();
      return;
    }
  }
}

// ---- Messages app: switch, contact list, Find Nearby / By IP ----
const int CONTACT_Y = 66, CONTACT_H = 38, CONTACT_ROWS = 5;

void openMessages() {
  chatConfirmDelete = false;
  chatTyping = false;
  chatContact = -1;
  currentScreen = SCREEN_MESSAGES;
  drawMessagesScreen();
}

void drawMessagesScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("ShareNote");

  drawGlassRect(8, 30, 224, 30, 12, messagingOn ? COLOR_ACCENT : COLOR_SUBTEXT);
  printFitted(&FreeSansBold12pt7b, 120, 37, messagingOn ? "ShareNote: On" : "ShareNote: Off", ILI9341_WHITE, 210);

  if (contactScroll > contactCount - CONTACT_ROWS) contactScroll = contactCount - CONTACT_ROWS;
  if (contactScroll < 0) contactScroll = 0;

  if (contactCount == 0) {
    printCentered(&FreeSans9pt7b, 120, 110, "No contacts yet", COLOR_SUBTEXT);
    printCentered(&FreeSans9pt7b, 120, 134, "Tap Find Nearby to see", COLOR_SUBTEXT);
    printCentered(&FreeSans9pt7b, 120, 154, "other SynRGs on this WiFi", COLOR_SUBTEXT);
  }
  for (int row = 0; row < CONTACT_ROWS && row + contactScroll < contactCount; row++) {
    Contact& c = contacts[row + contactScroll];
    int y = CONTACT_Y + row * CONTACT_H;
    drawGlassRect(8, y, 224, 34, 12, COLOR_SURFACE);
    drawContactAvatar(26, y + 17, 13, c);

    int last = lastMessageFor(c.ip);
    tft.setTextColor(COLOR_TEXT);
    if (strlen(c.name) <= 11) {
      tft.setFont(&FreeSansBold9pt7b);
      tft.setCursor(46, y + 15);
      tft.print(c.name);
      tft.setFont();
    } else {
      tft.setCursor(46, y + 6);
      tft.print(c.name);
    }
    tft.setTextColor(COLOR_SUBTEXT);
    if (last >= 0) {
      // time of the last message, top right
      tft.setCursor(226 - strlen(msgList[last].when) * 6, y + 6);
      tft.print(msgList[last].when);
      char preview[25];
      snprintf(preview, sizeof(preview), "%s%s", msgList[last].dir == 'i' ? "" : "You: ", msgList[last].text);
      if (c.unread > 0) tft.setTextColor(COLOR_TEXT);
      tft.setCursor(46, y + 22);
      tft.print(preview);
    } else {
      tft.setCursor(46, y + 22);
      tft.print(c.ip);
    }
    if (c.unread > 0) {
      tft.fillCircle(218, y + 24, 7, COLOR_ERROR);
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(216, y + 21);
      tft.print(c.unread > 9 ? 9 : c.unread);
    }
  }

  // status line, with scroll arrows when there are more contacts than rows
  bool canScroll = contactCount > CONTACT_ROWS;
  if (msgStatus[0]) printFitted(&FreeSans9pt7b, canScroll ? 96 : 120, 260, msgStatus, COLOR_SUBTEXT, canScroll ? 180 : 224);
  if (canScroll) {
    tft.fillTriangle(196, 272, 204, 260, 212, 272, contactScroll > 0 ? COLOR_TEXT : COLOR_FRAME);
    tft.fillTriangle(218, 260, 226, 272, 234, 260, contactScroll < contactCount - CONTACT_ROWS ? COLOR_TEXT : COLOR_FRAME);
  }

  uint16_t btn = messagingOn ? COLOR_BLUE : COLOR_SUBTEXT;
  drawGlassRect(8, 280, 124, 32, 14, btn);
  printFitted(&FreeSansBold12pt7b, 70, 288, "Find Nearby", ILI9341_WHITE, 116);
  drawGlassRect(140, 280, 92, 32, 14, btn);
  printFitted(&FreeSansBold12pt7b, 186, 288, "By IP", ILI9341_WHITE, 84);
}

void startMsgKeyboard(KeyboardContext ctx, const char* prefill, KeyboardMode mode) {
  strncpy(kbBuffer, prefill, sizeof(kbBuffer) - 1);
  kbBuffer[sizeof(kbBuffer) - 1] = '\0';
  kbBufferLen = strlen(kbBuffer);
  kbMode = mode;
  kbShift = false;
  kbContext = ctx;
  currentScreen = SCREEN_KEYBOARD;
  drawKeyboardScreen();
}

void handleMessagesTouch(int x, int y) {
  if (y >= 30 && y <= 60) {                                    // on/off switch
    messagingOn = !messagingOn;
    saveMessagingOn();
    msgStatus[0] = '\0';
    drawMessagesScreen();
    return;
  }
  if (y >= 280 && y <= 312) {                                  // Find Nearby / By IP
    if (!messagingOn) { strcpy(msgStatus, "Turn ShareNote on first"); drawMessagesScreen(); return; }
    msgStatus[0] = '\0';
    if (x >= 8 && x <= 132) {
      if (WiFi.status() != WL_CONNECTED) { strcpy(msgStatus, "No WiFi"); drawMessagesScreen(); return; }
      openNearby();
    } else if (x >= 140 && x <= 232) {
      startMsgKeyboard(KB_CONTEXT_MSG_IP, newChatIp, KB_MODE_123);
    }
    return;
  }
  if (contactCount > CONTACT_ROWS && y >= 254 && y <= 278 && x >= 190) {   // scroll arrows
    if (x < 215 && contactScroll > 0) contactScroll--;
    else if (x >= 215 && contactScroll < contactCount - CONTACT_ROWS) contactScroll++;
    drawMessagesScreen();
    return;
  }
  for (int row = 0; row < CONTACT_ROWS && row + contactScroll < contactCount; row++) {
    int ry = CONTACT_Y + row * CONTACT_H;
    if (y >= ry && y <= ry + 34) { openChat(row + contactScroll); return; }
  }
}

// ---- chat with one contact ----
// Removes a contact and erases the conversation with them.
void deleteContact(int idx) {
  if (idx < 0 || idx >= contactCount) return;
  int kept = 0;
  for (int i = 0; i < msgCount; i++)
    if (strcmp(msgList[i].ip, contacts[idx].ip) != 0) msgList[kept++] = msgList[i];
  msgCount = kept;
  saveMessages();
  for (int i = idx; i < contactCount - 1; i++) contacts[i] = contacts[i + 1];
  contactCount--;
  saveContacts();
}

void openChat(int idx) {
  chatConfirmDelete = false;
  chatTyping = false;
  chatScroll = 0;
  chatContact = idx;
  contacts[idx].unread = 0;
  msgStatus[0] = '\0';
  currentScreen = SCREEN_CHAT;
  drawChatScreen();
}

// Splits a message into lines of at most BUBBLE_CHARS, breaking at a space
// when there is one in the second half of the line. Returns the line count.
int wrapMessage(const char* text, char lines[BUBBLE_MAX_LINES][BUBBLE_CHARS + 1]) {
  int n = 0;
  int len = strlen(text);
  int pos = 0;
  while (pos < len && n < BUBBLE_MAX_LINES) {
    int remaining = len - pos;
    int take = remaining;
    int skip = 0;
    if (remaining > BUBBLE_CHARS) {
      take = BUBBLE_CHARS;
      for (int k = BUBBLE_CHARS; k >= BUBBLE_CHARS / 2; k--) {
        if (text[pos + k] == ' ') { take = k; skip = 1; break; }
      }
    }
    memcpy(lines[n], text + pos, take);
    lines[n][take] = '\0';
    n++;
    pos += take + skip;
  }
  if (n == 0) { lines[0][0] = '\0'; n = 1; }
  return n;
}

// Draws the conversation between y=top and y=bottom, newest at the bottom,
// skipping the newest `chatScroll` messages. Bubbles fit their text, have a
// little tail, and the time sits under the last bubble of each group.
void drawChatMessages(int top, int bottom) {
  Contact& c = contacts[chatContact];
  int idx[MSG_KEEP];
  int total = 0;
  for (int i = 0; i < msgCount; i++) if (strcmp(msgList[i].ip, c.ip) == 0) idx[total++] = i;
  if (chatScroll > total - 1) chatScroll = total > 0 ? total - 1 : 0;

  chatHitCount = 0;
  chatCanScrollUp = false;
  tft.setFont();
  tft.setTextSize(1);
  if (total == 0) { printCentered(&FreeSans9pt7b, 120, (top + bottom) / 2 - 8, "Say hi!", COLOR_SUBTEXT); return; }

  int y = bottom;
  char lines[BUBBLE_MAX_LINES][BUBBLE_CHARS + 1];
  for (int k = total - 1 - chatScroll; k >= 0; k--) {
    MsgLine& m = msgList[idx[k]];
    bool out = (m.dir != 'i');
    bool failed = (m.dir == 'f');
    int n = wrapMessage(m.text, lines);
    int widest = 1;
    for (int l = 0; l < n; l++) if ((int)strlen(lines[l]) > widest) widest = strlen(lines[l]);
    int widestPx = 6;
    tft.setFont();
    for (int l = 0; l < n; l++) {
      int16_t bx1, by1; uint16_t bw, bh;
      tft.getTextBounds(lines[l], 0, 0, &bx1, &by1, &bw, &bh);
      if ((int)bw > widestPx) widestPx = bw;
    }
    int w = widestPx + 16, h = n * 11 + 8;

    // time row: under the last bubble of a run from the same side & minute
    bool showTime = failed || (k == total - 1);
    if (!showTime) {
      MsgLine& next = msgList[idx[k + 1]];
      showTime = ((next.dir != 'i') != out) || strcmp(next.when, m.when) != 0 || next.dir == 'f';
    }
    int need = h + (showTime ? 11 : 0) + 4;
    if (y - need < top) {
      chatCanScrollUp = true;
      int room = (y - top - 4 - 8 - (showTime ? 11 : 0)) / 11;       // lines that would still fit
      if (y != bottom || room < 1) break;
      // nothing drawn yet: show the end of this message rather than a blank area
      int drop = n - room;
      for (int l = 0; l < room; l++) strcpy(lines[l], lines[l + drop]);
      n = room;
      if (strlen(lines[0]) > 3) memcpy(lines[0], "...", 3);
      h = n * 11 + 8;
      need = h + (showTime ? 11 : 0) + 4;
    }
    y -= need;

    int x = out ? 232 - w : 8;
    uint16_t fill = out ? COLOR_ACCENT : COLOR_SURFACE;
    tft.fillRoundRect(x, y, w, h, 8, fill);
    if (out) tft.fillTriangle(x + w - 10, y + h - 1, x + w - 1, y + h - 8, x + w + 4, y + h + 2, fill);
    else {
      tft.drawRoundRect(x, y, w, h, 8, COLOR_FRAME);
      tft.fillTriangle(x + 10, y + h - 1, x + 1, y + h - 8, x - 4, y + h + 2, fill);
    }
    tft.setTextColor(out ? ILI9341_WHITE : COLOR_TEXT);
    for (int l = 0; l < n; l++) { tft.setCursor(x + 8, y + 4 + l * 11); tft.print(lines[l]); }

    if (failed) {                                   // red "!" beside the bubble
      tft.fillCircle(x - 10, y + h / 2, 6, COLOR_ERROR);
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(x - 12, y + h / 2 - 3);
      tft.print("!");
    } else if (out) {                               // delivered tick
      tft.drawLine(x - 12, y + h - 8, x - 9, y + h - 5, COLOR_ACCENT);
      tft.drawLine(x - 9, y + h - 5, x - 4, y + h - 11, COLOR_ACCENT);
    }
    if (showTime) {
      const char* label = failed ? "Not sent - tap to retry" : m.when;
      tft.setTextColor(failed ? COLOR_ERROR : COLOR_SUBTEXT);
      tft.setCursor(out ? 220 - (int)strlen(label) * 6 : 20, y + h + 2);
      tft.print(label);
    }
    if (chatHitCount < CHAT_HIT_MAX) {
      chatHitMsg[chatHitCount] = idx[k];
      chatHitY0[chatHitCount] = y;
      chatHitY1[chatHitCount] = y + h + (showTime ? 11 : 0);
      chatHitCount++;
    }
  }
}

// the text box above the keyboard while typing
void drawChatInput() {
  tft.fillRoundRect(8, 98, 224, 24, 8, COLOR_SURFACE);
  tft.drawRoundRect(8, 98, 224, 24, 8, COLOR_ACCENT);
  tft.setFont();
  tft.setTextSize(1);
  if (kbBufferLen == 0) {
    tft.setTextColor(COLOR_SUBTEXT);
    tft.setCursor(16, 106);
    tft.print("Message...");
    return;
  }
  const int showMax = 30;
  const char* shown = kbBufferLen > showMax ? kbBuffer + (kbBufferLen - showMax) : kbBuffer;
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(16, 106);
  tft.print(shown);
  char count[8];
  snprintf(count, sizeof(count), "%d", MSG_TEXT_MAX - kbBufferLen);
  tft.setTextColor(COLOR_SUBTEXT);
  tft.setCursor(226 - strlen(count) * 6, 106);
  tft.print(count);
}

// the half-height keyboard under the chat
void drawChatKeyboard() {
  tft.fillRect(2, 124, 236, 194, COLOR_BG);
  bool upper = (kbMode == KB_MODE_ABC) && kbShift;
  drawKeyRow(kbGetRow1(), 126, 32, upper);
  drawKeyRow(kbGetRow2(), 162, 32, upper);
  drawKeyRow(kbGetRow3(), 198, 32, upper);

  const char* pageLabel = "123";
  if (kbMode == KB_MODE_123) pageLabel = "#+=";
  else if (kbMode == KB_MODE_SYM) pageLabel = "ABC";
  drawGlassRect(8, 234, 70, 32, 12, COLOR_SETTINGS);
  printFitted(&FreeSansBold12pt7b, 43, 242, pageLabel, ILI9341_WHITE, 60);
  drawGlassRect(86, 234, 70, 32, 12, kbShift ? COLOR_ACCENT : COLOR_SUBTEXT);
  printFitted(&FreeSansBold12pt7b, 121, 242, "SHIFT", ILI9341_WHITE, 60);
  drawGlassRect(164, 234, 70, 32, 12, COLOR_SUBTEXT);
  printFitted(&FreeSansBold12pt7b, 199, 242, "DEL", ILI9341_WHITE, 60);

  drawGlassRect(4, 272, 136, 40, 14, COLOR_SETTINGS);
  printFitted(&FreeSansBold12pt7b, 72, 284, "SPACE", ILI9341_WHITE, 124);
  drawGlassRect(148, 272, 88, 40, 14, COLOR_ACCENT);
  printFitted(&FreeSansBold12pt7b, 192, 284, "SEND", ILI9341_WHITE, 78);
}

void drawChatScreen() {
  if (chatContact < 0 || chatContact >= contactCount) { openMessages(); return; }
  Contact& c = contacts[chatContact];
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();

  // header: back arrow, avatar, name with the IP under it, Delete
  tft.fillRect(0, 0, 240, 36, COLOR_STATUSBAR);
  tft.setFont();
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(8, 10);
  tft.print("<");
  tft.setTextSize(1);
  drawContactAvatar(42, 18, 13, c);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(62, 7);
  tft.print(c.name);
  tft.setCursor(62, 21);
  tft.print(c.ip);
  tft.setCursor(198, 14);
  tft.print("Delete");

  if (chatTyping) {
    drawChatMessages(40, 96);
    drawChatInput();
    drawChatKeyboard();
  } else {
    drawChatMessages(40, 266);
    drawGlassRect(8, 272, 156, 40, 16, COLOR_ACCENT);
    printFitted(&FreeSansBold12pt7b, 86, 284, "Write", ILI9341_WHITE, 146);
    // scroll arrows: older / newer
    drawGlassRect(170, 272, 30, 40, 12, chatCanScrollUp ? COLOR_SETTINGS : COLOR_FRAME);
    tft.fillTriangle(177, 298, 185, 284, 193, 298, ILI9341_WHITE);
    drawGlassRect(204, 272, 30, 40, 12, chatScroll > 0 ? COLOR_SETTINGS : COLOR_FRAME);
    tft.fillTriangle(211, 286, 219, 300, 227, 286, ILI9341_WHITE);
  }

  if (chatConfirmDelete) {
    tft.fillRoundRect(16, 84, 208, 150, 18, COLOR_SHADOW);
    tft.fillRoundRect(14, 80, 208, 150, 18, COLOR_SURFACE);
    tft.drawRoundRect(14, 80, 208, 150, 18, COLOR_ERROR);
    char q[40];
    snprintf(q, sizeof(q), "Delete %s?", c.name);
    printFitted(&FreeSansBold12pt7b, 118, 98, q, COLOR_TEXT, 196);
    printCentered(&FreeSans9pt7b, 118, 130, "Your chat is erased too", COLOR_SUBTEXT);
    drawGlassRect(26, 170, 88, 44, 16, COLOR_SUBTEXT);
    printFitted(&FreeSansBold12pt7b, 70, 184, "Keep", ILI9341_WHITE, 80);
    drawGlassRect(122, 170, 88, 44, 16, COLOR_ERROR);
    printFitted(&FreeSansBold12pt7b, 166, 184, "Delete", ILI9341_WHITE, 80);
  }
}

// Tries a failed message again; on success it becomes a normal sent message.
void retryMessage(int msgIndex) {
  MsgLine& m = msgList[msgIndex];
  if (m.dir != 'f') return;
  int code = callPeer(m.ip, "/msg/send?from=" + urlEncode(deviceName) + rtxTag() + "&text=" + urlEncode(m.text));
  if (code == 200) {
    m.dir = 'o';
    DateTime now = clockNow();
    formatTime12h(m.when, now.hour(), now.minute());
    saveMessages();
  }
  drawChatScreen();
}

void handleChatTypingTouch(int x, int y) {
  char c = hitTestKeyRow(kbGetRow1(), x, y, 126, 32);
  if (!c) c = hitTestKeyRow(kbGetRow2(), x, y, 162, 32);
  if (!c) c = hitTestKeyRow(kbGetRow3(), x, y, 198, 32);
  if (c) {
    if (kbBufferLen < MSG_TEXT_MAX) {
      if (kbMode == KB_MODE_ABC && kbShift) c = toupper(c);
      kbBuffer[kbBufferLen++] = c;
      kbBuffer[kbBufferLen] = '\0';
      drawChatInput();                         // only the text box redraws: fast typing
    }
    return;
  }
  if (y >= 234 && y <= 266) {
    if (x >= 8 && x <= 78) {                                   // page: ABC -> 123 -> #+=
      kbMode = (kbMode == KB_MODE_ABC) ? KB_MODE_123 : (kbMode == KB_MODE_123 ? KB_MODE_SYM : KB_MODE_ABC);
      drawChatKeyboard();
    } else if (x >= 86 && x <= 156) {                          // SHIFT
      kbShift = !kbShift;
      drawChatKeyboard();
    } else if (x >= 164 && x <= 234 && kbBufferLen > 0) {      // DEL
      kbBuffer[--kbBufferLen] = '\0';
      drawChatInput();
    }
    return;
  }
  if (y >= 272 && y <= 312) {
    if (x >= 4 && x <= 140) {                                  // SPACE
      if (kbBufferLen < MSG_TEXT_MAX) {
        kbBuffer[kbBufferLen++] = ' ';
        kbBuffer[kbBufferLen] = '\0';
        drawChatInput();
      }
    } else if (x >= 148 && x <= 236 && kbBufferLen > 0) {      // SEND
      char text[MSG_TEXT_MAX + 1];
      strcpy(text, kbBuffer);
      kbBuffer[0] = '\0';
      kbBufferLen = 0;
      tft.fillRoundRect(8, 98, 224, 24, 8, COLOR_SURFACE);
      tft.setTextColor(COLOR_SUBTEXT);
      tft.setCursor(16, 106);
      tft.print("Sending...");
      sendChatMessage(text);
      chatScroll = 0;
      drawChatScreen();
    }
  }
}

void handleChatTouch(int x, int y) {
  if (chatConfirmDelete) {
    if (y >= 170 && y <= 214 && x >= 122 && x <= 210) {        // Delete
      char gone[DEVICE_NAME_MAX + 1];
      strcpy(gone, contacts[chatContact].name);
      deleteContact(chatContact);
      openMessages();
      snprintf(msgStatus, sizeof(msgStatus), "Deleted %s", gone);
      drawMessagesScreen();
    } else if (y >= 170 && y <= 214 && x >= 26 && x <= 114) {  // Keep
      chatConfirmDelete = false;
      drawChatScreen();
    }
    return;
  }
  if (y < 40) {                                                // header
    if (x < 60) {                                              // back: close keyboard first
      if (chatTyping) { chatTyping = false; drawChatScreen(); }
      else openMessages();
    } else if (x >= 188) {
      chatConfirmDelete = true;
      drawChatScreen();
    }
    return;
  }
  // a failed message: tap it to try again
  for (int i = 0; i < chatHitCount; i++) {
    if (y >= chatHitY0[i] && y <= chatHitY1[i] && msgList[chatHitMsg[i]].dir == 'f') {
      retryMessage(chatHitMsg[i]);
      return;
    }
  }
  if (chatTyping) { handleChatTypingTouch(x, y); return; }

  if (y >= 272 && y <= 312) {
    if (x >= 8 && x <= 164) {                                  // Write: keyboard slides in
      kbBuffer[0] = '\0';
      kbBufferLen = 0;
      kbMode = KB_MODE_ABC;
      kbShift = false;
      chatTyping = true;
      chatScroll = 0;
      drawChatScreen();
    } else if (x >= 170 && x <= 200 && chatCanScrollUp) {      // older
      chatScroll += 3;
      drawChatScreen();
    } else if (x >= 204 && x <= 234 && chatScroll > 0) {       // newer
      chatScroll = chatScroll > 3 ? chatScroll - 3 : 0;
      drawChatScreen();
    }
  }
}

// ============================================================
// NEWS -- headlines from NPR's public RSS feeds, by topic. Pick any mix of
// topics in the News app; the chosen ones are blended together in the list
// and in the scrolling ticker at the bottom of the Home screen.
// Feeds are read as a stream (never held whole in memory): only the
// <title> of each <item> is kept. Settings live in /news.txt as
// "topicMask,tickerOn".
// ============================================================
#define NEWS_CHIPS 6                   // topics you can pick in the News app
#define NEWS_CATS 7                    // ...plus the hidden Nvidia topic
#define NEWS_CAT_NVIDIA 6
#define NEWS_PER_CAT 6
#define NEWS_TITLE_MAX 110
#define NEWS_LINK_MAX 260
#define NEWS_DESC_MAX 400
struct NewsCat { const char* name; const char* url; };
const NewsCat newsCats[NEWS_CATS] = {
  {"Top",      "https://feeds.npr.org/1001/rss.xml"},
  {"Tech",     "https://feeds.npr.org/1019/rss.xml"},
  {"Politics", "https://feeds.npr.org/1014/rss.xml"},
  {"Business", "https://feeds.npr.org/1006/rss.xml"},
  {"Science",  "https://feeds.npr.org/1007/rss.xml"},
  {"Sports",   "https://feeds.npr.org/1055/rss.xml"},
  // Only while the secret Jensen theme is active: a Google News search for
  // Nvidia / Jensen Huang, blended in ahead of the other topics.
  {"Nvidia",   "https://news.google.com/rss/search?q=Nvidia+OR+%22Jensen+Huang%22&hl=en-US&gl=US&ceid=US:en"},
};
char newsTitles[NEWS_CATS][NEWS_PER_CAT][NEWS_TITLE_MAX + 1];
char newsLinks[NEWS_CATS][NEWS_PER_CAT][NEWS_LINK_MAX + 1];    // each story's web address
char newsDescs[NEWS_CATS][NEWS_PER_CAT][NEWS_DESC_MAX + 1];    // each story's short summary
int newsCount[NEWS_CATS] = {0, 0, 0, 0, 0, 0, 0};
unsigned long newsNextTryMs[NEWS_CATS] = {0, 0, 0, 0, 0, 0, 0};   // when each topic may be fetched again
int newsMixTheme = -1;                 // theme the blended list was built for
uint8_t newsMask = 0x03;               // which topics are on (bit per topic); default Top + Tech
bool newsTickerOn = true;
char newsStatus[40] = "";
int newsScroll = 0;

// the blended list: (topic, index) pairs, taking turns between topics
uint8_t newsMixCat[NEWS_CATS * NEWS_PER_CAT], newsMixIdx[NEWS_CATS * NEWS_PER_CAT];
int newsMixCount = 0;

uint16_t newsCatColor(int cat) {
  if (cat == NEWS_CAT_NVIDIA) return tft.color565(118, 185, 0);
  return rgb565(customAccents[(cat * 3 + 1) % 8]);
}

// Is this topic switched on? The six normal ones follow the chips; the
// Nvidia topic follows the Jensen theme.
bool newsCatOn(int cat) {
  if (cat == NEWS_CAT_NVIDIA) return currentTheme == THEME_SECRET;
  return newsMask & (1 << cat);
}
bool anyNewsCatOn() {
  for (int c = 0; c < NEWS_CATS; c++) if (newsCatOn(c)) return true;
  return false;
}

void loadNewsSettings() {
  if (!LittleFS.exists("/news.txt")) return;
  File f = LittleFS.open("/news.txt");
  if (!f) return;
  char buf[12];
  int i = 0;
  while (f.available() && i < 11) buf[i++] = f.read();
  buf[i] = '\0';
  f.close();
  int m = 3, t = 1;
  sscanf(buf, "%d,%d", &m, &t);
  newsMask = m & 0x3F;
  newsTickerOn = (t != 0);
}

void saveNewsSettings() {
  LittleFS.remove("/news.txt");
  File f = LittleFS.open("/news.txt", FILE_WRITE);
  if (f) { f.printf("%d,%d", newsMask, newsTickerOn ? 1 : 0); f.close(); }
}

void buildNewsMix() {
  newsMixCount = 0;
  newsMixTheme = currentTheme;
  for (int i = 0; i < NEWS_PER_CAT; i++)
    for (int k = 0; k < NEWS_CATS; k++) {
      int c = (k + NEWS_CAT_NVIDIA) % NEWS_CATS;            // Nvidia first, then Top, Tech, ...
      if (newsCatOn(c) && i < newsCount[c]) { newsMixCat[newsMixCount] = c; newsMixIdx[newsMixCount] = i; newsMixCount++; }
    }
  newsTickerDirty = true;
}

// Turns raw feed / web text into plain ASCII: drops CDATA wrappers and HTML
// tags (written plainly or as &lt;...&gt;), decodes the common &entities;,
// and maps curly quotes / dashes to plain ones.
void cleanNewsTitle(const char* raw, char* out, size_t outLen) {
  size_t j = 0;
  bool lastSpace = true;
  bool inTag = false;
  for (size_t i = 0; raw[i] && j < outLen - 1; ) {
    char c = raw[i];
    if (strncmp(raw + i, "<![CDATA[", 9) == 0) { i += 9; continue; }
    if (strncmp(raw + i, "]]>", 3) == 0) { i += 3; continue; }
    if (c == '&') {
      const char* semi = strchr(raw + i, ';');
      if (semi && semi - (raw + i) <= 8) {
        char ent[10];
        size_t n = semi - (raw + i) - 1;
        memcpy(ent, raw + i + 1, n); ent[n] = '\0';
        if (!strcmp(ent, "amp")) c = '&';
        else if (!strcmp(ent, "quot")) c = '"';
        else if (!strcmp(ent, "apos")) c = '\'';
        else if (!strcmp(ent, "lt")) c = '<';
        else if (!strcmp(ent, "gt")) c = '>';
        else if (!strcmp(ent, "nbsp")) c = ' ';
        else if (!strcmp(ent, "mdash") || !strcmp(ent, "ndash") || !strcmp(ent, "bull")) c = '-';
        else if (!strcmp(ent, "ldquo") || !strcmp(ent, "rdquo")) c = '"';
        else if (!strcmp(ent, "lsquo") || !strcmp(ent, "rsquo")) c = '\'';
        else if (!strcmp(ent, "hellip")) c = '.';
        else if (ent[0] == '#') {
          int v = (ent[1] == 'x' || ent[1] == 'X') ? (int)strtol(ent + 2, NULL, 16) : atoi(ent + 1);
          if (v == 8216 || v == 8217) c = '\'';
          else if (v == 8220 || v == 8221) c = '"';
          else if (v == 8211 || v == 8212) c = '-';
          else c = (v >= 32 && v < 127) ? (char)v : ' ';
        } else c = ' ';
        i = (semi - raw) + 1;
      } else i++;
    } else if ((unsigned char)c == 0xE2 && (unsigned char)raw[i + 1] == 0x80 && raw[i + 2]) {
      unsigned char k = raw[i + 2];                  // UTF-8 curly quotes and dashes
      c = (k == 0x98 || k == 0x99) ? '\'' : (k == 0x9C || k == 0x9D) ? '"' : (k == 0x93 || k == 0x94) ? '-' : ' ';
      i += 3;
    } else if ((unsigned char)c >= 0x80) {           // any other non-ASCII byte
      i++;
      continue;
    } else i++;
    if (c == '<') { inTag = true; continue; }       // skip <tags> entirely
    if (inTag) { if (c == '>') inTag = false; continue; }
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && lastSpace) continue;
    lastSpace = (c == ' ');
    out[j++] = c;
  }
  while (j > 0 && out[j - 1] == ' ') j--;
  out[j] = '\0';
}

// ---- streaming RSS reader: feed it one character at a time. For each
// <item> it keeps the <title>, <link> and <description>. ----
char newsWin[10];                    // the last few characters seen
int newsParseState = 0;              // 0 = before an <item>, 1 = inside an item, 2 = inside an opening tag, 3 = reading a field
int newsField = 0;                   // which field is being read: 0 title, 1 link, 2 description
char newsRaw[700];
int newsRawLen = 0;
char newsItemTitle[NEWS_TITLE_MAX + 1], newsItemLink[NEWS_LINK_MAX + 1], newsItemDesc[NEWS_DESC_MAX + 1];

void newsParserReset() { memset(newsWin, 0, sizeof(newsWin)); newsParseState = 0; newsRawLen = 0; }

bool newsWinEnds(const char* tail) {
  size_t n = strlen(tail);
  return memcmp(newsWin + 9 - n, tail, n) == 0;
}

// Returns true once the topic has all the headlines it can hold.
bool newsParserFeed(char c, int cat) {
  memmove(newsWin, newsWin + 1, 8);
  newsWin[8] = c;
  switch (newsParseState) {
    case 0:
      if (newsWinEnds("<item")) { newsParseState = 1; newsItemTitle[0] = newsItemLink[0] = newsItemDesc[0] = '\0'; }
      break;
    case 1:
      if (newsWinEnds("<title")) { newsField = 0; newsParseState = 2; }
      else if (newsWinEnds("<link")) { newsField = 1; newsParseState = 2; }
      else if (newsWinEnds("<descript")) { newsField = 2; newsParseState = 2; }
      else if (newsWinEnds("</item>")) {
        if (newsItemTitle[0] && newsCount[cat] < NEWS_PER_CAT) {
          int n = newsCount[cat]++;
          strcpy(newsTitles[cat][n], newsItemTitle);
          strcpy(newsLinks[cat][n], newsItemLink);
          strcpy(newsDescs[cat][n], newsItemDesc);
        }
        newsParseState = 0;
      }
      break;
    case 2: if (c == '>') { newsParseState = 3; newsRawLen = 0; } break;
    case 3: {
      if (newsRawLen < (int)sizeof(newsRaw) - 1) newsRaw[newsRawLen++] = c;
      const char* endTag = newsField == 0 ? "</title>" : newsField == 1 ? "</link>" : "</descri";
      if (newsWinEnds(endTag)) {
        int cut = strlen(endTag);
        if (newsRawLen < (int)sizeof(newsRaw) - 1) newsRawLen = newsRawLen >= cut ? newsRawLen - cut : 0;
        newsRaw[newsRawLen] = '\0';
        if (newsField == 0) cleanNewsTitle(newsRaw, newsItemTitle, sizeof(newsItemTitle));
        else if (newsField == 1) cleanNewsTitle(newsRaw, newsItemLink, sizeof(newsItemLink));
        else cleanNewsTitle(newsRaw, newsItemDesc, sizeof(newsItemDesc));
        newsParseState = 1;
      }
      break;
    }
  }
  return newsCount[cat] >= NEWS_PER_CAT;
}

// Downloads one topic's feed. Blocks for a second or two.
bool fetchNewsCategory(int cat) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);                        // plain (non-chunked) body, so it can be read as a stream
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  http.begin(client, String(newsCats[cat].url));
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    int kept = newsCount[cat];
    newsCount[cat] = 0;
    newsParserReset();
    WiFiClient* stream = http.getStreamPtr();
    unsigned long start = millis(), lastData = millis();
    bool full = false;
    while (!full && millis() - start < 15000 && millis() - lastData < 5000) {
      if (stream->available() > 0) {
        full = newsParserFeed((char)stream->read(), cat);
        lastData = millis();
      } else if (!http.connected()) break;
      else delay(2);
    }
    ok = newsCount[cat] > 0;
    if (!ok) newsCount[cat] = 0;
    (void)kept;
  }
  http.end();
  // fresh for 30 minutes; after a failure try again in 5
  newsNextTryMs[cat] = millis() + (ok ? 30UL * 60UL * 1000UL : 5UL * 60UL * 1000UL);
  buildNewsMix();
  return ok;
}

// Quietly keeps the chosen topics fresh while the PDA sits on Home or Lock:
// at most one topic every 15 seconds, so the screen never freezes for long.
unsigned long newsLastAutoMs = 0;
void newsAutoRefresh(bool touching) {
  if (touching || !anyNewsCatOn() || WiFi.status() != WL_CONNECTED) return;
  if (currentScreen != SCREEN_HOME && currentScreen != SCREEN_LOCK) return;
  if (millis() < 20000 || millis() - newsLastAutoMs < 15000) return;
  for (int c = 0; c < NEWS_CATS; c++) {
    if (!newsCatOn(c)) continue;
    if (newsNextTryMs[c] != 0 && (long)(millis() - newsNextTryMs[c]) < 0) continue;
    newsLastAutoMs = millis();
    fetchNewsCategory(c);
    newsLastAutoMs = millis();
    return;
  }
}

// ---- Home ticker: headlines sliding right-to-left along the bottom of the
// Home screen. Drawn into an off-screen strip first, so it doesn't flicker. ----
const int TICKER_X = 4, TICKER_Y = 288, TICKER_W = 232, TICKER_H = 15;
GFXcanvas16 tickerCanvas(TICKER_W, TICKER_H);
char newsTickerText[1500];
int newsTickerTextW = 0;
int newsTickerPos = 0;
unsigned long newsTickerMs = 0;

void rebuildTickerText() {
  newsTickerDirty = false;
  newsTickerText[0] = '\0';
  size_t len = 0;
  for (int i = 0; i < newsMixCount && i < 12; i++) {
    const char* t = newsTitles[newsMixCat[i]][newsMixIdx[i]];
    int n = snprintf(newsTickerText + len, sizeof(newsTickerText) - len, "%s      ", t);
    if (n < 0 || len + n >= sizeof(newsTickerText)) break;
    len += n;
  }
  tickerCanvas.setFont(&SynSmall);
  tickerCanvas.setTextWrap(false);
  int16_t x1, y1; uint16_t tw, th;
  tickerCanvas.getTextBounds(newsTickerText, 0, 0, &x1, &y1, &tw, &th);
  newsTickerTextW = tw;
  newsTickerPos = 0;
}

void drawNewsTicker() {
  if (newsMixTheme != currentTheme) buildNewsMix();     // the Jensen theme adds / removes the Nvidia topic
  if (newsTickerDirty) rebuildTickerText();
  const int labelW = 42;
  tickerCanvas.fillScreen(COLOR_SURFACE);
  tickerCanvas.setFont(&SynSmall);
  tickerCanvas.setTextWrap(false);
  tickerCanvas.setTextColor(COLOR_TEXT);
  if (!newsTickerOn) {
    tickerCanvas.setTextColor(COLOR_SUBTEXT);
    tickerCanvas.setCursor(labelW + 6, 11);
    tickerCanvas.print("Ticker disabled");
  } else if (newsMixCount == 0) {
    tickerCanvas.setTextColor(COLOR_SUBTEXT);
    tickerCanvas.setCursor(labelW + 6, 11);
    tickerCanvas.print(!anyNewsCatOn() ? "Tap to choose topics" : WiFi.status() == WL_CONNECTED ? "Getting headlines..." : "Waiting for WiFi");
  } else {
    tickerCanvas.setCursor(TICKER_W - newsTickerPos, 11);
    tickerCanvas.print(newsTickerText);
  }
  tickerCanvas.fillRect(0, 0, labelW, TICKER_H, COLOR_ACCENT);      // fixed "NEWS" label the text slides under
  tickerCanvas.setFont(&SynSmallBold);
  tickerCanvas.setTextColor(ILI9341_WHITE);
  tickerCanvas.setCursor(4, 11);
  tickerCanvas.print("NEWS");
  tft.drawRGBBitmap(TICKER_X, TICKER_Y, tickerCanvas.getBuffer(), TICKER_W, TICKER_H);
}

// Called every loop() on Home: slides the ticker along. (When the ticker is
// switched off the bar stays put, showing "Ticker disabled".)
void updateNewsTicker() {
  if (!newsTickerOn || msgBanner[0]) return;
  if (millis() - newsTickerMs < 40) return;
  newsTickerMs = millis();
  if (newsMixCount == 0 && !newsTickerDirty) {
    static unsigned long lastIdleDraw = 0;               // nothing to scroll: just refresh the message now and then
    if (millis() - lastIdleDraw < 2000) return;
    lastIdleDraw = millis();
  }
  newsTickerPos += 2;
  if (newsTickerPos > TICKER_W + newsTickerTextW) newsTickerPos = 0;
  drawNewsTicker();
}

// ---- News app ----
const int NEWS_LIST_Y = 88, NEWS_ROW_H = 28, NEWS_ROWS = 6;

void openNews() {
  newsScroll = 0;
  newsStatus[0] = '\0';
  currentScreen = SCREEN_NEWS;
  drawNewsScreen();
}

// Wraps `text` into at most two lines no wider than maxW pixels (small UI
// font); the second line ends in "..." when the headline is longer.
void wrapTwoLines(const char* text, int maxW, char* l1, char* l2, size_t len) {
  tft.setFont();
  tft.setTextSize(1);
  int16_t x1, y1; uint16_t tw, th;
  size_t n = strlen(text), cut = n;
  char tmp[NEWS_TITLE_MAX + 4];
  // longest prefix ending at a word break that fits on line 1
  for (size_t i = 0; i <= n; i++) {
    if (text[i] != ' ' && text[i] != '\0') continue;
    size_t k = i < sizeof(tmp) - 1 ? i : sizeof(tmp) - 1;
    memcpy(tmp, text, k); tmp[k] = '\0';
    tft.getTextBounds(tmp, 0, 0, &x1, &y1, &tw, &th);
    if ((int)tw > maxW) break;
    cut = i;
  }
  if (cut == n && n > 0) {                       // fits (or one unbreakable word): all on line 1
    tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
    if ((int)tw <= maxW) { snprintf(l1, len, "%s", text); l2[0] = '\0'; return; }
    cut = n * maxW / (tw + 1);
  }
  size_t k = cut < len - 1 ? cut : len - 1;
  memcpy(l1, text, k); l1[k] = '\0';
  const char* rest = text + cut;
  while (*rest == ' ') rest++;
  snprintf(l2, len, "%s", rest);
  for (;;) {                                     // trim line 2 until it fits, adding "..."
    tft.getTextBounds(l2, 0, 0, &x1, &y1, &tw, &th);
    size_t m = strlen(l2);
    if ((int)tw <= maxW || m < 5) break;
    char* sp = strrchr(l2, ' ');
    size_t at = (sp && sp > l2 + 4) ? (size_t)(sp - l2) : m - 4;
    if (at + 4 > len) at = len - 4;
    strcpy(l2 + at, "...");
    if (m == strlen(l2)) { l2[m - 4] = '\0'; strcat(l2, "..."); if (m <= 8) break; }
  }
}

void drawNewsScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("News");
  if (newsMixTheme != currentTheme) buildNewsMix();

  // topic chips: tap to turn each one on or off
  for (int c = 0; c < NEWS_CHIPS; c++) {
    int x = 8 + (c % 3) * 76, y = 30 + (c / 3) * 28;
    bool on = newsMask & (1 << c);
    drawGlassRect(x, y, 72, 23, 11, on ? newsCatColor(c) : COLOR_SURFACE);
    printFitted(&SynSmallBold, x + 36, y + 6, newsCats[c].name, on ? ILI9341_WHITE : COLOR_SUBTEXT, 66);
  }

  int maxScroll = newsMixCount > NEWS_ROWS ? newsMixCount - NEWS_ROWS : 0;
  if (newsScroll > maxScroll) newsScroll = maxScroll;
  if (newsMixCount == 0) {
    const char* msg = !anyNewsCatOn() ? "Pick at least one topic above" : "No headlines yet - tap Refresh";
    printCentered(&FreeSans9pt7b, 120, 150, msg, COLOR_SUBTEXT);
  }
  char l1[NEWS_TITLE_MAX + 1], l2[NEWS_TITLE_MAX + 1];
  for (int row = 0; row < NEWS_ROWS && row + newsScroll < newsMixCount; row++) {
    int cat = newsMixCat[row + newsScroll];
    int y = NEWS_LIST_Y + row * NEWS_ROW_H;
    tft.fillRoundRect(8, y, 4, 23, 2, newsCatColor(cat));            // topic color stripe
    wrapTwoLines(newsTitles[cat][newsMixIdx[row + newsScroll]], 212, l1, l2, sizeof(l1));
    tft.setTextColor(COLOR_TEXT);
    tft.setCursor(18, y + (l2[0] ? 0 : 6));
    tft.print(l1);
    if (l2[0]) { tft.setCursor(18, y + 12); tft.print(l2); }
  }

  // status line + scroll arrows
  tft.setFont();
  tft.setTextColor(COLOR_SUBTEXT);
  tft.setCursor(10, 262);
  tft.print(newsStatus[0] ? newsStatus : newsMixCount > 0 ? "Tap a headline to read it" : "Headlines from NPR");
  if (maxScroll > 0) {
    tft.fillTriangle(196, 274, 204, 262, 212, 274, newsScroll > 0 ? COLOR_TEXT : COLOR_FRAME);
    tft.fillTriangle(218, 262, 226, 274, 234, 262, newsScroll < maxScroll ? COLOR_TEXT : COLOR_FRAME);
  }

  drawGlassRect(8, 282, 110, 30, 14, newsTickerOn ? COLOR_ACCENT : COLOR_SUBTEXT);
  printFitted(&FreeSansBold9pt7b, 63, 290, newsTickerOn ? "Ticker: On" : "Ticker: Off", ILI9341_WHITE, 102);
  drawGlassRect(124, 282, 108, 30, 14, COLOR_BLUE);
  printFitted(&FreeSansBold9pt7b, 178, 290, "Refresh", ILI9341_WHITE, 100);
}

void refreshAllNews() {
  if (WiFi.status() != WL_CONNECTED) { strcpy(newsStatus, "No WiFi"); drawNewsScreen(); return; }
  int ok = 0, tried = 0;
  for (int c = 0; c < NEWS_CATS; c++) {
    if (!newsCatOn(c)) continue;
    snprintf(newsStatus, sizeof(newsStatus), "Updating %s...", newsCats[c].name);
    drawNewsScreen();
    tried++;
    if (fetchNewsCategory(c)) ok++;
  }
  if (tried == 0) strcpy(newsStatus, "Pick a topic first");
  else if (ok == tried) {
    DateTime now = clockNow();
    char t12[9];
    formatTime12h(t12, now.hour(), now.minute());
    snprintf(newsStatus, sizeof(newsStatus), "Updated %s", t12);
  } else snprintf(newsStatus, sizeof(newsStatus), "Couldn't load %d of %d topics", tried - ok, tried);
  newsScroll = 0;
  drawNewsScreen();
}

void handleNewsTouch(int x, int y) {
  for (int c = 0; c < NEWS_CHIPS; c++) {                             // topic chips
    int cx = 8 + (c % 3) * 76, cy = 30 + (c / 3) * 28;
    if (x >= cx && x <= cx + 72 && y >= cy && y <= cy + 23) {
      newsMask ^= (1 << c);
      saveNewsSettings();
      buildNewsMix();
      newsScroll = 0;
      newsStatus[0] = '\0';
      drawNewsScreen();
      return;
    }
  }
  if (y >= 282 && y <= 312) {
    if (x >= 8 && x <= 118) {                                        // ticker on/off
      newsTickerOn = !newsTickerOn;
      saveNewsSettings();
      drawNewsScreen();
    } else if (x >= 124 && x <= 232) {
      refreshAllNews();
    }
    return;
  }
  for (int row = 0; row < NEWS_ROWS && row + newsScroll < newsMixCount; row++) {   // a headline: open it
    int ry = NEWS_LIST_Y + row * NEWS_ROW_H;
    if (y >= ry && y < ry + NEWS_ROW_H - 2) {
      openArticle(newsMixCat[row + newsScroll], newsMixIdx[row + newsScroll]);
      return;
    }
  }
  int maxScroll = newsMixCount > NEWS_ROWS ? newsMixCount - NEWS_ROWS : 0;
  if (maxScroll > 0 && y >= 256 && y <= 280 && x >= 190) {            // scroll arrows
    if (x < 215) newsScroll = newsScroll > 3 ? newsScroll - 3 : 0;
    else newsScroll = newsScroll + 3 < maxScroll ? newsScroll + 3 : maxScroll;
    drawNewsScreen();
  }
}

// ============================================================
// NEWS ARTICLE -- tap a headline to see its summary (from the feed), then
// "Read full story" downloads NPR's plain-text version of the article
// (text.npr.org) and shows it a page at a time.
// ============================================================
#define ARTICLE_MAX 7000
#define ARTICLE_MAX_PAGES 40
char articleText[ARTICLE_MAX + 1];
int articleLen = 0;
int articleCat = 0, articleIdx = 0;
bool articleFull = false;            // false = summary view, true = paging through the full text
int articlePageStart[ARTICLE_MAX_PAGES + 1];
int articlePages = 0, articlePage = 0;
char articleStatus[40] = "";

int fontCharW(const GFXfont* f, char c) {
  if ((uint8_t)c < f->first || (uint8_t)c > f->last) return 0;
  return f->glyph[(uint8_t)c - f->first].xAdvance;
}

// Word-wraps text[start...] into lines at most `w` pixels wide and draws up
// to maxLines of them (or only measures when draw is false). '\n' starts a
// new paragraph with a little gap. Returns the index where it stopped.
int drawWrapped(const GFXfont* f, const char* text, int start, int x, int y, int w, int lineH, int maxLines, bool draw, uint16_t color, int* linesUsed) {
  bool small = (f == &SynSmall);
  if (draw) {
    if (small) tft.setFont(); else tft.setFont(f);
    tft.setTextSize(1);
    tft.setTextColor(color);
  }
  int pos = start, line = 0;
  char buf[80];
  while (text[pos] && line < maxLines) {
    if (text[pos] == '\n') {                      // paragraph break: skip a line, unless at the top of a page
      pos++;
      if (line > 0) line++;
      continue;
    }
    int width = 0, i = pos, lastBreak = -1;
    while (text[i] && text[i] != '\n') {
      int cw = fontCharW(f, text[i]);
      if (width + cw > w) break;
      width += cw;
      if (text[i] == ' ') lastBreak = i;
      i++;
      if (i - pos >= (int)sizeof(buf) - 1) break;
    }
    int end = i;
    if (text[i] && text[i] != '\n' && lastBreak > pos) end = lastBreak;     // break at the last space
    if (end == pos) end = pos + 1;                                         // always make progress
    if (draw) {
      int n = end - pos;
      memcpy(buf, text + pos, n); buf[n] = '\0';
      tft.setCursor(x, small ? y + line * lineH : y + line * lineH + 13);
      tft.print(buf);
    }
    pos = end;
    while (text[pos] == ' ') pos++;
    line++;
  }
  if (draw) tft.setFont();
  if (linesUsed) *linesUsed = line;
  return pos;
}

const int ART_TEXT_Y = 32, ART_LINE_H = 12, ART_LINES = 20;

void paginateArticle() {
  articlePages = 0;
  int pos = 0;
  while (articleText[pos] && articlePages < ARTICLE_MAX_PAGES) {
    articlePageStart[articlePages++] = pos;
    int used;
    pos = drawWrapped(&SynSmall, articleText, pos, 10, ART_TEXT_Y, 220, ART_LINE_H, ART_LINES, false, 0, &used);
    while (articleText[pos] == '\n') pos++;
  }
  articlePageStart[articlePages] = pos;
}

void openArticle(int cat, int idx) {
  articleCat = cat; articleIdx = idx;
  articleFull = false;
  articleStatus[0] = '\0';
  currentScreen = SCREEN_ARTICLE;
  drawArticleScreen();
}

void drawArticleScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar(newsCats[articleCat].name);

  if (articleFull) {
    drawWrapped(&SynSmall, articleText, articlePageStart[articlePage], 10, ART_TEXT_Y, 220, ART_LINE_H, ART_LINES, true, COLOR_TEXT, NULL);
    drawGlassRect(8, 284, 72, 28, 12, articlePage > 0 ? COLOR_SETTINGS : COLOR_FRAME);
    printFitted(&FreeSansBold9pt7b, 44, 291, "Prev", ILI9341_WHITE, 64);
    char pg[16];
    snprintf(pg, sizeof(pg), "%d / %d", articlePage + 1, articlePages);
    printCentered(&FreeSans9pt7b, 120, 291, pg, COLOR_SUBTEXT);
    drawGlassRect(160, 284, 72, 28, 12, articlePage < articlePages - 1 ? COLOR_ACCENT : COLOR_FRAME);
    printFitted(&FreeSansBold9pt7b, 196, 291, "Next", ILI9341_WHITE, 64);
    return;
  }

  // summary view: headline, topic, then the feed's summary
  tft.fillRoundRect(8, 32, 4, 14, 2, newsCatColor(articleCat));
  int used = 0;
  drawWrapped(&FreeSansBold9pt7b, newsTitles[articleCat][articleIdx], 0, 18, 30, 212, 19, 5, true, COLOR_TEXT, &used);
  int y = 30 + used * 19 + 10;
  tft.drawFastHLine(10, y - 4, 220, COLOR_FRAME);
  const char* desc = newsDescs[articleCat][articleIdx];
  bool nvidia = (articleCat == NEWS_CAT_NVIDIA);
  if (nvidia) desc = "This headline comes from Google News. The full story is on the publisher's website, which the PDA can't open.";
  else if (!desc[0]) desc = "No summary came with this story.";
  int maxLines = (262 - y) / 13;
  drawWrapped(&SynSmall, desc, 0, 10, y, 220, 13, maxLines, true, COLOR_SUBTEXT, NULL);

  tft.setFont();
  if (articleStatus[0]) {
    tft.setTextColor(COLOR_ERROR);
    tft.setCursor(10, 266);
    tft.print(articleStatus);
  }
  if (!nvidia) {
    drawGlassRect(20, 282, 200, 30, 14, COLOR_ACCENT);
    printFitted(&FreeSansBold9pt7b, 120, 290, "Read full story", ILI9341_WHITE, 190);
  }
}

// NPR links look like https://www.npr.org/2025/11/15/nx-s1-5609712/some-title
// (older ones use a long number instead of nx-s1-...). The text-only site
// serves the same story at https://text.npr.org/<that id>.
bool nprTextUrl(const char* link, char* out, size_t len) {
  const char* p = strstr(link, "npr.org/");
  if (!p) return false;
  p += 8;
  for (int seg = 0; seg < 3; seg++) {             // skip /YYYY/MM/DD/
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) p++;
    if (*p != '/') return false;
    p++;
  }
  char id[40];
  size_t n = 0;
  while (p[n] && p[n] != '/' && p[n] != '?' && n < sizeof(id) - 1) { id[n] = p[n]; n++; }
  id[n] = '\0';
  if (n < 5) return false;
  snprintf(out, len, "https://text.npr.org/%s", id);
  return true;
}

// ---- streaming reader for the text-only article page: after the <h1>
// headline, keep the text of every <p>...</p> until </article>. ----
int artState = 0;                  // 0 = before <h1, 1 = between paragraphs, 2 = inside a <p ...> tag, 3 = paragraph text, 4 = done
char artRaw[1200];
int artRawLen = 0;

void articleParserReset() { memset(newsWin, 0, sizeof(newsWin)); artState = 0; artRawLen = 0; articleLen = 0; articleText[0] = '\0'; }

void articleParserFeed(char c) {
  memmove(newsWin, newsWin + 1, 8);
  newsWin[8] = c;
  switch (artState) {
    case 0: if (newsWinEnds("<h1")) artState = 1; break;
    case 1:
      if (newsWinEnds("</article") || newsWinEnds("<footer")) artState = 4;
      else if (newsWinEnds("<p>")) { artState = 3; artRawLen = 0; }
      else if (newsWinEnds("<p ")) artState = 2;
      break;
    case 2: if (c == '>') { artState = 3; artRawLen = 0; } break;
    case 3:
      if (artRawLen < (int)sizeof(artRaw) - 1) artRaw[artRawLen++] = c;
      if (newsWinEnds("</p>")) {
        if (artRawLen < (int)sizeof(artRaw) - 1) artRawLen = artRawLen >= 4 ? artRawLen - 4 : 0;
        artRaw[artRawLen] = '\0';
        char clean[1200];
        cleanNewsTitle(artRaw, clean, sizeof(clean));
        int n = strlen(clean);
        if (n > 0 && articleLen + n + 2 < ARTICLE_MAX) {
          memcpy(articleText + articleLen, clean, n);
          articleLen += n;
          articleText[articleLen++] = '\n';
          articleText[articleLen] = '\0';
        } else if (n > 0) {
          artState = 4;                            // out of room: stop here
          return;
        }
        artState = 1;
      }
      break;
  }
}

void loadFullArticle() {
  char url[80];
  if (WiFi.status() != WL_CONNECTED) { strcpy(articleStatus, "No WiFi"); drawArticleScreen(); return; }
  if (!nprTextUrl(newsLinks[articleCat][articleIdx], url, sizeof(url))) {
    strcpy(articleStatus, "Full story isn't available");
    drawArticleScreen();
    return;
  }
  tft.fillRect(8, 262, 224, 52, COLOR_BG);
  printCentered(&FreeSans9pt7b, 120, 284, "Loading story...", COLOR_SUBTEXT);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  http.begin(client, String(url));
  int code = http.GET();
  if (code == 200) {
    articleParserReset();
    WiFiClient* stream = http.getStreamPtr();
    unsigned long start = millis(), lastData = millis();
    while (artState != 4 && millis() - start < 20000 && millis() - lastData < 5000) {
      if (stream->available() > 0) { articleParserFeed((char)stream->read()); lastData = millis(); }
      else if (!http.connected()) break;
      else delay(2);
    }
  }
  http.end();

  if (code == 200 && articleLen > 0) {
    paginateArticle();
    articlePage = 0;
    articleFull = true;
    articleStatus[0] = '\0';
  } else if (code == 200) {
    strcpy(articleStatus, "Couldn't read that story");
  } else {
    snprintf(articleStatus, sizeof(articleStatus), "Couldn't load it (%d)", code);
  }
  drawArticleScreen();
}

void handleArticleTouch(int x, int y) {
  if (articleFull) {
    if (y < 276) return;
    if (x < 100 && articlePage > 0) { articlePage--; drawArticleScreen(); }
    else if (x > 140 && articlePage < articlePages - 1) { articlePage++; drawArticleScreen(); }
    return;
  }
  if (articleCat != NEWS_CAT_NVIDIA && x >= 20 && x <= 220 && y >= 282 && y <= 312) loadFullArticle();
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

void handleWebTime() {
  DateTime now = clockNow();
  long secs = (long)now.hour() * 3600 + now.minute() * 60 + now.second();
  server.send(200, "text/plain", String(secs));
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

// Centered text that steps down a size when it doesn't fit: the font asked
// for, then bold 9pt (for the big fonts), then the small bold UI font.
void printFitted(const GFXfont* font, int centerX, int topY, const char* text, uint16_t color, int maxWidth) {
  const GFXfont* tries[3] = {font, &FreeSansBold9pt7b, &SynSmallBold};
  int dropY[3] = {0, 2, 5};          // smaller text sits a little lower to stay centered
  bool big = (font == &FreeSans12pt7b || font == &FreeSansBold12pt7b ||
              font == &FreeSansBold18pt7b || font == &FreeSansBold24pt7b);
  int16_t x1, y1; uint16_t tw, th;
  for (int i = 0; i < 3; i++) {
    if (i == 1 && !big) continue;
    tft.setFont(tries[i]);
    tft.setTextSize(1);
    tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
    if ((int)tw <= maxWidth || i == 2) {
      tft.setTextColor(color);
      tft.setCursor(centerX - (int)(tw / 2) - x1, topY + (big ? dropY[i] : (i == 2 ? 2 : 0)) - y1);
      tft.print(text);
      break;
    }
  }
  tft.setFont();
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
void iconChat(int cx, int cy, uint16_t color) {
  tft.fillRoundRect(cx - 10, cy - 8, 20, 14, 4, color);
  tft.fillTriangle(cx - 6, cy + 5, cx - 1, cy + 5, cx - 7, cy + 10, color);
  for (int d = -5; d <= 5; d += 5) tft.fillCircle(cx + d, cy - 1, 1, ILI9341_WHITE);
}
void iconGrid(int cx, int cy, uint16_t color) {
  int spacing = 6;
  for (int row = -1; row <= 1; row++)
    for (int col = -1; col <= 1; col++)
      tft.fillCircle(cx + col * spacing, cy + row * spacing, 1, color);
}

void drawTileIcon(int i, int radius) {
  int cx = tileX[i] + tileW / 2;
  int cy = tileY[i] + 21;
  tft.fillRect(cx - 18, cy - 18, 37, 37, tileColors[i]);
  tft.fillCircle(cx, cy, radius, ILI9341_WHITE);
  switch (i) {
    case 1: iconCheckbox(cx, cy, tileColors[i]); break;
    case 2: iconPencil(cx, cy, tileColors[i]); break;
    case 3: iconBook(cx, cy, tileColors[i]); break;
    case 4: iconSun(cx, cy, tileColors[i]); break;
    case 5: iconGear(cx, cy, tileColors[i]); break;
    case 6: iconChat(cx, cy, tileColors[i]); break;
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
  for (int i = 1; i < TILE_COUNT; i++) {
    unsigned long period = 5200 + i * 900;
    unsigned long phase = i * 1400;
    float t = (float)((millis() + phase) % period) / (float)period;
    float wave = sin(t * 2 * PI);
    int radius = TILE_ICON_R + (int)(wave * 2);
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
  // remember it, so a tap can flash it (see flashPressedButton)
  if (buttonRectCount < MAX_BUTTON_RECTS) buttonRects[buttonRectCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)r, baseColor};
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

// ---- Press feedback: every button drawn with drawGlassRect is remembered;
// a tap draws a dark ring just inside the one under the finger before its
// action runs. If the action didn't redraw the screen, the ring is removed
// again afterwards. ----
uint16_t darkerColor(uint16_t c, int amt) {
  int rr = (c >> 11) & 0x1F, gg = (c >> 5) & 0x3F, bb = c & 0x1F;
  rr = rr > amt ? rr - amt : 0; gg = gg > amt * 2 ? gg - amt * 2 : 0; bb = bb > amt ? bb - amt : 0;
  return (rr << 11) | (gg << 5) | bb;
}

int pressedButton = -1;

void drawButtonRing(const ButtonRect& b, uint16_t color) {
  for (int i = 1; i <= 2; i++)
    tft.drawRoundRect(b.x + i, b.y + i, b.w - 2 * i, b.h - 2 * i, b.r > i ? b.r - i : 1, color);
}

void flashPressedButton(int x, int y) {
  pressedButton = -1;
  for (int i = buttonRectCount - 1; i >= 0; i--) {       // newest (topmost) first
    ButtonRect& b = buttonRects[i];
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
      drawButtonRing(b, darkerColor(b.color, 9));
      pressedButton = i;
      delay(25);
      return;
    }
  }
}

// After the tap was handled: if that button is still on screen, un-ring it.
void releasePressedButton(ButtonRect was) {
  for (int i = buttonRectCount - 1; i >= 0; i--) {
    ButtonRect& b = buttonRects[i];
    if (b.x == was.x && b.y == was.y && b.w == was.w && b.h == was.h) { drawButtonRing(b, b.color); return; }
  }
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
  // back chevron + label
  tft.drawLine(12, 7, 7, 12, ILI9341_WHITE);
  tft.drawLine(7, 12, 12, 17, ILI9341_WHITE);
  tft.drawLine(13, 7, 8, 12, ILI9341_WHITE);
  tft.drawLine(8, 12, 13, 17, ILI9341_WHITE);
  tft.setCursor(18, 7);
  tft.print("Back");
  // title in the small bold font, centered
  tft.setFont(&SynSmallBold);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(title, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((240 - tw) / 2 - x1, 16);
  tft.print(title);
  tft.setFont();
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
    char c = row[i];
    if (upperCase) c = toupper(c);
    char label[2] = {c, '\0'};
    // bold 9pt letter, centered on the key
    tft.setFont(&FreeSansBold9pt7b);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    int16_t x1, y1; uint16_t tw, th;
    tft.getTextBounds("x", 0, 0, &x1, &y1, &tw, &th);       // x-height: keeps a common baseline
    int baseline = y + keyH / 2 + 5;
    tft.getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor(x + 11 - (int)tw / 2 - x1, baseline);
    tft.print(label);
    tft.setFont();
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
  if (kbContext == KB_CONTEXT_MSG_IP) return 15;
  return 36;
}

// The text box at the top of the keyboard. Typing redraws only this, not
// the whole screen, so keys respond quickly.
void drawKeyboardInput() {
  tft.fillRoundRect(10, 30, 220, 40, 12, COLOR_SURFACE);
  tft.drawRoundRect(10, 30, 220, 40, 12, COLOR_SUBTEXT);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT);
  // Show the end of the text (the part being typed) when it's wider than the box.
  const char* shown = kbBuffer;
  int16_t x1, y1; uint16_t tw, th;
  for (;;) {
    tft.getTextBounds(shown, 0, 0, &x1, &y1, &tw, &th);
    if (tw <= 200 || !*shown) break;
    shown++;
  }
  tft.setCursor(18, 56);
  tft.print(shown);
  tft.fillRect(18 + tw + 3, 40, 2, 20, COLOR_ACCENT);    // cursor bar
  tft.setFont();
}

void drawKeyboardScreen() {
  tft.fillScreen(COLOR_BG);
  const char* title = "Add Task";
  if (kbContext == KB_CONTEXT_ADD_HOMEWORK) title = "Add Homework";
  if (kbContext == KB_CONTEXT_WIFI_PASSWORD) title = "WiFi Password";
  if (kbContext == KB_CONTEXT_DEVICE_NAME) title = "Name Your Device";
  if (kbContext == KB_CONTEXT_MSG_IP) title = "Send To (IP)";
  drawBackBar(title);
  drawKeyboardInput();

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
      drawKeyboardInput();
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
      drawKeyboardInput();
    }
    return;
  }

  if (x >= 4 && x <= 144 && y >= 254 && y <= 300) { // SPACE
    if (kbBufferLen < kbMaxLen()) {
      kbBuffer[kbBufferLen++] = ' ';
      kbBuffer[kbBufferLen] = '\0';
      drawKeyboardInput();
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
        DateTime now = clockNow();
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
    } else if (kbContext == KB_CONTEXT_MSG_IP) {
      openMessages();
      if (kbBufferLen > 0) {
        strncpy(newChatIp, kbBuffer, sizeof(newChatIp) - 1);
        newChatIp[sizeof(newChatIp) - 1] = '\0';
        sendConnectRequest(newChatIp);
        if (currentScreen == SCREEN_MESSAGES) drawMessagesScreen();
      }
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
    DateTime now = clockNow();
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
      drawHomeScreen(clockNow());
      lastDrawnMinute = clockNow().minute();
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

int ipLineY() { return currentScreen == SCREEN_LOCK ? 296 : 306; }

void drawIpLine() {
  buildIpText(shownIpText, sizeof(shownIpText));
  int y = ipLineY();
  if (currentScreen == SCREEN_LOCK) drawLockBackground(y - 2, y + 12);
  else tft.fillRect(4, y - 2, 232, 12, COLOR_BG);
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
// UI: LOCK SCREEN -- soft theme gradient, big clock, a card for the
// current class with a progress bar, then cards for unread messages and
// the weather.
// ============================================================
const char* dayNamesFull[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* dayNamesShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* monthNamesShort[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Mix two 565 colors: pct = 0 gives a, 100 gives b.
uint16_t blendColor(uint16_t a, uint16_t b, int pct) {
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (br - ar) * pct / 100, g = ag + (bg - ag) * pct / 100, bl = ab + (bb - ab) * pct / 100;
  return (r << 11) | (g << 5) | bl;
}

// The lock/boot background: the theme background fading toward its accent.
uint16_t lockBgAt(int y) { return blendColor(COLOR_BG, COLOR_ACCENT, y * 38 / 319); }

void drawLockBackground(int y0, int y1) {
  for (int y = y0; y < y1; y++) tft.drawFastHLine(0, y, 240, lockBgAt(y));
}

// A flat rounded card (no shadow) used on the lock screen.
void drawLockCard(int y, int h) {
  tft.fillRoundRect(14, y, 212, h, 12, COLOR_SURFACE);
  tft.drawRoundRect(14, y, 212, h, 12, COLOR_FRAME);
}

// Index of the next class that hasn't started yet today, or -1.
int nextPeriodIndex(int nowMinutes) {
  int next = -1;
  for (int i = 0; i < numPeriods; i++) {
    int start = schedule[i].startHour * 60 + schedule[i].startMin;
    if (start > nowMinutes && (next == -1 || start < schedule[next].startHour * 60 + schedule[next].startMin)) next = i;
  }
  return next;
}

void drawLockScreen(DateTime now) {
  buttonRectCount = 0;
  drawLockBackground(0, 320);

  char buf[48];
  snprintf(buf, sizeof(buf), "%s, %s %d", dayNamesFull[now.dayOfTheWeek()], monthNamesShort[now.month() - 1], now.day());
  printFitted(&FreeSans12pt7b, 120, 22, buf, COLOR_SUBTEXT, 224);

  char timeStr[9];
  formatTime12h(timeStr, now.hour(), now.minute());
  printCentered(&FreeSansBold24pt7b, 120, 54, timeStr, COLOR_TEXT);

  // ---- class card ----
  int nowMinutes = now.hour() * 60 + now.minute();
  int idx = getCurrentPeriodIndex(now.hour(), now.minute());
  drawLockCard(108, 56);
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(COLOR_SUBTEXT);
  tft.setCursor(26, 115);
  if (idx != -1) {
    int startMinutes = schedule[idx].startHour * 60 + schedule[idx].startMin;
    int endMinutes = schedule[idx].endHour * 60 + schedule[idx].endMin;
    tft.print("Now");
    snprintf(buf, sizeof(buf), "%d min left", endMinutes - nowMinutes);
    int16_t x1, y1; uint16_t tw, th;
    tft.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor(214 - tw, 115);
    tft.print(buf);
    tft.setFont(&FreeSansBold9pt7b);
    tft.setTextColor(COLOR_TEXT);
    tft.setCursor(26, 143);
    tft.print(schedule[idx].name);
    tft.setFont();
    // progress through the period
    int span = endMinutes - startMinutes;
    int done = span > 0 ? (nowMinutes - startMinutes) * 188 / span : 0;
    tft.fillRoundRect(26, 151, 188, 6, 3, COLOR_FRAME);
    if (done > 4) tft.fillRoundRect(26, 151, done, 6, 3, COLOR_ACCENT);
  } else {
    int next = nextPeriodIndex(nowMinutes);
    tft.print(next == -1 ? "Today" : "Next up");
    tft.setFont(&FreeSansBold9pt7b);
    tft.setTextColor(COLOR_TEXT);
    tft.setCursor(26, 143);
    tft.print(next == -1 ? "No more classes" : schedule[next].name);
    tft.setFont();
    if (next != -1) {
      char t12[9];
      formatTime12h(t12, schedule[next].startHour, schedule[next].startMin);
      snprintf(buf, sizeof(buf), "Starts at %s", t12);
      tft.setTextColor(COLOR_SUBTEXT);
      tft.setCursor(26, 149);
      tft.print(buf);
    }
  }

  // ---- notification cards: unread chats first, then the weather ----
  int cardY = 172, cards = 0;
  for (int i = 0; i < contactCount && cards < 2; i++) {
    if (contacts[i].unread == 0) continue;
    drawLockCard(cardY, 36);
    drawContactAvatar(34, cardY + 18, 12, contacts[i]);
    tft.setFont(&SynSmallBold);
    tft.setTextColor(COLOR_TEXT);
    tft.setCursor(54, cardY + 15);
    tft.print(contacts[i].name);
    tft.setFont();
    int last = lastMessageFor(contacts[i].ip);
    char preview[30];
    if (contacts[i].unread > 1) snprintf(preview, sizeof(preview), "%d new messages", contacts[i].unread);
    else snprintf(preview, sizeof(preview), "%s", last >= 0 ? msgList[last].text : "New message");
    tft.setTextColor(COLOR_SUBTEXT);
    tft.setCursor(54, cardY + 19);
    tft.print(preview);
    tft.fillCircle(212, cardY + 18, 4, COLOR_ERROR);
    cardY += 42; cards++;
  }
  if (cards < 2 && strcmp(weatherTemp, "--") != 0) {
    drawLockCard(cardY, 36);
    tft.fillCircle(34, cardY + 18, 12, COLOR_WEATHER);
    iconSun(34, cardY + 18, ILI9341_WHITE);
    snprintf(buf, sizeof(buf), "%s F", weatherTemp);
    tft.setFont(&SynSmallBold);
    tft.setTextColor(COLOR_TEXT);
    tft.setCursor(54, cardY + 15);
    tft.print(buf);
    tft.setFont();
    snprintf(buf, sizeof(buf), "%s - %s", weatherDesc, WEATHER_LOCATION_LABEL);
    tft.setTextColor(COLOR_SUBTEXT);
    tft.setCursor(54, cardY + 19);
    tft.print(buf);
  }

  // ---- unlock hint ----
  tft.setFont();
  tft.setTextColor(COLOR_TEXT);
  const char* hint = "Tap anywhere to unlock";
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(hint, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((240 - tw) / 2, 276);
  tft.print(hint);

  drawIpLine();
}

// ============================================================
// BOOT SCREEN -- the logo grows ring by ring on the lock gradient while
// the PDA finishes starting up (WiFi, web server, clock).
// ============================================================
void drawBootScreen() {
  drawLockBackground(0, 320);
  int cx = 120, cy = 128;
  for (int r = 2; r <= 16; r += 2) { tft.fillCircle(cx, cy, r, COLOR_ACCENT); delay(18); }
  delay(60);
  tft.drawCircle(cx, cy, 27, COLOR_ACCENT); tft.drawCircle(cx, cy, 28, COLOR_ACCENT);
  delay(90);
  tft.drawCircle(cx, cy, 39, COLOR_ACCENT); tft.drawCircle(cx, cy, 40, COLOR_ACCENT);
  delay(90);
  printCentered(&FreeSansBold18pt7b, 120, 186, "SynRG", COLOR_TEXT);
  if (currentTheme == THEME_SECRET) {
    printCentered(&FreeSans9pt7b, 120, 226, "The more you buy,", COLOR_ACCENT);
    printCentered(&FreeSans9pt7b, 120, 246, "the more you save.", COLOR_ACCENT);
  }
  tft.setTextColor(COLOR_SUBTEXT);
  tft.setTextSize(1);
  char v[24];
  snprintf(v, sizeof(v), "%s  -  starting up", FIRMWARE_VERSION);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(v, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((240 - tw) / 2, 296);
  tft.print(v);
}

// ============================================================
// UI: HOME SCREEN -- status bar, a wide live widget, then six app tiles.
// ============================================================
void drawTileFrame(int i) {
  int x = tileX[i], y = tileY[i];
  drawGlassRect(x, y, tileW, tileH, 16, tileColors[i]);
  tft.setFont();
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(tileNames[i], 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(x + (tileW - tw) / 2, y + tileH - 16);
  tft.print(tileNames[i]);
  if (i == TILE_MESSAGES && (totalUnread() > 0 || reqPending)) {    // unread badge
    int n = totalUnread();
    tft.fillCircle(x + tileW - 16, y + 14, 8, COLOR_ERROR);
    tft.setCursor(x + tileW - 19, y + 9);
    if (n > 0) tft.print(n > 9 ? 9 : n); else tft.print("!");
  }
}

// Small back arrow at the left of the Home status bar -- tapping it
// returns to the lock screen.
bool touchedHomeLockArrow(int x, int y) { return x < 36 && y < 28; }

// WiFi strength: four bars, lit by signal level (all dim when offline).
void drawWifiBars(int x, int y) {
  int level = 0;
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    level = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
  }
  uint16_t dim = blendColor(COLOR_STATUSBAR, ILI9341_WHITE, 35);
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 3;
    tft.fillRect(x + i * 5, y + 12 - h, 3, h, i < level ? ILI9341_WHITE : dim);
  }
}

void drawHomeStatusBar(DateTime now) {
  tft.fillRect(0, 0, 240, 24, COLOR_STATUSBAR);
  tft.drawRoundRect(4, 3, 24, 18, 4, blendColor(COLOR_STATUSBAR, ILI9341_WHITE, 55));
  tft.fillTriangle(10, 12, 16, 7, 16, 17, ILI9341_WHITE);
  tft.fillRect(16, 10, 6, 5, ILI9341_WHITE);
  tft.setFont();
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  char dateStr[20];
  snprintf(dateStr, sizeof(dateStr), "%s, %s %d", dayNamesShort[now.dayOfTheWeek()], monthNamesShort[now.month() - 1], now.day());
  tft.setCursor(36, 7);
  tft.print(dateStr);
  drawWifiBars(150, 6);
  char timeStr[9];
  formatTime12h(timeStr, now.hour(), now.minute());
  tft.setFont(&SynSmallBold);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(timeStr, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(240 - 8 - tw - x1, 16);
  tft.print(timeStr);
  tft.setFont();
}

// ---- Clock widget: an analog face with hour, minute and a ticking second
// hand at the left of the wide widget, with the digital time (with seconds)
// and the date beside it. The face is drawn once; each second only the
// hands and the digits are erased and redrawn. ----
const int CLOCK_R = 26;
int clockShownSecond = -1;
int clockShownH = 0, clockShownM = 0, clockShownS = 0;

int clockCX() { return WIDGET_X + 40; }
int clockCY() { return WIDGET_Y + WIDGET_H / 2; }

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

  clockHandEnd(((h % 12) + m / 60.0) / 12.0, 12, x, y);       // hour (thick)
  tft.drawLine(cx, cy, x, y, handColor);
  tft.drawLine(cx + 1, cy, x + 1, y, handColor);
  tft.drawLine(cx, cy + 1, x, y + 1, handColor);

  clockHandEnd((m + sec / 60.0) / 60.0, 17, x, y);            // minute
  tft.drawLine(cx, cy, x, y, handColor);
  tft.drawLine(cx + 1, cy, x + 1, y, handColor);

  clockHandEnd(sec / 60.0, 19, x, y);                         // second
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

  // digital time with seconds, right of the face
  int dh = now.hour() % 12;
  if (dh == 0) dh = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d:%02d", dh, now.minute(), now.second());
  int tx = WIDGET_X + 80, ty = WIDGET_Y + 14;
  tft.fillRect(tx, ty - 1, WIDGET_W - 80 - 8, 22, tileColors[0]);
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(tx - x1, ty + 17);
  tft.print(buf);
  tft.setFont();
  tft.setCursor(tx + tw + 5, ty + 8);
  tft.print(now.hour() < 12 ? "AM" : "PM");
}

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

// Left-aligned text that steps down from bold 12pt to bold 9pt to the small
// bold font until it fits in maxWidth.
void printLeftFitted(int x, int topY, const char* text, uint16_t color, int maxWidth) {
  const GFXfont* tries[3] = {&FreeSansBold12pt7b, &FreeSansBold9pt7b, &SynSmallBold};
  int dropY[3] = {0, 2, 5};
  int16_t x1, y1; uint16_t tw, th;
  for (int i = 0; i < 3; i++) {
    tft.setFont(tries[i]);
    tft.setTextSize(1);
    tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
    if ((int)tw <= maxWidth || i == 2) {
      tft.setTextColor(color);
      tft.setCursor(x - x1, topY + dropY[i] - y1);
      tft.print(text);
      break;
    }
  }
  tft.setFont();
}

// The wide widget under the status bar. Which one it shows is picked in
// Settings ("Widget: ..."). Every widget is a small title, one big line and
// one detail line; the Clock adds a ticking analog face.
void drawWidgetTile() {
  DateTime now = clockNow();
  drawGlassRect(WIDGET_X, WIDGET_Y, WIDGET_W, WIDGET_H, 16, tileColors[0]);

  int x = WIDGET_X + 14, y = WIDGET_Y;
  int nowMinutes = now.hour() * 60 + now.minute();
  char title[24] = "", mainLine[40] = "", sub[48] = "";
  int progress = -1;                       // 0-100 draws a bar; -1 = none

  switch (homeWidget) {
    case WIDGET_CLOCK: {
      drawClockFace();
      clockShownSecond = -1;               // force hands + digits to draw now
      updateClockWidget(now);
      char d[24];
      snprintf(d, sizeof(d), "%s, %s %d", dayNamesFull[now.dayOfTheWeek()], monthNamesShort[now.month() - 1], now.day());
      tft.setFont();
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(WIDGET_X + 80, y + 42);
      tft.print(d);
      return;
    }
    case WIDGET_NEXT_CLASS: {
      int next = nextPeriodIndex(nowMinutes);
      strcpy(title, "Next up");
      if (next == -1) { strcpy(mainLine, "No more classes"); strcpy(sub, "Nothing else today"); }
      else {
        char t12[9];
        formatTime12h(t12, schedule[next].startHour, schedule[next].startMin);
        snprintf(mainLine, sizeof(mainLine), "%s", schedule[next].name);
        snprintf(sub, sizeof(sub), "Starts at %s", t12);
      }
      break;
    }
    case WIDGET_WEATHER:
      snprintf(title, sizeof(title), "%s", WEATHER_LOCATION_LABEL);
      if (strcmp(weatherTemp, "--") == 0) { strcpy(mainLine, "-- F"); strcpy(sub, "Open Weather to update"); }
      else { snprintf(mainLine, sizeof(mainLine), "%s F", weatherTemp); snprintf(sub, sizeof(sub), "%s", weatherDesc); }
      break;
    case WIDGET_TODO: {
      int n = countFileLines("/todo.txt");
      strcpy(title, "To-Do");
      snprintf(mainLine, sizeof(mainLine), n == 1 ? "%d item" : "%d items", n);
      strcpy(sub, n == 0 ? "All done!" : "on your list");
      break;
    }
    default: { // WIDGET_CLASS
      int idx = getCurrentPeriodIndex(now.hour(), now.minute());
      strcpy(title, "Now");
      if (idx == -1) { strcpy(mainLine, "No class"); strcpy(sub, "right now"); }
      else {
        int startMinutes = schedule[idx].startHour * 60 + schedule[idx].startMin;
        int endMinutes = schedule[idx].endHour * 60 + schedule[idx].endMin;
        snprintf(mainLine, sizeof(mainLine), "%s", schedule[idx].name);
        snprintf(sub, sizeof(sub), "%d min left", endMinutes - nowMinutes);
        if (endMinutes > startMinutes) progress = (nowMinutes - startMinutes) * 100 / (endMinutes - startMinutes);
      }
      break;
    }
  }

  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(x, y + 7);
  tft.print(title);
  printLeftFitted(x, y + 22, mainLine, ILI9341_WHITE, WIDGET_W - 28);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(x, y + 44);
  tft.print(sub);
  if (progress >= 0) {
    int barW = WIDGET_W - 28;
    tft.fillRoundRect(x, y + 57, barW, 4, 2, darkerColor(tileColors[0], 5));
    if (progress > 2) tft.fillRoundRect(x, y + 57, barW * progress / 100, 4, 2, ILI9341_WHITE);
  }
}

void drawHomeScreen(DateTime now) {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawHomeStatusBar(now);

  drawWidgetTile();
  for (int i = 1; i < TILE_COUNT; i++) {
    drawTileFrame(i);
    drawTileIcon(i, TILE_ICON_R);
  }
  drawIpLine();
  if (!msgBanner[0]) drawNewsTicker();
  if (msgBanner[0]) {                 // "Hound Dog accepted!" / "New message from ..."
    tft.fillRoundRect(20, 287, 200, 17, 8, COLOR_ACCENT);
    tft.setFont();
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    int16_t x1, y1; uint16_t tw, th;
    tft.getTextBounds(msgBanner, 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor(120 - tw / 2, 290);
    tft.print(msgBanner);
  }
}

void handleHomeTouch(int x, int y) {
  if (touchedHomeLockArrow(x, y)) {
    DateTime now = clockNow();
    currentScreen = SCREEN_LOCK;
    drawLockScreen(now);
    lastDrawnMinute = now.minute();
    return;
  }
  if (y >= 286 && y <= 305 && !msgBanner[0]) {     // the news ticker opens the News app
    openNews();
    return;
  }
  for (int i = 1; i < TILE_COUNT; i++) {
    if (x >= tileX[i] && x <= tileX[i] + tileW && y >= tileY[i] && y <= tileY[i] + tileH) {
      if (i == 1) {
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
      } else if (i == TILE_MESSAGES) {
        msgBanner[0] = '\0';
        if (reqPending) showConnectPopup();
        else openMessages();
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

        // round checkbox: an empty ring, or filled with a tick when done
        if (completed) {
          tft.fillCircle(20, y + 10, 8, COLOR_ACCENT);
          tft.drawLine(16, y + 10, 19, y + 13, ILI9341_WHITE);
          tft.drawLine(19, y + 13, 24, y + 7, ILI9341_WHITE);
          tft.drawLine(16, y + 11, 19, y + 14, ILI9341_WHITE);
          tft.drawLine(19, y + 14, 24, y + 8, ILI9341_WHITE);
        } else {
          tft.drawCircle(20, y + 10, 8, COLOR_SUBTEXT);
          tft.drawCircle(20, y + 10, 7, COLOR_SUBTEXT);
        }

        tft.setFont();
        tft.setTextColor(completed ? COLOR_SUBTEXT : COLOR_TEXT);
        tft.setTextSize(1);
        tft.setCursor(36, y + 5);
        tft.print(displayText);
        if (completed) {                       // strike through finished tasks
          int16_t bx, by; uint16_t bw, bh;
          tft.getTextBounds(displayText, 0, 0, &bx, &by, &bw, &bh);
          tft.drawFastHLine(36, y + 10, bw + 2, COLOR_SUBTEXT);
        }

        // delete: a small drawn x
        tft.drawLine(212, y + 6, 220, y + 14, COLOR_SUBTEXT);
        tft.drawLine(220, y + 6, 212, y + 14, COLOR_SUBTEXT);
        tft.drawFastHLine(12, y + 23, 216, COLOR_FRAME);   // row divider

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
    tft.print("No tasks yet - tap Add Task");
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

// Each class gets its own color (cycling through the eight accent colors),
// shown as a stripe down the left of its section.
uint16_t hwClassColor(int period) { return rgb565(customAccents[period % 8]); }

void drawHomeworkHeader(int y, const char* name, uint16_t color) {
  tft.fillRoundRect(10, y - 1, 4, 13, 2, color);
  tft.setFont(&SynSmallBold);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(20, y + 8);
  tft.print(name);
  tft.setFont();
}

void drawHomeworkItem(int y, const char* text, uint16_t color) {
  tft.fillRect(11, y - 2, 2, 13, color);                 // stripe continues beside the items
  tft.fillCircle(24, y + 4, 2, color);
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(32, y);
  tft.print(text);
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
            drawHomeworkHeader(y, schedule[p].name, hwClassColor(p));
            y += 14;
            headerDrawn = true;
            linesDrawn++;
          }
          drawHomeworkItem(y, textPart, hwClassColor(p));

          if (hwLineCount < 8) {
            strncpy(hwLines[hwLineCount], rawLine, sizeof(hwLines[0]) - 1);
            hwLines[hwLineCount][sizeof(hwLines[0]) - 1] = '\0';
            hwLineY[hwLineCount] = y;
            tft.drawLine(213, y + 1, 219, y + 7, COLOR_SUBTEXT);
            tft.drawLine(219, y + 1, 213, y + 7, COLOR_SUBTEXT);
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
            drawHomeworkHeader(y, "Unscheduled", COLOR_SUBTEXT);
            y += 14;
            headerDrawn = true;
            linesDrawn++;
          }
          drawHomeworkItem(y, textPart, COLOR_SUBTEXT);

          if (hwLineCount < 8) {
            strncpy(hwLines[hwLineCount], rawLine, sizeof(hwLines[0]) - 1);
            hwLines[hwLineCount][sizeof(hwLines[0]) - 1] = '\0';
            hwLineY[hwLineCount] = y;
            tft.drawLine(213, y + 1, 219, y + 7, COLOR_SUBTEXT);
            tft.drawLine(219, y + 1, 213, y + 7, COLOR_SUBTEXT);
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
    tft.print("Nothing logged yet - tap Add Homework");
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

bool touchedWeatherRefresh(int x, int y) { return x >= 70 && x <= 170 && y >= 264 && y <= 304; }

// ---- big condition icon, built from circles and lines ----
void wxSun(int cx, int cy, int r) {
  uint16_t c = tft.color565(255, 190, 40);
  tft.fillCircle(cx, cy, r, c);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    for (int t = -1; t <= 1; t++)
      tft.drawLine(cx + cos(a) * (r + 5) + t, cy + sin(a) * (r + 5), cx + cos(a) * (r + 13) + t, cy + sin(a) * (r + 13), c);
  }
}

void wxCloud(int cx, int cy, uint16_t c) {
  tft.fillCircle(cx - 16, cy + 4, 12, c);
  tft.fillCircle(cx, cy - 6, 17, c);
  tft.fillCircle(cx + 18, cy + 2, 13, c);
  tft.fillRoundRect(cx - 28, cy + 2, 60, 15, 7, c);
}

void drawWeatherIcon(int cx, int cy, int code) {
  uint16_t cloud = blendColor(COLOR_SUBTEXT, COLOR_SURFACE, 45);
  uint16_t dark = COLOR_SUBTEXT;
  uint16_t rain = tft.color565(70, 150, 240);
  if (code == 0) { wxSun(cx, cy, 16); return; }
  if (code == 1 || code == 2) { wxSun(cx + 14, cy - 12, 11); wxCloud(cx - 4, cy + 6, cloud); return; }
  if (code == 3) { wxCloud(cx, cy, cloud); return; }
  if (code == 45 || code == 48) {                                  // fog: soft bars
    for (int i = 0; i < 4; i++) tft.fillRoundRect(cx - 28 + (i % 2) * 8, cy - 18 + i * 11, 50, 5, 2, cloud);
    return;
  }
  bool thunder = (code >= 95);
  bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  wxCloud(cx, cy - 8, thunder ? dark : cloud);
  if (thunder) {
    uint16_t bolt = tft.color565(255, 200, 40);
    tft.fillTriangle(cx + 2, cy + 8, cx - 8, cy + 24, cx + 1, cy + 22, bolt);
    tft.fillTriangle(cx - 1, cy + 20, cx + 8, cy + 18, cx - 4, cy + 36, bolt);
  } else if (snow) {
    for (int i = 0; i < 4; i++) tft.fillCircle(cx - 21 + i * 14, cy + 20 + (i % 2) * 8, 2, COLOR_TEXT);
  } else {                                                         // drizzle, rain, showers
    for (int i = 0; i < 4; i++)
      for (int t = 0; t < 2; t++)
        tft.drawLine(cx - 18 + i * 13 + t, cy + 14, cx - 23 + i * 13 + t, cy + 28, rain);
  }
}

void drawWeatherScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Weather");

  printCentered(&FreeSans9pt7b, 120, 36, WEATHER_LOCATION_LABEL, COLOR_SUBTEXT);

  if (weatherFetching) {
    printCentered(&FreeSans12pt7b, 120, 140, "Fetching...", COLOR_SUBTEXT);
  } else {
    if (weatherCode >= 0) drawWeatherIcon(120, 96, weatherCode);
    drawTempWithDegree(weatherTemp, 120, weatherCode >= 0 ? 146 : 110, COLOR_TEXT);
    printCentered(&FreeSans12pt7b, 120, weatherCode >= 0 ? 194 : 170, weatherDesc, COLOR_SUBTEXT);
    if (weatherHigh > -999 && weatherLow > -999) {
      // "High 61 / Low 43" with a big bold slash in the accent color
      char hi[16], lo[16];
      snprintf(hi, sizeof(hi), "High %d", weatherHigh);
      snprintf(lo, sizeof(lo), "Low %d", weatherLow);
      int16_t bx, by; uint16_t wHi, wLo, wSlash, bh;
      tft.setFont(&FreeSansBold9pt7b);
      tft.setTextSize(1);
      tft.getTextBounds(hi, 0, 0, &bx, &by, &wHi, &bh);
      tft.getTextBounds(lo, 0, 0, &bx, &by, &wLo, &bh);
      tft.setFont(&FreeSansBold18pt7b);
      tft.getTextBounds("/", 0, 0, &bx, &by, &wSlash, &bh);
      int gap = 10, total = wHi + gap + wSlash + gap + wLo;
      int x = 120 - total / 2;
      tft.setFont(&FreeSansBold9pt7b);
      tft.setTextColor(COLOR_TEXT);
      tft.setCursor(x, 240);
      tft.print(hi);
      tft.setFont(&FreeSansBold18pt7b);
      tft.setTextColor(COLOR_ACCENT);
      tft.setCursor(x + wHi + gap - bx, 247);
      tft.print("/");
      tft.setFont(&FreeSansBold9pt7b);
      tft.setTextColor(COLOR_TEXT);
      tft.setCursor(x + wHi + gap + wSlash + gap, 240);
      tft.print(lo);
      tft.setFont();
    }
  }

  if (weatherStatus[0] && !weatherFetching) {       // why the last Refresh didn't work
    tft.setFont();
    tft.setTextColor(COLOR_ERROR);
    int16_t sx, sy; uint16_t sw, sh;
    tft.getTextBounds(weatherStatus, 0, 0, &sx, &sy, &sw, &sh);
    tft.setCursor(120 - sw / 2, 250);
    tft.print(weatherStatus);
  }

  drawGlassRect(70, 264, 100, 40, 18, COLOR_WEATHER);
  printFitted(&FreeSansBold12pt7b, 120, 276, "Refresh", ILI9341_WHITE, 90);
}

// Downloads the current weather. On success the values are replaced and it
// returns true; on any failure the previous values are left untouched.
bool fetchWeatherData() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LAT "&longitude=" WEATHER_LON "&current_weather=true&temperature_unit=fahrenheit"
               "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto";
  http.begin(client, url);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && !doc["current_weather"]["temperature"].isNull()) {
      float t = doc["current_weather"]["temperature"];
      int wcode = doc["current_weather"]["weathercode"];
      sprintf(weatherTemp, "%.0f", t);
      weatherCode = wcode;
      // today's high / low (left unknown if the reply doesn't include them)
      weatherHigh = weatherLow = -999;
      if (!doc["daily"]["temperature_2m_max"][0].isNull() && !doc["daily"]["temperature_2m_min"][0].isNull()) {
        weatherHigh = (int)lroundf(doc["daily"]["temperature_2m_max"][0].as<float>());
        weatherLow = (int)lroundf(doc["daily"]["temperature_2m_min"][0].as<float>());
      }
      strncpy(weatherDesc, weatherCodeToDesc(wcode), sizeof(weatherDesc) - 1);
      weatherDesc[sizeof(weatherDesc) - 1] = '\0';
      ok = true;
    }
  }
  http.end();
  // fresh for 30 minutes; after a failure try again in 5
  weatherNextTryMs = millis() + (ok ? 30UL * 60UL * 1000UL : 5UL * 60UL * 1000UL);
  return ok;
}

// The Refresh button on the Weather screen. If it can't update, the last
// reading stays on screen with a short note saying why.
void fetchWeather() {
  weatherStatus[0] = '\0';
  if (WiFi.status() != WL_CONNECTED) {
    strcpy(weatherStatus, "No WiFi - couldn't update");
    if (weatherCode < 0) strcpy(weatherDesc, "Connect WiFi first");
    drawWeatherScreen();
    return;
  }
  weatherFetching = true;
  drawWeatherScreen();
  if (!fetchWeatherData()) {
    strcpy(weatherStatus, "Couldn't update - try again");
    if (weatherCode < 0) strcpy(weatherDesc, "Fetch failed");
  }
  weatherFetching = false;
  drawWeatherScreen();
}

// Weather auto-sync: while the PDA sits on Home or Lock with WiFi, the
// weather is fetched shortly after start-up and then every 30 minutes.
// A failed sync keeps the last reading, so the lock screen's weather card
// (and the Weather widget) never blank out. Takes turns with the news
// refresh so the screen is never held up for long.
void weatherAutoRefresh(bool touching) {
  if (touching || WiFi.status() != WL_CONNECTED) return;
  if (currentScreen != SCREEN_HOME && currentScreen != SCREEN_LOCK) return;
  if (millis() < 12000) return;
  if (newsLastAutoMs != 0 && millis() - newsLastAutoMs < 15000) return;
  if (weatherNextTryMs != 0 && (long)(millis() - weatherNextTryMs) < 0) return;
  bool ok = fetchWeatherData();
  newsLastAutoMs = millis();
  if (!ok) return;
  DateTime now = clockNow();
  if (currentScreen == SCREEN_LOCK) drawLockScreen(now);
  else if (homeWidget == WIDGET_WEATHER) drawHomeScreen(now);
  lastDrawnMinute = now.minute();
}

// ============================================================
// UI: SNAKE GAME
// ============================================================
// Layout: a back bar + score line up top, then the board. Controlled
// entirely by swiping on the board -- swipe up/down/left/right to steer.
// A 18 x 20 board of 12-pixel squares on a checkerboard, so it's easy to
// see exactly which square the snake and the food are on. Only the squares
// that change are redrawn each step (new head, old head, tail), so the
// board never flickers.
#define SNAKE_CELL 12
const int snakeGridCols = 18;
const int snakeGridRows = 20;
const int snakeFieldW = snakeGridCols * SNAKE_CELL;  // 216
const int snakeFieldH = snakeGridRows * SNAKE_CELL;  // 240
const int snakeFieldX = (240 - snakeFieldW) / 2;      // 12
const int snakeFieldY = 46;

#define SNAKE_MAX_LEN 360
int snakeX[SNAKE_MAX_LEN], snakeY[SNAKE_MAX_LEN];
int snakeLen = 3;
SnakeDir snakeDir = SNAKE_RIGHT;
SnakeDir snakeQueue[2];              // up to two turns waiting, so quick double swipes both count
int snakeQueueLen = 0;
int snakeFoodX = 0, snakeFoodY = 0;
bool snakeGameOver = false;
bool snakeWaiting = true;            // true until the first swipe starts the game
unsigned long snakeLastTickMs = 0;
int snakeScore = 0;
int snakeBest = -1;                  // best score, kept in /snake.txt (-1 = not loaded yet)

// ---- Swipe tracking: a swipe is measured from where the finger first
// touched down on the field to how far it has dragged since. Once it
// clears the threshold in some direction, that swipe "fires" once and
// won't fire again until the finger lifts and touches down again. ----
int snakeSwipeStartX = -1;
int snakeSwipeStartY = -1;
bool snakeSwipeFired = false;
const int SNAKE_SWIPE_THRESHOLD = 14;

void loadSnakeBest() {
  snakeBest = 0;
  if (!LittleFS.exists("/snake.txt")) return;
  File f = LittleFS.open("/snake.txt");
  if (!f) return;
  char buf[8];
  int i = 0;
  while (f.available() && i < 7) buf[i++] = f.read();
  buf[i] = '\0';
  f.close();
  snakeBest = atoi(buf);
}

void saveSnakeBest() {
  LittleFS.remove("/snake.txt");
  File f = LittleFS.open("/snake.txt", FILE_WRITE);
  if (f) { f.print(snakeBest); f.close(); }
}

// The snake speeds up a little with every bite, down to a floor.
unsigned long snakeTickMs() {
  int t = 150 - snakeScore * 3;
  return t < 85 ? 85 : t;
}

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
  if (snakeBest < 0) loadSnakeBest();
  snakeLen = 3;
  int startX = snakeGridCols / 2;
  int startY = snakeGridRows / 2;
  for (int i = 0; i < snakeLen; i++) {
    snakeX[i] = startX - i;
    snakeY[i] = startY;
  }
  snakeDir = SNAKE_RIGHT;
  snakeQueueLen = 0;
  snakeGameOver = false;
  snakeWaiting = true;
  snakeScore = 0;
  snakeLastTickMs = millis();
  snakePlaceFood();
}

void drawSnakeScore() {
  tft.fillRect(0, 25, 240, 18, COLOR_BG);
  char buf[24];
  tft.setFont(&SynSmallBold);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT);
  snprintf(buf, sizeof(buf), "Score  %d", snakeScore);
  tft.setCursor(snakeFieldX, 38);
  tft.print(buf);
  snprintf(buf, sizeof(buf), "Best  %d", snakeBest < 0 ? 0 : snakeBest);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
  tft.setTextColor(COLOR_SUBTEXT);
  tft.setCursor(snakeFieldX + snakeFieldW - tw - x1, 38);
  tft.print(buf);
  tft.setFont();
}

// ---- drawing one square at a time ----
uint16_t snakeCellBg(int cx, int cy) {
  return ((cx + cy) & 1) ? COLOR_SURFACE : blendColor(COLOR_SURFACE, COLOR_FRAME, 45);
}

void snakeDrawEmpty(int cx, int cy) {
  tft.fillRect(snakeFieldX + cx * SNAKE_CELL, snakeFieldY + cy * SNAKE_CELL, SNAKE_CELL, SNAKE_CELL, snakeCellBg(cx, cy));
}

void snakeDrawBody(int cx, int cy) {
  int px = snakeFieldX + cx * SNAKE_CELL, py = snakeFieldY + cy * SNAKE_CELL;
  tft.fillRect(px, py, SNAKE_CELL, SNAKE_CELL, snakeCellBg(cx, cy));
  tft.fillRoundRect(px + 1, py + 1, SNAKE_CELL - 2, SNAKE_CELL - 2, 3, COLOR_ACCENT);
}

void snakeDrawHead(int cx, int cy) {
  int px = snakeFieldX + cx * SNAKE_CELL, py = snakeFieldY + cy * SNAKE_CELL;
  tft.fillRect(px, py, SNAKE_CELL, SNAKE_CELL, snakeCellBg(cx, cy));
  tft.fillRoundRect(px + 1, py + 1, SNAKE_CELL - 2, SNAKE_CELL - 2, 4, darkerColor(COLOR_ACCENT, 6));
  // two eyes on the side it is heading toward
  int ex1, ey1, ex2, ey2;
  if (snakeDir == SNAKE_RIGHT)     { ex1 = px + 8; ey1 = py + 3; ex2 = px + 8; ey2 = py + 7; }
  else if (snakeDir == SNAKE_LEFT) { ex1 = px + 2; ey1 = py + 3; ex2 = px + 2; ey2 = py + 7; }
  else if (snakeDir == SNAKE_UP)   { ex1 = px + 3; ey1 = py + 2; ex2 = px + 7; ey2 = py + 2; }
  else                             { ex1 = px + 3; ey1 = py + 8; ex2 = px + 7; ey2 = py + 8; }
  tft.fillRect(ex1, ey1, 2, 2, ILI9341_WHITE);
  tft.fillRect(ex2, ey2, 2, 2, ILI9341_WHITE);
}

void snakeDrawFood() {
  int px = snakeFieldX + snakeFoodX * SNAKE_CELL, py = snakeFieldY + snakeFoodY * SNAKE_CELL;
  tft.fillRect(px, py, SNAKE_CELL, SNAKE_CELL, snakeCellBg(snakeFoodX, snakeFoodY));
  tft.fillCircle(px + 6, py + 6, 4, COLOR_ERROR);
  tft.fillRect(px + 6, py, 2, 3, COLOR_HOMEWORK);        // little stem
}

// The whole board. Only used when the screen opens or a game restarts.
void drawSnakeField() {
  tft.drawRect(snakeFieldX - 1, snakeFieldY - 1, snakeFieldW + 2, snakeFieldH + 2, COLOR_SUBTEXT);
  for (int cy = 0; cy < snakeGridRows; cy++)
    for (int cx = 0; cx < snakeGridCols; cx++) snakeDrawEmpty(cx, cy);
  snakeDrawFood();
  for (int i = snakeLen - 1; i > 0; i--) snakeDrawBody(snakeX[i], snakeY[i]);
  snakeDrawHead(snakeX[0], snakeY[0]);
}

void drawSnakeHint(const char* text) {
  tft.fillRect(0, snakeFieldY + snakeFieldH + 2, 240, 320 - (snakeFieldY + snakeFieldH + 2), COLOR_BG);
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(COLOR_SUBTEXT);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((240 - tw) / 2, snakeFieldY + snakeFieldH + 10);
  tft.print(text);
}

void drawSnakeGameOverOverlay() {
  // a card big enough for all four lines, centered on the board
  int bx = 26, by = snakeFieldY + 56, bw = 188, bh = 124;
  tft.fillRoundRect(bx + 3, by + 4, bw, bh, 16, COLOR_SHADOW);
  tft.fillRoundRect(bx, by, bw, bh, 16, COLOR_ERROR);
  printCentered(&FreeSansBold12pt7b, 120, by + 14, "Game Over", ILI9341_WHITE);
  char buf[24];
  snprintf(buf, sizeof(buf), "Score: %d", snakeScore);
  printCentered(&FreeSansBold9pt7b, 120, by + 46, buf, ILI9341_WHITE);
  snprintf(buf, sizeof(buf), snakeScore > 0 && snakeScore >= snakeBest ? "New best!" : "Best: %d", snakeBest);
  printCentered(&FreeSans9pt7b, 120, by + 68, buf, ILI9341_WHITE);
  printCentered(&FreeSans9pt7b, 120, by + 96, "Tap to play again", ILI9341_WHITE);
}

void drawSnakeScreen() {
  tft.fillScreen(COLOR_BG);
  drawBackBar("Snake");
  drawSnakeScore();
  drawSnakeField();
  if (snakeGameOver) drawSnakeGameOverOverlay();
  drawSnakeHint(snakeGameOver ? "" : snakeWaiting ? "Swipe on the board to start" : "Swipe to steer");
}

bool touchedSnakeField(int x, int y) {
  return x >= snakeFieldX && x <= snakeFieldX + snakeFieldW && y >= snakeFieldY && y <= snakeFieldY + snakeFieldH;
}

void snakeApplyDirection(SnakeDir d) {
  // The first swipe starts the game (any direction except straight back).
  SnakeDir last = snakeQueueLen > 0 ? snakeQueue[snakeQueueLen - 1] : snakeDir;
  // Ignore a swipe that would reverse the snake straight into its own body,
  // or that repeats the way it is already going.
  if ((last == SNAKE_UP && d == SNAKE_DOWN) || (last == SNAKE_DOWN && d == SNAKE_UP) ||
      (last == SNAKE_LEFT && d == SNAKE_RIGHT) || (last == SNAKE_RIGHT && d == SNAKE_LEFT)) return;
  if (snakeWaiting) {
    snakeWaiting = false;
    snakeLastTickMs = millis();
    drawSnakeHint("Swipe to steer");
  }
  if (d == last) return;
  if (snakeQueueLen < 2) snakeQueue[snakeQueueLen++] = d;
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
  if (snakeGameOver || snakeWaiting) return;
  if (millis() - snakeLastTickMs < snakeTickMs()) return;
  snakeLastTickMs = millis();

  if (snakeQueueLen > 0) {                       // take the next waiting turn
    snakeDir = snakeQueue[0];
    snakeQueue[0] = snakeQueue[1];
    snakeQueueLen--;
  }
  int nx = snakeX[0];
  int ny = snakeY[0];
  if (snakeDir == SNAKE_UP) ny--;
  else if (snakeDir == SNAKE_DOWN) ny++;
  else if (snakeDir == SNAKE_LEFT) nx--;
  else nx++;

  bool ate = (nx == snakeFoodX && ny == snakeFoodY);
  bool crashed = (nx < 0 || nx >= snakeGridCols || ny < 0 || ny >= snakeGridRows);
  // the tail square is about to move away, so running into it is fine (unless growing)
  for (int i = 0; i < snakeLen - (ate ? 0 : 1) && !crashed; i++) {
    if (snakeX[i] == nx && snakeY[i] == ny) crashed = true;
  }
  if (crashed) {
    snakeGameOver = true;
    if (snakeScore > snakeBest) { snakeBest = snakeScore; saveSnakeBest(); }
    drawSnakeScore();
    drawSnakeGameOverOverlay();
    drawSnakeHint("");
    return;
  }

  int tailX = snakeX[snakeLen - 1], tailY = snakeY[snakeLen - 1];
  int oldHeadX = snakeX[0], oldHeadY = snakeY[0];
  if (ate && snakeLen < SNAKE_MAX_LEN - 1) snakeLen++;
  else if (!ate) snakeDrawEmpty(tailX, tailY);    // the tail leaves its square

  for (int i = snakeLen - 1; i > 0; i--) {
    snakeX[i] = snakeX[i - 1];
    snakeY[i] = snakeY[i - 1];
  }
  snakeX[0] = nx;
  snakeY[0] = ny;

  snakeDrawBody(oldHeadX, oldHeadY);              // where the head was is now body
  snakeDrawHead(nx, ny);

  if (ate) {
    snakeScore++;
    snakePlaceFood();
    snakeDrawFood();
    drawSnakeScore();
  }
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
  tft.setTextColor(rtcOK && rtcTicking ? COLOR_HOMEWORK : COLOR_ERROR);
  tft.print(!rtcOK ? "Not Found" : (rtcTicking ? "OK" : "Not ticking"));

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
      snprintf(label, sizeof(label), "Theme: %s", currentTheme == THEME_CUSTOM ? "Custom" : themeFor(currentTheme).name);
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
        DateTime now = clockNow();
        editHour = now.hour();
        editMinute = now.minute();
        currentScreen = SCREEN_SET_TIME;
        drawSetTimeScreen();
      } else if (action == MORE_ACTION_SET_DATE) {
        DateTime now = clockNow();
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

// Rows get a little shorter once the secret theme adds a ninth one.
int themeRowPitch() { return shownThemeCount() > 8 ? 32 : 36; }

void drawThemesScreen() {
  tft.fillScreen(COLOR_BG);
  drawScreenFrame();
  drawBackBar("Theme");

  int pitch = themeRowPitch(), h = pitch - 6;
  for (int i = 0; i < shownThemeCount(); i++) {
    int rowY = 30 + i * pitch;
    bool selected = (i == currentTheme);
    drawGlassRect(12, rowY, 216, h, 14, selected ? COLOR_ACCENT : COLOR_SURFACE);

    Theme t = themeFor(i);                       // Custom resolves to its base + accent
    drawThemeSwatches(22, rowY + (h - 20) / 2, t.bg, t.accent, t.statusbar, t.todo);

    const char* name = (i == THEME_CUSTOM) ? "Custom >" : t.name;
    tft.setFont(&FreeSansBold12pt7b);
    tft.setTextSize(1);
    tft.setTextColor(selected ? ILI9341_WHITE : COLOR_TEXT);
    tft.setCursor(84, rowY + h / 2 + 7);
    tft.print(name);
    tft.setFont();
  }
}

void handleThemesTouch(int x, int y) {
  int pitch = themeRowPitch(), h = pitch - 6;
  for (int i = 0; i < shownThemeCount(); i++) {
    int rowY = 30 + i * pitch;
    if (x >= 12 && x <= 228 && y >= rowY && y <= rowY + h) {
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

// The reveal when the secret is found: switch to the theme and say so.
void unlockSecretTheme() {
  secretUnlocked = true;
  saveSecretUnlocked();
  currentTheme = THEME_SECRET;
  applyTheme(currentTheme);
  saveThemeSettings();

  tft.fillScreen(ILI9341_BLACK);
  for (int r = 4; r <= 40; r += 4) { tft.drawCircle(120, 96, r, COLOR_ACCENT); delay(25); }
  tft.fillCircle(120, 96, 14, COLOR_ACCENT);
  printCentered(&FreeSansBold18pt7b, 120, 160, "Jensen Mode", COLOR_ACCENT);
  printCentered(&FreeSans9pt7b, 120, 206, "Secret theme unlocked", ILI9341_WHITE);
  printCentered(&FreeSans9pt7b, 120, 244, "\"The more you buy,", COLOR_SUBTEXT);
  printCentered(&FreeSans9pt7b, 120, 264, "the more you save.\"", COLOR_SUBTEXT);
  delay(2600);
  drawMoreAboutScreen();
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
    DateTime now = clockNow();
    int yr = now.year() < 2020 ? 2026 : now.year();
    int mo = now.month() < 1 ? 1 : now.month();
    int dy = now.day() < 1 ? 1 : now.day();
    setClock(DateTime(yr, mo, dy, editHour, editMinute, 0));
    Serial.println("Time saved.");

    if (firstBootFlow) {
      DateTime updated = clockNow();
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
    DateTime now = clockNow();
    setClock(DateTime(editYear, editMonth, editDay, now.hour(), now.minute(), now.second()));
    Serial.println("Date saved.");

    if (firstBootFlow) {
      firstBootFlow = false;
      currentScreen = SCREEN_HOME;
      DateTime updated = clockNow();
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
    drawHomeScreen(clockNow());
    lastDrawnMinute = clockNow().minute();
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
  loadMessaging();
  loadNewsSettings();
  tft.setRotation(displayRotation);

  char hostname[33];
  buildHostname(hostname, sizeof(hostname));
  WiFi.setHostname(hostname);
  WiFi.mode(WIFI_STA);
  startTimeSync();
  initClockFromRtc();

  loadThemeSettings();
  applyTheme(currentTheme);
  drawBootScreen();

  connectToBestSavedNetwork();

  server.on("/", handleWebRoot);
  server.on("/addtodo", handleWebAddTodo);
  server.on("/addhomework", handleWebAddHomework);
  server.on("/deletetodo", handleWebDeleteTodo);
  server.on("/deletehomework", handleWebDeleteHomework);
  server.on("/calibrate", handleWebCalibrate);
  server.on("/update", handleWebUpdate);
  server.on("/restart", handleWebRestart);
  server.on("/time", handleWebTime);
  server.on("/msg/request", handleMsgRequest);
  server.on("/msg/accept", handleMsgAccept);
  server.on("/msg/send", handleMsgSend);
  server.begin();

  DateTime now = clockNow();
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
  DateTime now = clockNow();

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
  maintainClock();

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
  pollDiscovery(false);
  updateBanner();
  if (currentScreen == SCREEN_HOME) updateNewsTicker();
  weatherAutoRefresh(touchedNow);
  newsAutoRefresh(touchedNow);
  if (bannerActive && justTouched && touchY < 44) {   // tapped the banner
    openBannerChat();
    justTouched = false;
  }

  // press feedback (not where touches draw or steer)
  ButtonRect pressedWas = {0, 0, 0, 0, 0, 0};
  bool didFlash = false;
  if (justTouched && currentScreen != SCREEN_NOTES && currentScreen != SCREEN_SNAKE &&
      currentScreen != SCREEN_TOUCH_CAL && currentScreen != SCREEN_LOCK) {
    flashPressedButton(touchX, touchY);
    if (pressedButton >= 0) { pressedWas = buttonRects[pressedButton]; didFlash = true; }
  }
  if (reqPending && currentScreen != SCREEN_MSG_REQUEST && canShowPopupNow()) showConnectPopup();

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
          } else if (kbContext == KB_CONTEXT_MSG_IP) {
            openMessages();
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
        secretTapCount = 0;
        currentScreen = SCREEN_SETTINGS;
        drawSettingsScreen();
      } else if (justTouched && touchY >= 40 && touchY <= 68 && !secretUnlocked) {
        if (++secretTapCount >= 7) { secretTapCount = 0; unlockSecretTheme(); }   // shh
      }
      break;

    case SCREEN_MESSAGES:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          msgStatus[0] = '\0';
          currentScreen = SCREEN_HOME;
          drawHomeScreen(now);
          lastDrawnMinute = now.minute();
        } else {
          handleMessagesTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_CHAT:
      if (justTouched) handleChatTouch(touchX, touchY);
      break;

    case SCREEN_ARTICLE:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          if (articleFull) { articleFull = false; drawArticleScreen(); }   // back to the summary first
          else { currentScreen = SCREEN_NEWS; drawNewsScreen(); }
        } else {
          handleArticleTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_NEWS:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) {
          currentScreen = SCREEN_HOME;
          drawHomeScreen(now);
          lastDrawnMinute = now.minute();
        } else {
          handleNewsTouch(touchX, touchY);
        }
      }
      break;

    case SCREEN_NEARBY:
      if (justTouched) {
        if (touchedBack(touchX, touchY)) openMessages();
        else handleNearbyTouch(touchX, touchY);
      }
      break;

    case SCREEN_MSG_REQUEST:
      if (justTouched) handleConnectPopupTouch(touchX, touchY);
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
  if (didFlash) releasePressedButton(pressedWas);
  delay(2);
}