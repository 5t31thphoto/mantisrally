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
    float miles[V_COUNT];          // lifetime odometer per vehicle
    float wallet[V_COUNT];         // unspent miles per vehicle
    uint16_t visited;              // biomes seen (bit per biome)
    bool offroadUnlocked, offroadOn;
};
extern Progress g_prog;

void progressInit();               // loads (or starts fresh: the STI)
void progressSave();
void progressDrive(float metres);  // credit the current vehicle; saves now and then
int  progressNextLocked();         // the next vehicle for sale (-1: you own them all)
bool progressCanBuy(int v);
bool progressBuy(int v);
bool progressAllBiomes();

}
