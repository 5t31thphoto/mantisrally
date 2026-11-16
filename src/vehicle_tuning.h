// Every vehicle's numbers, by name: edit a line and rebuild. (Included once, by vehicle.cpp.)
// The table order is the vehicle ids: STI, Jeep, Vette, bike.
#pragma once
// (inside namespace mantis: vehicle.cpp includes it there)

static VehicleSpec tuneSti() {
    VehicleSpec s = {};
    // ---- the store
    s.name = "MANTIS STI";                                   // shown in the store
    s.shortName = "STI";                                     // shown in the garage list
    s.blurb1 = "2013 HATCH. AWD. TURBO FLAT FOUR.";          // store: what it is
    s.blurb2 = "PACE NOTES, LAUNCH CONTROL";                 // store: a hint (never the trick itself)
    s.bike = false;                                          // a bike (leans, one track)
    // ---- chassis
    s.mass = 1480.f;                                         // kg
    s.wheelbase = 2.62f;                                     // m, axle to axle
    s.frontWeight = 0.60f;                                   // share of the weight on the front axle
    s.cgHeight = 0.50f;                                      // m, centre of gravity above the ground
    s.track = 1.53f;                                         // m, wheel to wheel across
    s.yawInertiaK = 1.05f;                                   // how hard it is to turn the body: yaw inertia = mass x a x b x this
    // ---- steering
    s.lockDeg = 30.f;                                        // steering lock at a standstill, degrees
    s.steerRefSpeed = 26.f;                                  // m/s: the lock eases with speed, lock / sqrt(1 + (speed / this)^2)
    s.steerRate = 3.4f;                                      // how fast the front wheels turn, rad/s
    s.assist = 0.55f;                                        // automatic counter-steer in a slide, 0..1
    // ---- engine
    s.engine = ENG_BOXER4T;                                  // sound model
    s.torque = 410.f;                                        // N m at the peak
    s.peakRpm = 4000.f;                                      // rpm of peak torque
    s.idleRpm = 850.f;                                       // rpm
    s.redline = 6900.f;                                      // rpm
    s.launchRpm = 3600.f;                                    // rpm the clutch slips at under full throttle from a stop
    s.turbo = 0.45f;                                         // 0 = no turbo, else the boost lag in seconds
    // ---- gearbox
    s.gears = 6;                                             // forward gears
    { const float t[6] = {3.64f, 2.24f, 1.52f, 1.14f, 0.97f, 0.76f}; for (int i = 0; i < 6; i++) s.ratio[i] = t[i]; }   // gear ratios (first .. sixth; unused past gears)
    s.finalDrive = 3.90f;                                    // final drive ratio
    s.wheelRadius = 0.32f;                                   // m
    s.shiftTime = 0.11f;                                     // s per gear change
    // ---- drivetrain and brakes
    s.drive = DRIVE_AWD;                                     // driven wheels
    s.frontSplit = 0.41f;                                    // share of the drive to the front (AWD, 4WD)
    s.brakeG = 1.10f;                                        // braking force, in g
    s.brakeBias = 0.62f;                                     // share of the braking on the front
    s.abs = true;                                            // anti-lock brakes
    // ---- tyres
    { const float t[8] = {1.00f, 0.92f, 0.87f, 0.62f, 0.56f, 0.62f, 0.64f, 0.76f}; for (int i = 0; i < 8; i++) s.mu[i] = t[i]; }   // grip on each surface: tarmac, gravel, dirt, mud, snow, sand, grass, rock
    s.tarmacC = 1.55f;                                       // tyre stiffness on paved surfaces
    s.looseC = 1.28f;                                        // tyre stiffness on loose surfaces
    s.peakSlip = 0.115f;                                     // slip at peak grip (x 1.6 on loose)
    s.rollMul = 1.0f;                                        // rolling resistance on loose ground, x
    // ---- aero and suspension
    s.cdA = 0.78f;                                           // drag area, m^2 (drag = 0.6 x this x speed^2)
    s.springHz = 2.1f;                                       // suspension frequency, Hz
    s.damping = 0.62f;                                       // suspension damping ratio
    s.rideHeight = 0.50f;                                    // m
    s.travel = 0.17f;                                        // m of suspension travel
    // ---- roles and the cockpit camera
    s.offroadBand = 3.0f;                                    // m past the road edge before the off-road clock (999 = free roam)
    s.fordDepth = 0.35f;                                     // m of water it can wade
    s.eyeHeight = 1.18f;                                     // m, cockpit camera height
    s.eyeBack = 0.0f;                                        // m, cockpit camera back from the centre of gravity
    s.cockpit = COCKPIT_RALLY;                               // dash art
    // ---- price and the store bars
    s.priceVehicle = V_STI;                                  // paid for in miles driven in this vehicle
    s.priceMiles = 0.f;                                      // miles
    s.statSpeed = 0.80f;                                     // store bar, 0..1
    s.statGrip = 0.85f;                                      // store bar, 0..1
    s.statOffroad = 0.35f;                                   // store bar, 0..1
    return s;
}

static VehicleSpec tuneJeep() {
    VehicleSpec s = {};
    // ---- the store
    s.name = "OLD JEEP 4.0";                                 // shown in the store
    s.shortName = "JEEP";                                    // shown in the garage list
    s.blurb1 = "1993 4.0 SIX. PART-TIME 4X4.";               // store: what it is
    s.blurb2 = "SOME ROADS AREN'T ON ANY MAP.";              // store: a hint (never the trick itself)
    s.bike = false;                                          // a bike (leans, one track)
    // ---- chassis
    s.mass = 1520.f;                                         // kg
    s.wheelbase = 2.37f;                                     // m, axle to axle
    s.frontWeight = 0.54f;                                   // share of the weight on the front axle
    s.cgHeight = 0.78f;                                      // m, centre of gravity above the ground
    s.track = 1.48f;                                         // m, wheel to wheel across
    s.yawInertiaK = 1.10f;                                   // how hard it is to turn the body: yaw inertia = mass x a x b x this
    // ---- steering
    s.lockDeg = 34.f;                                        // steering lock at a standstill, degrees
    s.steerRefSpeed = 17.f;                                  // m/s: the lock eases with speed, lock / sqrt(1 + (speed / this)^2)
    s.steerRate = 1.8f;                                      // how fast the front wheels turn, rad/s
    s.assist = 0.30f;                                        // automatic counter-steer in a slide, 0..1
    // ---- engine
    s.engine = ENG_I6;                                       // sound model
    s.torque = 310.f;                                        // N m at the peak
    s.peakRpm = 2800.f;                                      // rpm of peak torque
    s.idleRpm = 700.f;                                       // rpm
    s.redline = 5000.f;                                      // rpm
    s.launchRpm = 2000.f;                                    // rpm the clutch slips at under full throttle from a stop
    s.turbo = 0.f;                                           // 0 = no turbo, else the boost lag in seconds
    // ---- gearbox
    s.gears = 5;                                             // forward gears
    { const float t[6] = {3.83f, 2.45f, 1.56f, 1.00f, 0.79f, 0.f}; for (int i = 0; i < 6; i++) s.ratio[i] = t[i]; }   // gear ratios (first .. sixth; unused past gears)
    s.finalDrive = 4.10f;                                    // final drive ratio
    s.wheelRadius = 0.39f;                                   // m
    s.shiftTime = 0.22f;                                     // s per gear change
    // ---- drivetrain and brakes
    s.drive = DRIVE_4WD;                                     // driven wheels
    s.frontSplit = 0.50f;                                    // share of the drive to the front (AWD, 4WD)
    s.brakeG = 0.85f;                                        // braking force, in g
    s.brakeBias = 0.65f;                                     // share of the braking on the front
    s.abs = false;                                           // anti-lock brakes
    // ---- tyres
    { const float t[8] = {0.84f, 0.88f, 0.90f, 0.84f, 0.70f, 0.82f, 0.82f, 0.95f}; for (int i = 0; i < 8; i++) s.mu[i] = t[i]; }   // grip on each surface: tarmac, gravel, dirt, mud, snow, sand, grass, rock
    s.tarmacC = 1.40f;                                       // tyre stiffness on paved surfaces
    s.looseC = 1.18f;                                        // tyre stiffness on loose surfaces
    s.peakSlip = 0.16f;                                      // slip at peak grip (x 1.6 on loose)
    s.rollMul = 0.45f;                                       // rolling resistance on loose ground, x
    // ---- aero and suspension
    s.cdA = 1.20f;                                           // drag area, m^2 (drag = 0.6 x this x speed^2)
    s.springHz = 1.25f;                                      // suspension frequency, Hz
    s.damping = 0.24f;                                       // suspension damping ratio
    s.rideHeight = 0.62f;                                    // m
    s.travel = 0.30f;                                        // m of suspension travel
    // ---- roles and the cockpit camera
    s.offroadBand = 45.f;                                    // m past the road edge before the off-road clock (999 = free roam)
    s.fordDepth = 0.85f;                                     // m of water it can wade
    s.eyeHeight = 1.62f;                                     // m, cockpit camera height
    s.eyeBack = 0.0f;                                        // m, cockpit camera back from the centre of gravity
    s.cockpit = COCKPIT_JEEP;                                // dash art
    // ---- price and the store bars
    s.priceVehicle = V_STI;                                  // paid for in miles driven in this vehicle
    s.priceMiles = 20.f;                                     // miles
    s.statSpeed = 0.45f;                                     // store bar, 0..1
    s.statGrip = 0.60f;                                      // store bar, 0..1
    s.statOffroad = 0.85f;                                   // store bar, 0..1
    return s;
}

static VehicleSpec tuneVette() {
    VehicleSpec s = {};
    // ---- the store
    s.name = "STINGRAY BAJA";                                // shown in the store
    s.shortName = "VETTE";                                   // shown in the garage list
    s.blurb1 = "LIFTED V8 ON ALL-TERRAINS. RWD.";            // store: what it is
    s.blurb2 = "WHY TAKE THE LONG WAY ROUND?";               // store: a hint (never the trick itself)
    s.bike = false;                                          // a bike (leans, one track)
    // ---- chassis
    s.mass = 1450.f;                                         // kg
    s.wheelbase = 2.49f;                                     // m, axle to axle
    s.frontWeight = 0.50f;                                   // share of the weight on the front axle
    s.cgHeight = 0.66f;                                      // m, centre of gravity above the ground
    s.track = 1.58f;                                         // m, wheel to wheel across
    s.yawInertiaK = 1.0f;                                    // how hard it is to turn the body: yaw inertia = mass x a x b x this
    // ---- steering
    s.lockDeg = 32.f;                                        // steering lock at a standstill, degrees
    s.steerRefSpeed = 30.f;                                  // m/s: the lock eases with speed, lock / sqrt(1 + (speed / this)^2)
    s.steerRate = 3.9f;                                      // how fast the front wheels turn, rad/s
    s.assist = 0.30f;                                        // automatic counter-steer in a slide, 0..1
    // ---- engine
    s.engine = ENG_V8;                                       // sound model
    s.torque = 900.f;                                        // N m at the peak
    s.peakRpm = 4400.f;                                      // rpm of peak torque
    s.idleRpm = 750.f;                                       // rpm
    s.redline = 6600.f;                                      // rpm
    s.launchRpm = 3000.f;                                    // rpm the clutch slips at under full throttle from a stop
    s.turbo = 0.f;                                           // 0 = no turbo, else the boost lag in seconds
    // ---- gearbox
    s.gears = 5;                                             // forward gears
    { const float t[6] = {2.64f, 1.75f, 1.34f, 1.00f, 0.78f, 0.f}; for (int i = 0; i < 6; i++) s.ratio[i] = t[i]; }   // gear ratios (first .. sixth; unused past gears)
    s.finalDrive = 3.55f;                                    // final drive ratio
    s.wheelRadius = 0.40f;                                   // m
    s.shiftTime = 0.14f;                                     // s per gear change
    // ---- drivetrain and brakes
    s.drive = DRIVE_RWD;                                     // driven wheels
    s.frontSplit = 0.0f;                                     // share of the drive to the front (AWD, 4WD)
    s.brakeG = 1.15f;                                        // braking force, in g
    s.brakeBias = 0.60f;                                     // share of the braking on the front
    s.abs = true;                                            // anti-lock brakes
    // ---- tyres
    { const float t[8] = {1.55f, 1.32f, 1.32f, 1.12f, 0.88f, 1.25f, 1.18f, 1.25f}; for (int i = 0; i < 8; i++) s.mu[i] = t[i]; }   // grip on each surface: tarmac, gravel, dirt, mud, snow, sand, grass, rock. grip to match 900 Nm: insane traction
    s.tarmacC = 1.50f;                                       // tyre stiffness on paved surfaces
    s.looseC = 1.20f;                                        // tyre stiffness on loose surfaces
    s.peakSlip = 0.17f;                                      // slip at peak grip (x 1.6 on loose)
    s.rollMul = 0.50f;                                       // rolling resistance on loose ground, x
    // ---- aero and suspension
    s.cdA = 0.86f;                                           // drag area, m^2 (drag = 0.6 x this x speed^2)
    s.springHz = 1.40f;                                      // suspension frequency, Hz
    s.damping = 0.55f;                                       // suspension damping ratio
    s.rideHeight = 0.60f;                                    // m
    s.travel = 0.40f;                                        // m of suspension travel
    // ---- roles and the cockpit camera
    s.offroadBand = 18.f;                                    // m past the road edge before the off-road clock (999 = free roam)
    s.fordDepth = 0.45f;                                     // m of water it can wade
    s.eyeHeight = 1.38f;                                     // m, cockpit camera height
    s.eyeBack = 0.0f;                                        // m, cockpit camera back from the centre of gravity
    s.cockpit = COCKPIT_VETTE;                               // dash art
    // ---- price and the store bars
    s.priceVehicle = V_JEEP;                                 // paid for in miles driven in this vehicle
    s.priceMiles = 40.f;                                     // miles
    s.statSpeed = 1.0f;                                      // store bar, 0..1
    s.statGrip = 0.95f;                                      // store bar, 0..1
    s.statOffroad = 0.60f;                                   // store bar, 0..1
    return s;
}

static VehicleSpec tuneBike() {
    VehicleSpec s = {};
    // ---- the store
    s.name = "CR250 DIRT BIKE";                              // shown in the store
    s.shortName = "BIKE";                                    // shown in the garage list
    s.blurb1 = "TWO-STROKE 250. KNOBBIES.";                  // store: what it is
    s.blurb2 = "WHAT IF THE ROAD WAS OPTIONAL?";             // store: a hint (never the trick itself)
    s.bike = true;                                           // a bike (leans, one track)
    // ---- chassis
    s.mass = 190.f;                                          // kg
    s.wheelbase = 1.48f;                                     // m, axle to axle
    s.frontWeight = 0.47f;                                   // share of the weight on the front axle
    s.cgHeight = 0.95f;                                      // m, centre of gravity above the ground
    s.track = 0.20f;                                         // m, wheel to wheel across
    s.yawInertiaK = 0.7f;                                    // how hard it is to turn the body: yaw inertia = mass x a x b x this
    // ---- steering
    s.lockDeg = 32.f;                                        // steering lock at a standstill, degrees
    s.steerRefSpeed = 22.f;                                  // m/s: the lock eases with speed, lock / sqrt(1 + (speed / this)^2)
    s.steerRate = 6.0f;                                      // how fast the front wheels turn, rad/s
    s.assist = 0.25f;                                        // automatic counter-steer in a slide, 0..1
    // ---- engine
    s.engine = ENG_2STROKE;                                  // sound model
    s.torque = 48.f;                                         // N m at the peak
    s.peakRpm = 8000.f;                                      // rpm of peak torque
    s.idleRpm = 1600.f;                                      // rpm
    s.redline = 10000.f;                                     // rpm
    s.launchRpm = 5000.f;                                    // rpm the clutch slips at under full throttle from a stop
    s.turbo = 0.f;                                           // 0 = no turbo, else the boost lag in seconds
    // ---- gearbox
    s.gears = 5;                                             // forward gears
    { const float t[6] = {2.20f, 1.68f, 1.36f, 1.13f, 0.96f, 0.f}; for (int i = 0; i < 6; i++) s.ratio[i] = t[i]; }   // gear ratios (first .. sixth; unused past gears)
    s.finalDrive = 11.8f;                                    // final drive ratio
    s.wheelRadius = 0.33f;                                   // m
    s.shiftTime = 0.06f;                                     // s per gear change
    // ---- drivetrain and brakes
    s.drive = DRIVE_RWD;                                     // driven wheels
    s.frontSplit = 0.0f;                                     // share of the drive to the front (AWD, 4WD)
    s.brakeG = 1.05f;                                        // braking force, in g
    s.brakeBias = 0.65f;                                     // share of the braking on the front
    s.abs = true;                                            // anti-lock brakes
    // ---- tyres
    { const float t[8] = {0.92f, 0.95f, 1.00f, 0.86f, 0.70f, 0.88f, 0.94f, 0.92f}; for (int i = 0; i < 8; i++) s.mu[i] = t[i]; }   // grip on each surface: tarmac, gravel, dirt, mud, snow, sand, grass, rock
    s.tarmacC = 1.45f;                                       // tyre stiffness on paved surfaces
    s.looseC = 1.20f;                                        // tyre stiffness on loose surfaces
    s.peakSlip = 0.15f;                                      // slip at peak grip (x 1.6 on loose)
    s.rollMul = 0.45f;                                       // rolling resistance on loose ground, x
    // ---- aero and suspension
    s.cdA = 0.55f;                                           // drag area, m^2 (drag = 0.6 x this x speed^2)
    s.springHz = 1.20f;                                      // suspension frequency, Hz
    s.damping = 0.50f;                                       // suspension damping ratio
    s.rideHeight = 0.55f;                                    // m
    s.travel = 0.40f;                                        // m of suspension travel
    // ---- roles and the cockpit camera
    s.offroadBand = 999.f;                                   // m past the road edge before the off-road clock (999 = free roam)
    s.fordDepth = 0.60f;                                     // m of water it can wade
    s.eyeHeight = 1.52f;                                     // m, cockpit camera height
    s.eyeBack = 0.25f;                                       // m, cockpit camera back from the centre of gravity
    s.cockpit = COCKPIT_BIKE;                                // dash art
    // ---- price and the store bars
    s.priceVehicle = V_VETTE;                                // paid for in miles driven in this vehicle
    s.priceMiles = 60.f;                                     // miles
    s.statSpeed = 0.85f;                                     // store bar, 0..1
    s.statGrip = 0.80f;                                      // store bar, 0..1
    s.statOffroad = 1.0f;                                    // store bar, 0..1
    return s;
}
