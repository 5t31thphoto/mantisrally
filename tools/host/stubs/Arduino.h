#pragma once
#include "esp_sdk_macros.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
uint32_t esp_random();
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
inline void* heap_caps_malloc(size_t n, int) { return malloc(n); }
// FreeRTOS surface used by display.cpp. On the host the push task never runs; the harness reads
// the canvas that was last filled instead.
typedef void* TaskHandle_t;
typedef void* SemaphoreHandle_t;
typedef int BaseType_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
inline SemaphoreHandle_t xSemaphoreCreateBinary() { static int s; return &s; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return pdTRUE; }
inline uint32_t ulTaskNotifyTake(BaseType_t, uint32_t) { return 1; }
inline void xTaskNotifyGive(TaskHandle_t) {}
inline void vTaskDelay(uint32_t) {}
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*, int, TaskHandle_t* h, int) { static int t; if (h) *h = &t; return pdTRUE; }
// the serial port (telemetry on the device; quiet here)
struct HostSerial { void begin(unsigned long) {} template <class... A> int printf(const char*, A...) { return 0; } };
static HostSerial Serial;
