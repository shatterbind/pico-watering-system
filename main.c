#include <arm_acle.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/powman.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

#include "canvas.h"
#include "ds3231.h"
#include "epd.h"
#include "epd_2_13.h"
#include "images/watering_template.h"

#define HUMIDITY_ADC_PIN 0 // 26

#define AIR_VALUE 4095.0f
#define WATER_VALUE 1100.0f

#define MIN_HUMIDITY 40.0f

#define TIME_POMP_ON 5000
#define POMP_PIN 22
#define USE_RELAY_GND_ON true

#define TEMP_SENSOR_CHANNEL 4

/* =========================================================================
 * Supply voltage (VSYS) measurement
 * The Pico 2 has an on-board 200k/100k divider from VSYS to GP29 (ADC3),
 * so ADC3 reads VSYS / 3.
 * ========================================================================= */
#define VSYS_ADC_GPIO 29
#define VSYS_ADC_CHANNEL 3
#define VSYS_DIVIDER 3.0f
#define VSYS_SAMPLES 16

// Li-ion thresholds (single cell, measured at rest before the pump runs).
// Above VSYS_USB_V the board is assumed to be on USB / external 5 V.
#define VSYS_USB_V 4.40f
#define BATTERY_3_BARS_V 3.95f // ~70-100 %
#define BATTERY_2_BARS_V 3.75f // ~40-70 %
#define BATTERY_1_BAR_V 3.55f  // ~10-40 %, below: empty, charge now
#define WAKEUP_GPIO 15

static powman_power_state off_state = POWMAN_POWER_STATE_NONE;
static powman_power_state on_state = POWMAN_POWER_STATE_NONE;

#define I2C_BAUDRATE (100 * 1000)
#define I2C_SCL_PIN 20
#define I2C_SDA_PIN 21

#define CONFIGURE_RTC 1

/* =========================================================================
 * Wake-up schedule (DS3231 Alarm 1)
 * The board wakes every WAKE_INTERVAL_DAYS days + WAKE_INTERVAL_HOURS hours,
 * always on the full hour (hh:00:00). Each wake schedules the next one.
 * Total interval: 1 hour .. 27 days (Alarm 1 matches day-of-month, so the
 * interval must be shorter than the shortest month).
 * ========================================================================= */
#define WAKE_INTERVAL_DAYS 0
#define WAKE_INTERVAL_HOURS 1

#define WAKE_INTERVAL_TOTAL_HOURS                                              \
  (WAKE_INTERVAL_DAYS * 24 + WAKE_INTERVAL_HOURS)

_Static_assert(WAKE_INTERVAL_DAYS >= 0 && WAKE_INTERVAL_HOURS >= 0,
               "Wake interval must not be negative");
_Static_assert(WAKE_INTERVAL_TOTAL_HOURS >= 1,
               "Wake interval must be at least 1 hour");
_Static_assert(WAKE_INTERVAL_TOTAL_HOURS < 28 * 24,
               "Wake interval must be shorter than 28 days");

// #define DEBUG

/**
 * @brief Delay in seconds to compensate for compilation and upload time.
 */
#define COMPILE_UPLOAD_OFFSET_SECONDS 6

#define ADC_CONVERSION_FACTOR (3.3f / (1 << 12))

/* =========================================================================
 * Display & Dashboard Template Configuration
 * ========================================================================= */
#define EPD_DC_PIN 8
#define CANVAS_BUFFER_SIZE (((EPD_2_13_WIDTH * 2 + 7) / 8) * EPD_2_13_HEIGHT)

// Battery Slot
#define SLOT_BATTERY_X 90
#define SLOT_BATTERY_Y 5

// Battery icon drawn in the template at (66, 6), 17x9 px with 3 empty bar
// slots (see generate_watering_template.py: draw_battery_icon).
#define BATTERY_ICON_X 66
#define BATTERY_ICON_Y 6
#define BATTERY_BAR_X0 (BATTERY_ICON_X + 2)
#define BATTERY_BAR_Y0 (BATTERY_ICON_Y + 2)
#define BATTERY_BAR_STEP 4
#define BATTERY_BAR_WIDTH 3
#define BATTERY_BAR_HEIGHT 5

// Plant Hero Card
#define SLOT_PLANT_NAME_X 12
#define SLOT_PLANT_NAME_Y 38
#define PLANT_NAME_TEXT "MONSTERA"

#define SLOT_PLANT_SUB_X 12
#define SLOT_PLANT_SUB_Y 54
#define PLANT_SUB_TEXT "DELICIOSA"

#define SLOT_PLANT_ZONE_X 12
#define SLOT_PLANT_ZONE_Y 64

// Soil Moisture Card
#define SLOT_MOISTURE_PCT_X 42
#define SLOT_MOISTURE_PCT_Y 92

#define SLOT_MOISTURE_STATUS_X 42
#define SLOT_MOISTURE_STATUS_Y 112
#define STATUS_WATERING_TEXT "WATERING!"
#define STATUS_OPTIMAL_TEXT "OPTIMAL"

// Moisture Progress Bar
#define MOISTURE_BAR_X 13
#define MOISTURE_BAR_Y 127
#define MOISTURE_BAR_MAX_WIDTH 95
#define MOISTURE_BAR_HEIGHT 5

// Temperature Card
#define SLOT_TEMP_VAL_X 42
#define SLOT_TEMP_VAL_Y 164

#define SLOT_TEMP_LABEL_X 42
#define SLOT_TEMP_LABEL_Y 184
#define TEMP_LABEL_TEXT "AMBIENT TEMP"

// Footer Card
#define SLOT_FOOTER_TIME_X 30
#define SLOT_FOOTER_TIME_Y 224

#define SLOT_FOOTER_CYCLE_X 30
#define SLOT_FOOTER_CYCLE_Y 235
#define FOOTER_CYCLE_FORMAT "Alarm: %s cycle" // %s = e.g. "1h", "2d", "1d 6h"

static int enter_power_off_state(void) {
  stdio_flush();

  bool valid_state = powman_configure_wakeup_state(off_state, on_state);

  if (!valid_state) {
    return PICO_ERROR_INVALID_STATE;
  }

  powman_hw->boot[0] = 0;
  powman_hw->boot[1] = 0;
  powman_hw->boot[2] = 0;
  powman_hw->boot[3] = 0;

  int rc = powman_set_power_state(off_state);

  if (rc != PICO_OK) {
    return rc;
  }

  while (true) {
    __wfi();
  }

  return rc;
}

void humidity_sensor_init(uint gpio) {
  adc_init();
  adc_set_temp_sensor_enabled(true);
  adc_gpio_init(gpio);
  adc_gpio_init(VSYS_ADC_GPIO);
}

/** Supply voltage on VSYS in volts (averaged). */
static float read_vsys(void) {
  adc_select_input(VSYS_ADC_CHANNEL);

  uint32_t sum = 0;
  for (int i = 0; i < VSYS_SAMPLES; i++) {
    sum += adc_read();
  }

  return ((float)sum / VSYS_SAMPLES) * ADC_CONVERSION_FACTOR * VSYS_DIVIDER;
}

/** Number of battery bars (0-3) for a single Li-ion cell voltage. */
static int battery_bars(float vsys) {
  if (vsys >= BATTERY_3_BARS_V) return 3; // also USB power
  if (vsys >= BATTERY_2_BARS_V) return 2;
  if (vsys >= BATTERY_1_BAR_V) return 1;
  return 0;
}

static void draw_battery(epd_t *epd, float vsys, uint8_t *canvas) {
  const int bars = battery_bars(vsys);
  // Same colors as the template preview: yellow when >1 bar, red when low.
  const color_t bar_color = bars > 1 ? BLACK : RED;

  for (int i = 0; i < bars; i++) {
    fill_rect(epd, BATTERY_BAR_X0 + i * BATTERY_BAR_STEP, BATTERY_BAR_Y0,
              BATTERY_BAR_WIDTH, BATTERY_BAR_HEIGHT, bar_color, canvas);
  }

  char text[8];
  if (vsys >= VSYS_USB_V) {
    snprintf(text, sizeof(text), "USB");
  } else {
    snprintf(text, sizeof(text), "%.2fV", vsys);
  }

  draw_text_with_bg(epd, SLOT_BATTERY_X, SLOT_BATTERY_Y, text, FONT_TAHOMA_8,
                    bars <= 1 ? RED : BLACK, 1, WHITE, canvas);
}

void pomp(float humidity) {

  if (humidity < MIN_HUMIDITY) {
    gpio_put(POMP_PIN, !USE_RELAY_GND_ON);
    sleep_ms(TIME_POMP_ON);
  }

  gpio_put(POMP_PIN, USE_RELAY_GND_ON);
}

static void core1_entry(void) {
  uint32_t raw = multicore_fifo_pop_blocking();
  float humidity;
  memcpy(&humidity, &raw, sizeof(float));
  pomp(humidity);
  multicore_fifo_push_blocking(1); // signal done
}

static float read_onboard_temperature(bool toCelsius) {
  float adc;
  float tempC;

  adc_select_input(TEMP_SENSOR_CHANNEL);

  adc = (float)adc_read() * ADC_CONVERSION_FACTOR;
  tempC = 27.0f - (adc - 0.706f) / 0.001721f;

  return toCelsius ? tempC : tempC * 9 / 5 + 32;
}

static uint16_t read_humiduty() {
  uint16_t raw;

  adc_select_input(HUMIDITY_ADC_PIN);
  raw = adc_read();

  return raw;
}

/** Sortable key for comparing two RTC date/times (2000-2099). */
static uint64_t datetime_key(const ds3231_datetime_t *dt) {
  return ((uint64_t)dt->year << 40) | ((uint64_t)dt->month << 32) |
         ((uint64_t)dt->day << 24) | ((uint64_t)dt->hour << 16) |
         ((uint64_t)dt->min << 8) | dt->sec;
}

/**
 * Sets the RTC from the firmware compile time, but only when the RTC has lost
 * its time: the oscillator-stop flag is set (new module, dead coin cell) or
 * the RTC is behind the compile time (first boot after flashing).
 *
 * Every wake is a full reboot, so writing the time unconditionally would roll
 * the clock back to the compile time on every wake.
 */
static void setup_rtc(void) {
  struct tm compile_tm = {0};

  sscanf(__DATE__, "%*s %d %d", &compile_tm.tm_mday, &compile_tm.tm_year);

  compile_tm.tm_year -= 1900; // tm_year is years since 1900

  sscanf(__TIME__, "%d:%d:%d", &compile_tm.tm_hour, &compile_tm.tm_min,
         &compile_tm.tm_sec);

  const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char month_str[4];

  sscanf(__DATE__, "%3s", month_str);

  for (int i = 0; i < 12; i++) {
    if (strcmp(month_str, months[i]) == 0) {
      compile_tm.tm_mon = i;
      break;
    }
  }

  time_t compile_time_t = mktime(&compile_tm);
  compile_time_t += COMPILE_UPLOAD_OFFSET_SECONDS;
  struct tm *now_tm = localtime(&compile_time_t);

  ds3231_datetime_t dt_to_set = {
      .year = (uint8_t)(now_tm->tm_year % 100),
      .month = (uint8_t)(now_tm->tm_mon + 1),
      .day = (uint8_t)now_tm->tm_mday,
      .dow = (uint8_t)(now_tm->tm_wday + 1), // tm_wday is 0-6, DS3231 is 1-7
      .hour = (uint8_t)now_tm->tm_hour,
      .min = (uint8_t)now_tm->tm_min,
      .sec = (uint8_t)now_tm->tm_sec};

  bool osc_stopped = false;
  ds3231_datetime_t current = {0};

  // If the RTC can't be read, don't write either: a transient I2C error must
  // not roll a good clock back to the compile time.
  if (!ds3231_oscillator_stopped(&osc_stopped) ||
      !ds3231_read_time(&current)) {
    printf("ERROR: Failed to read RTC, time not changed.\n");
    return;
  }

  const bool behind = datetime_key(&current) < datetime_key(&dt_to_set);

  if (!osc_stopped && !behind) {
    return; // RTC keeps its own time
  }

  printf("RTC %s, setting it from compile time...\n",
         osc_stopped ? "lost power" : "is behind compile time");

  if (ds3231_set_time(&dt_to_set) && ds3231_clear_oscillator_stop_flag()) {
    printf("Time set successfully!\n");
  } else {
    printf("ERROR: Failed to set time.\n");
  }
}

/**
 * Arms Alarm 1 for the next wake: the current hour rounded down to hh:00:00
 * plus WAKE_INTERVAL_TOTAL_HOURS. If the RTC time is unknown, falls back to
 * waking at the next full hour so the board never sleeps forever.
 */
static void schedule_next_alarm(const ds3231_datetime_t *now, bool now_ok) {
  ds3231_alarm_time_t alarm = {.day = 1, .hour = 0, .min = 0, .sec = 0};
  ds3231_alarm1_mode_t mode = ALARM1_MINUTES_SECONDS_MATCH; // fallback: hourly

  if (now_ok) {
    struct tm next = {.tm_year = now->year + 100, // years since 1900
                      .tm_mon = now->month - 1,
                      .tm_mday = now->day,
                      .tm_hour = now->hour + WAKE_INTERVAL_TOTAL_HOURS,
                      .tm_min = 0,
                      .tm_sec = 0,
                      .tm_isdst = 0};
    mktime(&next); // normalizes hour/day/month overflow

    alarm.day = (uint8_t)next.tm_mday;
    alarm.hour = (uint8_t)next.tm_hour;
    mode = ALARM1_DATE_H_M_S_MATCH;
  }

  // Only Alarm 1 is used. Make sure Alarm 2 can't pull the wake line low
  // (e.g. if it was enabled on this module by older firmware).
  ds3231_disable_alarm(ALARM_2);

  if (ds3231_set_alarm1(&alarm, mode)) {
    printf("Next wake: day %d, %02d:00:00%s\n", alarm.day, alarm.hour,
           now_ok ? "" : " (fallback: next full hour)");
  } else {
    printf("ERROR: Failed to configure Alarm 1.\n");
  }
}

/** Formats the wake interval as "1h", "2d" or "1d 6h". */
static void format_wake_interval(char *out, size_t size) {
#if WAKE_INTERVAL_DAYS > 0 && WAKE_INTERVAL_HOURS > 0
  snprintf(out, size, "%dd %dh", WAKE_INTERVAL_DAYS, WAKE_INTERVAL_HOURS);
#elif WAKE_INTERVAL_DAYS > 0
  snprintf(out, size, "%dd", WAKE_INTERVAL_DAYS);
#else
  snprintf(out, size, "%dh", WAKE_INTERVAL_HOURS);
#endif
}

static void blink(uint32_t ms) {
  gpio_put(PICO_DEFAULT_LED_PIN, 1);
  sleep_ms(ms);
  gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

int main() {
  stdio_init_all();
  multicore_launch_core1(core1_entry);

  static uint8_t canvas[CANVAS_BUFFER_SIZE] = {0};
  epd_t epd = epd_create(EPD_DC_PIN);

  epd_init(&epd);

  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

  powman_timer_start();
  powman_set_debug_power_request_ignored(true);

  on_state = powman_power_state_with_domain_on(
      on_state, POWMAN_POWER_DOMAIN_SWITCHED_CORE);
  on_state = powman_power_state_with_domain_on(on_state,
                                               POWMAN_POWER_DOMAIN_XIP_CACHE);

  humidity_sensor_init(26);

  gpio_init(POMP_PIN);
  gpio_set_dir(POMP_PIN, GPIO_OUT);

#ifdef USE_RELAY_GND_ON
  gpio_pull_up(POMP_PIN);
#else
  gpio_pull_down(POMP_PIN);
#endif

  gpio_init(WAKEUP_GPIO);
  gpio_set_dir(WAKEUP_GPIO, GPIO_IN);
  gpio_pull_up(WAKEUP_GPIO);

  blink(100);

  i2c_init(DS3231_I2C_PORT, I2C_BAUDRATE);

  gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
  gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);

  gpio_pull_up(I2C_SDA_PIN);
  gpio_pull_up(I2C_SCL_PIN);

#if CONFIGURE_RTC
  setup_rtc();
#endif

  ds3231_datetime_t now = {0};
  const bool now_ok = ds3231_read_time(&now);
  schedule_next_alarm(&now, now_ok);

  const uint16_t raw = read_humiduty();
  float humidity = (AIR_VALUE - raw) / (AIR_VALUE - WATER_VALUE) * 100.0f;
  float temperature = read_onboard_temperature(true);
  // Read before the pump starts so the motor load doesn't drag VSYS down.
  float vsys = read_vsys();

  char buffer[32] = {0};

#ifdef DEBUG
  printf("Current RTC time: %02d-%02d-%02d %02d:%02d:%02d\n", now.day,
         now.month, now.year, now.hour, now.min, now.sec);
  while (true) {
    const uint16_t dbg_raw = read_humiduty();
    float dbg_humidity =
        (AIR_VALUE - dbg_raw) / (AIR_VALUE - WATER_VALUE) * 100.0f;

    snprintf(buffer, sizeof(buffer), "%.2f%%", dbg_humidity);

    printf("Raw humidity sensor value: %d\n", dbg_raw);
    printf("Humidity: %s\n", buffer);
    printf("VSYS: %.2f V (%d bars)\n", read_vsys(),
           battery_bars(read_vsys()));

    sleep_ms(1000);
  }
#endif

  // 1. Draw static template background containing icons, cards, and labels
  draw_image(&epd, 0, 0, watering_template_122x250, WATERING_TEMPLATE_WIDTH,
             WATERING_TEMPLATE_HEIGHT, canvas);

  // 2. Battery / Power slot (top right)
  draw_battery(&epd, vsys, canvas);

  // 3. Plant Identity (Hero card)
  draw_text_with_bg(&epd, SLOT_PLANT_NAME_X, SLOT_PLANT_NAME_Y, PLANT_NAME_TEXT,
                    FONT_TAHOMA_12, BLACK, 1, WHITE, canvas);
  draw_text_with_bg(&epd, SLOT_PLANT_SUB_X, SLOT_PLANT_SUB_Y, PLANT_SUB_TEXT,
                    FONT_TAHOMA_8, RED, 1, WHITE, canvas);

  // 4. Soil Moisture card
  int moisture_pct = (int)humidity;
  if (moisture_pct < 0) moisture_pct = 0;
  if (moisture_pct > 100) moisture_pct = 100;

  snprintf(buffer, sizeof(buffer), "%d%%", moisture_pct);
  draw_text_with_bg(&epd, SLOT_MOISTURE_PCT_X, SLOT_MOISTURE_PCT_Y, buffer,
                    FONT_TAHOMA_16, BLACK, 1, WHITE, canvas);

  bool is_dry = (humidity < MIN_HUMIDITY);
  draw_text_with_bg(&epd, SLOT_MOISTURE_STATUS_X, SLOT_MOISTURE_STATUS_Y,
                    is_dry ? STATUS_WATERING_TEXT : STATUS_OPTIMAL_TEXT,
                    FONT_TAHOMA_8, is_dry ? RED : BLACK, 1, WHITE, canvas);

  // Fill progress gauge bar (up to MOISTURE_BAR_MAX_WIDTH)
  uint16_t fill_w = (uint16_t)(MOISTURE_BAR_MAX_WIDTH * moisture_pct / 100);
  if (fill_w > 0) {
    fill_rect(&epd, MOISTURE_BAR_X, MOISTURE_BAR_Y, fill_w, MOISTURE_BAR_HEIGHT,
              is_dry ? RED : YELLOW, canvas);
  }

  // 5. Temperature card
  snprintf(buffer, sizeof(buffer), "%.1f C", temperature);
  draw_text_with_bg(&epd, SLOT_TEMP_VAL_X, SLOT_TEMP_VAL_Y, buffer,
                    FONT_TAHOMA_16, BLACK, 1, WHITE, canvas);
  draw_text_with_bg(&epd, SLOT_TEMP_LABEL_X, SLOT_TEMP_LABEL_Y, TEMP_LABEL_TEXT,
                    FONT_TAHOMA_8, BLACK, 1, WHITE, canvas);

  // 6. Footer (Timestamp & Next Cycle)
  snprintf(buffer, sizeof(buffer), "%02d-%02d %02d:%02d", now.month, now.day,
           now.hour, now.min);
  draw_text_with_bg(&epd, SLOT_FOOTER_TIME_X, SLOT_FOOTER_TIME_Y, buffer,
                    FONT_TAHOMA_8, RED, 1, WHITE, canvas);
  char interval[12];
  format_wake_interval(interval, sizeof(interval));
  snprintf(buffer, sizeof(buffer), FOOTER_CYCLE_FORMAT, interval);
  draw_text_with_bg(&epd, SLOT_FOOTER_CYCLE_X, SLOT_FOOTER_CYCLE_Y, buffer,
                    FONT_TAHOMA_8, BLACK, 1, WHITE, canvas);

  // Send humidity to core 1 to run pump in parallel
  uint32_t humidity_raw;
  memcpy(&humidity_raw, &humidity, sizeof(float));
  multicore_fifo_push_blocking(humidity_raw);

  epd_display(&epd, canvas);
  epd_sleep(&epd);

  if (ds3231_check_alarm_flag(ALARM_1)) {
    printf("<<<<< ALARM 1 TRIGGERED! >>>>>\n");
    ds3231_clear_alarm_flag(ALARM_1);
  }

  // Wait for core 1 pump to finish before powering off
  multicore_fifo_pop_blocking();

  powman_enable_gpio_wakeup(0, WAKEUP_GPIO, false, false);

  int rc = enter_power_off_state();
  hard_assert(rc == PICO_OK);

  // shall never to loop
  while (true) {
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
    sleep_ms(200);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
    sleep_ms(200);
  }

  return 0;
}