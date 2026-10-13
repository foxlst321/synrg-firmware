# Notes for Claude

- The sketch lives at `synrg/synrg.ino` (folder name must match file name).
- For every requested change: edit `synrg/synrg.ino` AND bump
  `#define FIRMWARE_VERSION` by one step (v1.8 -> v1.9 ...). The PDA only
  offers an update when that number changes.
- ALWAYS update `version.txt` to match `FIRMWARE_VERSION`, and keep the
  "Current version" line in `README.md` in sync.
- `.github/workflows/build.yml` compiles on every push to `main` touching
  `synrg/**` and commits `firmware.bin` + `version.txt`.
- NEVER push a `version.txt` bump to `main` without a `firmware.bin` of the
  same version in the same commit -- if the Actions build fails, devices
  otherwise loop reinstalling the old binary. Build locally first and
  commit firmware.bin + version.txt together; the workflow then rebuilds.
- Arduino auto-generates prototypes at the top of the sketch, so any
  enum/struct used as a function parameter must be declared near the top.
