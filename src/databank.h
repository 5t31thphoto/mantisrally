#pragma once
#include <stdint.h>

// Stages finished, best times, scanned roadside things. Persisted in flash on device.

namespace mantis {

struct ScanCard {
    uint8_t species;
    const char* name;
    const char* line1;
    const char* line2;
};

void databankInit();
void databankSave();
void databankLoad();

void databankRecordStage(uint8_t stage, float timeSec);
float databankBestTime(uint8_t stage);

void databankScan(uint8_t species);
bool databankHas(uint8_t species);
const ScanCard* databankCard(uint8_t species);
int  databankScanCount();

}
