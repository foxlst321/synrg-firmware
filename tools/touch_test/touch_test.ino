// ============================================================
// SynRG touch test -- flash over USB, open Tools > Serial Monitor at
// 115200 baud, then press the screen. Uses the same pins as the PDA.
//
// What the output means:
//   z=0 while not touching, z jumps above ~200 when pressed  -> touch works
//   x/y/z stuck at 0 even when pressing                      -> touch chip not answering
//                                                               (T_CS, T_DO/MISO, or power)
//   x/y stuck near 4095 all the time                          -> MISO line floating/shorted
// The screen shows the same numbers and draws a dot where you press.
// ============================================================
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>

#define TFT_CS   D0
#define TFT_RST  D1
#define TFT_DC   D7
#define T_CS     D3
#define TFT_SCK  D8
#define TFT_MISO D9
#define TFT_MOSI D10

Adafruit_ILI9341 tft = Adafruit_ILI9341(&SPI, TFT_DC, TFT_CS, TFT_RST);
XPT2046_Touchscreen ts(T_CS);

void setup() {
  Serial.begin(115200);
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(2);
  ts.begin();
  ts.setRotation(0);

  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("Touch test");
  Serial.println("SynRG touch test -- press the screen");
}

void loop() {
  TS_Point p = ts.getPoint();
  bool down = ts.touched();

  char buf[48];
  snprintf(buf, sizeof(buf), "x=%4d y=%4d z=%4d %s", p.x, p.y, p.z, down ? "TOUCH" : "");
  Serial.println(buf);

  tft.setTextSize(1);
  tft.setCursor(10, 40);
  tft.print(buf);
  tft.print("      ");

  if (down) {
    int sx = map(p.x, 200, 3900, 0, 239);
    int sy = map(p.y, 200, 3900, 0, 319);
    tft.fillCircle(constrain(sx, 0, 239), constrain(sy, 0, 319), 3, ILI9341_GREEN);
  }
  delay(100);
}
