// ---- simulator driver: appended after the sketch in the same file ----
unsigned long simMillis = 100000;
SerialStub Serial; EspStub ESP; SPIStub SPI; WireStub Wire; LittleFSStub LittleFS;
WiFiStub WiFi; UpdateStub Update;
bool simTouched = false; TS_Point simTouchPoint; bool simWifiUp = true;
DateTime simNow(2026, 11, 16, 10, 42, 7);
std::map<std::string, std::string> simFiles;

static const char* simOutDir = "shots";
void shot(const char* name) {
  char path[256];
  snprintf(path, sizeof(path), "%s/%s.ppm", simOutDir, name);
  FILE* f = fopen(path, "wb");
  fprintf(f, "P6\n240 320\n255\n");
  for (int i = 0; i < 240 * 320; i++) {
    uint16_t c = tft.fb[i];
    uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

// one tap at screen coordinates (uses the sketch's default touch mapping)
void tap(int x, int y) {
  simTouchPoint.x = 200 + (long)x * 3600 / 240;
  simTouchPoint.y = 200 + (long)y * 3600 / 320;
  simTouchPoint.z = 1000;
  simTouched = true;  loop();
  simTouched = false; loop();
  simMillis += 50;
}

#include "scenes.inc"

int main(int argc, char** argv) {
  if (argc > 1) simOutDir = argv[1];
  simFiles["/todo.txt"] = "[ ] Finish math worksheet\n[x] Charge PDA\n[ ] Email Mr. Hall\n";
  simFiles["/homework.txt"] = "Math|Page 42 #1-20\nEnglish|Read chapter 5\n";
  simFiles["/name.txt"] = "Fox";
  simFiles["/msgon.txt"] = "1";
  simFiles["/contacts.txt"] = "Hound Dog|192.168.1.77\nMaya|192.168.1.80\n";
  simFiles["/messages.txt"] =
    "i|192.168.1.77|10:30 AM|yo are you at lunch\n"
    "o|192.168.1.77|10:31 AM|yeah table by the window\n"
    "i|192.168.1.77|10:31 AM|ok omw save me a seat please i have to grab my stuff first\n"
    "o|192.168.1.77|10:40 AM|k\n"
    "f|192.168.1.77|10:41 AM|hurry up\n"
    "i|192.168.1.80|9:15 AM|did you do the homework\n";
  setup();
  runScenes();
  return 0;
}
