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
    xTaskCreate(blinker_task, "led", 1024, nullptr, tskIDLE_PRIORITY + 1, &s_task);
  }
}

void led_status_set(led_pattern_t p) {
  s_pattern = p;
}
