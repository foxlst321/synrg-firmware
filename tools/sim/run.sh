#!/bin/bash
# Builds the sketch for the PC simulator and writes PNG screenshots of every
# screen to tools/sim/shots/. Needs g++, universal-ctags, python3 + Pillow,
# and the Arduino libraries (Adafruit GFX, ArduinoJson) in $LIBS.
set -e
cd "$(dirname "$0")"
LIBS=${LIBS:-$HOME/Arduino/libraries}
GFX=$LIBS/Adafruit-GFX-Library; [ -d $GFX/src ] && GFX=$GFX/src
OUT=${OUT:-build}; mkdir -p $OUT/synrg shots
python3 mkproto.py ../../synrg/synrg.ino $OUT/synrg
cat $OUT/synrg/main.cpp driver.inc > $OUT/sim.cpp
g++ -std=gnu++17 -O1 -w -DARDUINO=200 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0 -fpermissive -Istubs -I. -I../../synrg -I$GFX -I$LIBS/ArduinoJson/src \
    $OUT/sim.cpp $GFX/Adafruit_GFX.cpp -o $OUT/sim
rm -f shots/*.ppm shots/*.png
$OUT/sim shots
python3 - <<'PY'
import glob
from PIL import Image
files = sorted(glob.glob('shots/*.ppm'))
for f in files:
    Image.open(f).save(f[:-4] + '.png')
# contact sheets of 6 screens each, for quick review
ims = [Image.open(f) for f in files]
for n in range(0, len(ims), 6):
    part = ims[n:n+6]
    sheet = Image.new('RGB', (len(part) * 250 + 10, 340), (40, 40, 40))
    for i, im in enumerate(part): sheet.paste(im, (10 + i * 250, 10))
    sheet.save('shots/sheet%d.png' % (n // 6 + 1))
print(len(files), 'screens ->', 'shots/')
PY
rm -f shots/*.ppm
