#pragma once
#include <stdint.h>
#include <stddef.h>

// The SD card (as MantisPlanet): it shares the SPI bus with the LCD, so every card operation holds the bus.
//   /rally_saves/progress.bin   the save (+ progress.bak, the one before it). Written to a .tmp first.
//   /images/rally/rally_NNNN.mg   photos (MG01: 320 x 240 RGB565 little-endian, see mgimage in MantisPlanet)

namespace mantis {

bool storageInit();                     // mounts the card (false: no card)
bool storageOk();
bool storageSave(const void* data, size_t n);           // the save, crash-safe
bool storageLoad(void* data, size_t n);                  // the newest good save (progress.bin, else .bak)
// A photo, one 320-pixel row at a time (native RGB565). Returns the number used, or -1.
int  storagePhoto(void (*row)(int y, uint16_t* out));

}
