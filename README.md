# Mantis Rally

Infinite branching rally / drift on M5Stack Core2. Same systems family as MantisPlanet and Mantis Seadiver: procedural stages, tilt controls, live audio, palette renderer.

## Controls

| Input | Action |
|-------|--------|
| Tip forward | Throttle |
| Pull back | Brake (does not force reverse at a standstill) |
| Tilt left / right | Steer |
| Drag on the touchscreen | Fine steering |
| Tap a roadside object/sign | Scan it into the databank |
| Hold the touchscreen | Toggle mute |
| **A** | Horn / scare nearby spectators |
| **B** | Re-centre the IMU steering neutral |
| Hold **C** | Handbrake / initiate a drift |

## Gameplay

- Drive freely over the 3D heightfield. The road is a curved ribbon, not an invisible wall.
- Cutting across rough ground costs grip and speed and produces controller rumble.
- Staying off the road starts a visible three-second recovery timer. Re-enter the road to cancel it; when it expires, the car returns to the nearest road edge with reduced momentum.
- Forks display left/right biome destinations. A committed fork bends the road toward the chosen branch, and that biome becomes the route target.
- Sustained, fast slides bank drift points when the drift ends. The HUD shows speed, distance, current biome, grip, score, fork distance, scanning state, and off-road recovery.
- The world is endless: biome progression, road forks, weather, day/night, props, scans, and drift scoring are the core loop rather than a separate race-finish screen.

## Build (local)

```bash
pio run -e m5stack-core2
pio run -t upload
```

`merge_bin.py` runs after build and writes `firmware_merged.bin` (bootloader + partitions + app) for the web flasher.

## GitHub Actions + Pages

Workflow: `GitHub/workflows/ci.yml` (repo maps this into `.github/workflows` — do not ship a `.github` folder in the payload zip).

On push to `main` / `master` (or manual dispatch):

1. Optional root `*.zip` payload is extracted into the repo and consumed
2. PlatformIO builds `m5stack-core2`
3. Merged firmware is copied to `website/firmware/firmware-merged.bin`
4. `website/` is deployed to **GitHub Pages**

Enable **Settings → Pages → Build and deployment → GitHub Actions**.

## Web flasher

Live site (after Pages deploy): the install button uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) against:

```
website/firmware/manifest.json
website/firmware/firmware-merged.bin
```

Chrome / Edge + USB. Same pattern as Planet and Seadiver.

Manual flash:

```bash
esptool.py --chip esp32 write_flash 0x0 firmware_merged.bin
```

## Host tools

```bash
# gameplay regression checks (no PlatformIO required)
g++ -O2 -DHOST -I src -I tools/host/stubs tools/host/gameplay_test.cpp \
  src/universe.cpp src/journey.cpp src/terrain.cpp -lm -o /tmp/gameplay_test
/tmp/gameplay_test

# showcase stills
g++ -O2 -DHOST -I src -I tools/host/stubs tools/host/snap_rally.cpp \
  src/universe.cpp src/journey.cpp src/terrain.cpp src/field.cpp \
  src/scene.cpp src/render.cpp src/databank.cpp -lm -o /tmp/snap_rally
# driving GIF frames
g++ -O2 -DHOST -I src -I tools/host/stubs tools/host/drive_gif.cpp \
  ... same objects ... -lm -o /tmp/drive_gif
```

## Architecture

- **Helm** — tilt throttle / brake, C handbrake, steer
- **Terrain** — Seadiver-style scrolling heightfield, road cut, banks, junctions
- **Journey** — 17 biomes, branch commits
- **Field** — roadside props, slide collisions
- **Render** — TOD palette (Planet), heightmap ground raycast, props
- **Audio** — RPM engine bed, surface layers, reactive score
- **Haptics** — motor gestures for slip / bumps / shifts
