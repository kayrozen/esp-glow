// led_status.cpp — status LED blinker running in its own FreeRTOS task.
//
// Kept deliberately tiny: one task, one GPIO, a small pattern table. Later
// phases call led_status_set() to reflect WiFi / render / OTA / error state.
#include "led_status.h"

#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static int s_gpio = -1;
static led_pattern_t s_pattern = LED_OFF;
static TaskHandle_t s_task = nullptr;

static void set_led(int on) {
  if (s_gpio < 0) return;
  gpio_set_level((gpio_num_t)s_gpio, on ? 1 : 0);
}

static void blinker_task(void*) {
  // Each pattern: (on_ms, off_ms, on_ms, off_ms, cycle_ms). cycle_ms==0 means
  // the pair repeats.
  struct phase { uint16_t a, b; uint16_t cycle; };
  static const phase table[] = {
    [LED_OFF]          = {   0,   0, 0 },   // off
    [LED_ON]           = {1000,   0, 0 },   // solid on
    [LED_BLINK_SLOW]   = { 500, 500, 0 },
    [LED_BLINK_FAST]   = { 125, 125, 0 },
    [LED_BLINK_DOUBLE] = { 120, 120, 0 },   // handled specially below
    [LED_ERROR]        = { 100, 100, 0 },
  };

  while (true) {
    led_pattern_t p = s_pattern;
    if (p == LED_OFF) { set_led(0); vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    if (p == LED_ON)  { set_led(1); vTaskDelay(pdMS_TO_TICKS(200)); continue; }

    if (p == LED_BLINK_DOUBLE) {
      set_led(1); vTaskDelay(pdMS_TO_TICKS(120));
      set_led(0); vTaskDelay(pdMS_TO_TICKS(120));
      set_led(1); vTaskDelay(pdMS_TO_TICKS(120));
      set_led(0); vTaskDelay(pdMS_TO_TICKS(640));
      continue;
    }

    const phase& ph = table[p];
    set_led(1); vTaskDelay(pdMS_TO_TICKS(ph.a));
    set_led(0); vTaskDelay(pdMS_TO_TICKS(ph.b));
  }
}

void led_status_init(int gpio) {
  s_gpio = gpio;
  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << gpio);
  io.mode = GPIO_MODE_OUTPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  set_led(0);

  if (s_task == nullptr) {
    // Pin to core 0 (PRO_CPU, the same core app_main runs on), NOT
    // tskNO_AFFINITY. Architectural invariant for this project: core 1
    // (APP_CPU) runs the 44 Hz render/DMX task and NOTHING else; every other
    // task -- network, MIDI, OSC, this blinker, wifi reconnect -- lives on
    // core 0 (see the CI guard that fails on a bare xTaskCreate in
    // .github/workflows/). A trivial status blinker has no business sharing
    // the real-time core.
    //
    // This is HYGIENE, not the boot-stall fix: the actual QEMU boot stall was
    // the otadata esp_partition_mmap running with a second task already alive
    // on another core, and it is fixed by ordering ota_manager_init() into
    // the single-task boot window (see main.cpp). Pinning this blinker alone
    // only masked that stall (~33% -> ~5%); the reorder closed it (0/50).
    // Pinning still matters so a flash op this task's cached loop body might
    // trigger later can never contend across cores mid-show.
    xTaskCreatePinnedToCore(blinker_task, "led", 1024, nullptr,
                            tskIDLE_PRIORITY + 1, &s_task, 0);
  }
}

void led_status_set(led_pattern_t p) {
  s_pattern = p;
}
