# Mantis Rally (M5Stack Core2)

A nature-and-weather simulator you explore by rally car. A 160x120 palette renderer doubled to the panel (MantisPlanet's
strip-streaming DMA display), live 24 kHz synthesis on core 0, a real vehicle-dynamics model, and a world that is dreamt
ahead and forgotten behind: coherent, never stored.

## Controls (hold it like a wheel)

| Input | Action |
|---|---|
| Tilt left / right | Steer |
| Tip the top away | Throttle |
| Pull the top back | Brake. Hold it at a standstill to engage reverse; in reverse it drives backwards. Throttle returns to drive. |
| **A** (left button) | Horn (deer bolt) |
| **B** tap | Change view: cockpit, eye level, chase |
| **B** hold (0.65 s) | Re-centre the tilt |
| **C** (right button) | Handbrake (hold) |
| Touch the picture | Use what's in front of you (the dealership) |
| Hold the picture 1.5 s | Mute / unmute |
| Bike in the air | Tip forward / back to pitch the bike for the landing |

The A/B/C buttons are zones of the touch panel below the picture; touches there are buttons only (the old build counted
them as screen touches too, which is why holding a button muted the sound). Steering direction was also inverted; fixed.

## Vehicles

| | Drive | Trick |
|---|---|---|
| **MANTIS STI** (start) | AWD 41/59, turbo flat four, rally tyres, best on gravel | Pace notes from the co-driver (next corner: direction and severity 1-6, H = hairpin, crests). Launch control: brake + throttle at a standstill, release the brake. |
| **OLD JEEP 4.0** | 4x4, soft and bouncy, heavy slow steering, no ABS | Leave the road up to 45 m without the clock; low range kicks in climbing slowly; wades water to 0.85 m. |
| **STINGRAY BAJA** | Lifted V8, RWD, all-terrains: fastest, corners hardest on tarmac, loose on dirt | Shortcuts (18 m off-road band), power-oversteer drifts, full-throttle full-lock donuts that fling mud onto the glass. |
| **CR250 DIRT BIKE** (endgame) | Two-stroke 250, knobbies, wheelies | Goes anywhere, no clock. Off the roads your compass heading picks the next biome; roads cross your path now and then. |

Each vehicle is one row in `VEHICLES` (`src/vehicle.cpp`): mass, wheelbase, weight split, steering, torque curve, gears,
drivetrain split, brakes, tyre grip per surface, Pacejka shape, aero, suspension, off-road band, wading depth, camera, and its
price. A new unlockable is a new row plus its art (see Tools).

## Progression

The currency is miles driven in a particular vehicle. The Jeep costs 20 miles in the STI, the Stingray 40 in the Jeep, the bike
60 in the Stingray. **MANTIS MOTORS** appears beside the road every few miles (sooner once you can afford the next vehicle). Pull
onto the gravel lot and stop. It shows only the next vehicle for sale (BUY greyed until you can afford it), your garage to swap
between, and, once you've seen all 12 biomes, the switch for **off-road mode for every vehicle**. Progress is saved to flash.

## The world

12 biomes on a climate map (north colder and higher, east wetter): Pine Forest, Redwood Cathedral, Lake Country, Rain Moor,
Devil's Pass, Alpine Summit, Red Canyon, Coast Highway, Wildflower Meadow, Autumn Ridge, Bayou Mud, Frost Tundra. A biome is
about 2 miles. Every 3-7 miles the road forks (the first comes early); the sign and a small banner say where each branch
leads and how far. Stay on a road and it routes biome by biome to its destination, then carries on to a new one. Roads go
forever. Each biome sets the road's surface (tarmac, gravel, dirt, mud, snow), width, corner mix (sweepers to switchback
hairpins), grade (up to 12 %), crests (getting air on the road is possible but hard), land, water, plants, colours and weather.

Weather wanders: clear spells, rain, storms with lightning (thunder arrives after the flash), fog, snow, dust, rainbows after
rain, aurora on clear cold nights, fireflies at dusk, falling leaves, light shafts. Headlights come on automatically and light
the road, the paint (it's retro-reflective) and the trees. Water reflects the sky and the land; wet roads shine.

## Views

Cockpit (your interior art, the wheel turns with the steering), eye level (no interior), and chase. The chase vehicles are
rendered from your sprite sheet: each vehicle's side profile, rear view and top view are carved into a voxel model, textured
from those drawings and rendered with true perspective (6 yaw steps mirrored, uphill / level / downhill). The bike's frames are
its rear drawing leaned about the tyre's contact patch.

## Tools

* `tools/sprites/make_vehicles.py`: chase frames from `vehicles_sheet.jpg` (+ top views from `angles_sheet.png`) -> `src/vehicle_art.h`.
  Writes `vehicles_preview.png`. Better side / rear / top drawings in the sheet = better chase sprites, automatically.
* `tools/sprites/make_cockpits.py`: cockpits from `cockpits_sheet.jpg` -> `src/cockpit_art.h`.
* `tools/host/gameplay_test.cpp`: the CI world test (40 miles of road, forks, biomes, dealership, recovery, free roam).
* `tools/host/drive_sim.cpp`: the whole game on the desktop (real setup()/loop(), a tilt autopilot through the IMU path),
  with frame timings. `drive_sim [minutes] [vehicle 0-3] [view 0-2]`.
* `tools/host/snap.cpp` + `render.py`: showcase stills (biomes x times of day x views x vehicles).

Build: `pio run -e m5stack-core2` (the CI workflow and the web installer are unchanged).

## Tuning knobs

`src/helm.cpp` (tilt ranges, expo, inversion), `src/main.cpp` (off-road clock, stall clock, dealership stop), `src/vehicle.cpp`
(every vehicle), `src/journey.cpp` (biome table), `src/terrain.cpp` (road generation, fork spacing, store spacing),
`src/scene.cpp` (weather), `src/render.cpp` (palette keys, fog, cameras), `src/engine.cpp` (engine characters).
