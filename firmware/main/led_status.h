// led_status.h — reusable status LED indicator.
//
// A single GPIO LED is the only feedback you have before the serial console
// comes up and before WiFi is up. Each phase of the firmware reuses this to
// signal its state, so the bring-up progress is visible without a logic
// analyzer.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LED blink patterns. Periods are in ms; the cycle repeats.
typedef enum {
  LED_OFF = 0,
  LED_ON,            // solid: booted, idle
  LED_BLINK_SLOW,    // 1 Hz: bring-up OK, waiting for show
  LED_BLINK_FAST,    // 4 Hz: running the render loop
  LED_BLINK_DOUBLE,  // double-pulse: WiFi connected
  LED_ERROR,         // 5 Hz error: no bundle / corrupt / OTA fail
} led_pattern_t;

// Initialise the status LED on the given GPIO. Safe to call once from app_main.
void led_status_init(int gpio);

// Set the current pattern. The blinker runs in its own FreeRTOS task, so this
// just updates the active pattern atomically.
void led_status_set(led_pattern_t p);

#ifdef __cplusplus
}
#endif
