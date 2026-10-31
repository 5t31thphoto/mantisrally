#pragma once
#include <stdint.h>

// MANTIS MOTORS: the roadside dealership. Shows the NEXT vehicle for sale (only that one), what it costs (miles
// driven in a particular vehicle), your garage to swap between, and the off-road switch once every biome is
// visited. Drawn into the full-resolution overlay; driven by taps.

namespace mantis {
void storeOpen();
bool storeActive();
// returns true while the store stays open; *changed = the vehicle changed
bool storeUpdate(bool tap, int tx, int ty, bool& changed);
}
