// Minimal Arduino/ESP32 stand-ins so the SynRG sketch can be compiled and
// run on a PC, drawing into a framebuffer that is saved as PNG screenshots.
// Simulator only -- never part of the firmware build.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <string>
#include <map>
#include <vector>
#include <functional>

#define PROGMEM
#define F(x) x
#define PI 3.14159265358979
#define pgm_read_byte(a) (*(const uint8_t*)(a))
#define pgm_read_word(a) (*(const uint16_t*)(a))
#define pgm_read_dword(a) (*(const uint32_t*)(a))
#define pgm_read_pointer(a) ((void*)*(const uintptr_t*)(a))
typedef uint8_t byte;
typedef bool boolean;
enum { D0, D1, D2, D3, D4, D5, D6, D7, D8, D9, D10 };

extern unsigned long simMillis;
inline unsigned long millis() { return simMillis; }
inline void delay(unsigned long ms) { simMillis += ms; }
inline void yield() {}
inline long map(long x, long a, long b, long c, long d) { return (x - a) * (d - c) / (b - a) + c; }
#define constrain(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

class __FlashStringHelper;
#define radians(d) ((d) * PI / 180.0)
inline long simRandom(long n) { return n > 0 ? rand() % n : 0; }
inline long simRandom(long a, long b) { return b > a ? a + rand() % (b - a) : a; }
#define random(...) simRandom(__VA_ARGS__)

class String {
public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& c) : s(c) {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(unsigned int v) : s(std::to_string(v)) {}
  const char* c_str() const { return s.c_str(); }
  unsigned int length() const { return s.size(); }
  void trim() {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
  }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
  String& operator+=(char o) { s += o; return *this; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator==(const char* o) const { return s == o; }
  char operator[](int i) const { return s[i]; }
  int toInt() const { return atoi(s.c_str()); }
  bool concat(const char* c) { s += c; return true; }
  bool concat(const char* c, size_t n) { s.append(c, n); return true; }
  bool concat(char c) { s += c; return true; }
  bool reserve(size_t) { return true; }
  String& operator=(const char* c) { s = c ? c : ""; return *this; }
};
inline String operator+(const String& a, const String& b) { return String(a.s + b.s); }
inline String operator+(const String& a, const char* b) { return String(a.s + b); }
inline String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  size_t print(const char* s) { size_t n = 0; while (*s) n += write((uint8_t)*s++); return n; }
  size_t print(const String& s) { return print(s.c_str()); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { return print(std::to_string(v).c_str()); }
  size_t print(unsigned v) { return print(std::to_string(v).c_str()); }
  size_t print(long v) { return print(std::to_string(v).c_str()); }
  size_t print(unsigned long v) { return print(std::to_string(v).c_str()); }
  size_t print(double v, int d = 2) { char b[32]; snprintf(b, 32, "%.*f", d, v); return print(b); }
  size_t println() { return print("\n"); }
  template <class T> size_t println(T v) { size_t n = print(v); return n + print("\n"); }
  size_t printf(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    return print(b);
  }
};

struct SerialStub : Print {
  void begin(long) {}
  size_t write(uint8_t) override { return 1; }
};
extern SerialStub Serial;

class IPAddress {
public:
  uint8_t b[4];
  IPAddress(uint8_t a = 0, uint8_t c = 0, uint8_t d = 0, uint8_t e = 0) : b{a, c, d, e} {}
  uint8_t& operator[](int i) { return b[i]; }
  uint8_t operator[](int i) const { return b[i]; }
  bool operator==(const IPAddress& o) const { return memcmp(b, o.b, 4) == 0; }
  bool fromString(const char* s) {
    int a, c, d, e; char extra;
    if (sscanf(s, "%d.%d.%d.%d%c", &a, &c, &d, &e, &extra) != 4) return false;
    b[0] = a; b[1] = c; b[2] = d; b[3] = e; return true;
  }
  String toString() const { char t[20]; snprintf(t, 20, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]); return String(t); }
};

struct EspStub { void restart() {} };
extern EspStub ESP;
inline bool getLocalTime(struct tm* t, uint32_t = 0) { time_t n = time(nullptr); localtime_r(&n, t); return true; }
inline void configTzTime(const char*, const char*, const char* = 0, const char* = 0) {}
