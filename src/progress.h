#pragma once
#include <stdint.h>
#include "vehicle.h"

// What you own and what you've earned. The currency is miles driven in a particular vehicle: the next vehicle
// is paid for with miles in the one before it (see VehicleSpec::priceVehicle / priceMiles). Saved to flash.

namespace mantis {

struct Progress {
    uint8_t owned;                 // bit per vehicle
    uint8_t current;
    uint8_t view;
    uint8_t radio;                 // the radio: bits 0-1 station, 2-4 volume (0: the default), bit 7 set once saved
    float miles[V_COUNT];          // lifetime odometer per vehicle
    float wallet[V_COUNT];         // unspent miles per vehicle
    uint16_t visited;              // biomes seen (bit per biome)
    bool offroadUnlocked, offroadOn;
    // ---- added at checkpoint 45 (a 40-byte save from before loads with these zero) ----
    uint32_t style;                // unspent Style: earned with airtime and drifts, spent on tuning at the dealership
    uint8_t upg[V_COUNT];          // tuning per vehicle: bits 0-1 handling, 2-3 grip, 4-5 power (vehicleSetUpgrades)
};
static_assert(sizeof(Progress) == 48, "the save's size; the first 40 bytes are the old save's, exactly");
extern Progress g_prog;

void progressInit();               // loads (or starts fresh: the STI)
void progressSave();
void progressDrive(float metres);  // credit the current vehicle; saves now and then
int  progressNextLocked();         // the next vehicle for sale (-1: you own them all)
bool progressCanBuy(int v);
bool progressBuy(int v);
bool progressAllBiomes();
void progressStyle(int points);    // bank Style (airtime, drifts)
int  progressUpgradeLevel(int kind);           // the current vehicle's level (0-3) in UPG_*
int  progressUpgradeCost(int kind);            // Style for its next level (0: maxed)
bool progressBuyUpgrade(int kind);

}
