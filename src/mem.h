#pragma once
#include <stdlib.h>
#include <string.h>
#ifndef HOST
#include <esp_heap_caps.h>
#endif
// Runtime buffers. The ESP32's static RAM segment is small (the linker overflows if big arrays are
// declared static), so large buffers are allocated once at runtime instead:
//   memSlow: big, rarely touched -> PSRAM first
//   memFast: on a hot path       -> internal RAM first
namespace mantis {
inline void* memSlow(size_t n) {
#ifdef HOST
    return calloc(1, n);
#else
    void* p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
#endif
}
inline void* memFast(size_t n) {
#ifdef HOST
    return calloc(1, n);
#else
    void* p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}
}
