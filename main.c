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

#define AIR_VALUE 2600.0f
#define WATER_VALUE 1000.0f

#define MIN_HUMIDITY 40.0f

// Soil sensor sanity checks. A reading is a fault (no watering) when the
// average raw value is below SENSOR_MIN_RAW (shorted/disconnected) or the
// samples spread by more than SENSOR_MAX_SPREAD (floating wire).
#define SENSOR_SAMPLES 16
#define SENSOR_MIN_RAW 300
#define SENSOR_MAX_SPREAD 300

#define TIME_POMP_ON 5000
#define POMP_PIN 22
#define USE_RELAY_GND_ON true // true: relay is active-LOW, false: active-HIGH
#define POMP_ON_LEVEL (!USE_RELAY_GND_ON)
#define POMP_OFF_LEVEL (USE_RELAY_GND_ON)

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

/* =========================================================================
 * Pump guards (decide_pomp)
 * ========================================================================= */
// Don't start the pump on a boot caused by a brown-out. The most likely cause
// is the pump's own start-up current on a weak battery; starting it again
// would reset the board again. The next normal alarm wake tries again.
#define SKIP_POMP_AFTER_BROWNOUT 1
// Don't start the pump when VSYS (measured at rest) is below this. On USB
// VSYS is ~4.7 V, so this only applies on battery. 0 disables the check.
#define MIN_POMP_VSYS_V 3.60f
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

// All text uses one font family (the driver's Tahoma bitmap font) in three
// sizes. Enable each size in CMakeLists.txt (USE_FONT_TAHOMA_*).
#define FONT_SMALL FONT_TAHOMA_8   // labels, status, header, footer
#define FONT_MEDIUM FONT_TAHOMA_12 // plant name
#define FONT_LARGE FONT_TAHOMA_16  // moisture % and temperature values

// Section labels. The template has no text; these are drawn on top of it.
// Capitals start 1 px below y.
#define LABEL_PLANT_TEXT "PLANT"
#define LABEL_PLANT_X 10
#define LABEL_PLANT_Y 28

#define LABEL_MOISTURE_TEXT "SOIL MOISTURE"
#define LABEL_MOISTURE_X 10
#define LABEL_MOISTURE_Y 83

#define LABEL_SCALE_Y 148 // DRY / OK / WET under the moisture bar, centered
#define LABEL_DRY_CX 28 // segment centers of the template bar (12..44..76..109)
#define LABEL_OK_CX 60
#define LABEL_WET_CX 93

#define LABEL_TEMP_TEXT "TEMPERATURE"
#define LABEL_TEMP_X 42 // above the value, right of the thermometer
#define LABEL_TEMP_Y 172

#define LABEL_REFRESH_TEXT "LAST REFRESH"
#define LABEL_REFRESH_X 30
#define LABEL_REFRESH_Y 213

// Header title (top left), smallest font (Tahoma 8, 11 px high).
// The area from SLOT_TITLE_X up to the battery icon is cleared first, which
// also hides the template's red status dot. About 58 px wide: "shatterbind"
// takes 54 px.
#define HEADER_TITLE_TEXT "shatterbind"
#define SLOT_TITLE_X 6
#define SLOT_TITLE_Y 5
#define TITLE_FONT FONT_SMALL
#define TITLE_FONT_HEIGHT 11
#define TITLE_AREA_END_X 63 // last free column before the battery icon

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
#define PLANT_NAME_TEXT "Indian"

#define SLOT_PLANT_SUB_X 12
#define SLOT_PLANT_SUB_Y 54
#define PLANT_SUB_TEXT "Laurel"

#define SLOT_PLANT_ZONE_X 12
#define SLOT_PLANT_ZONE_Y 64

// Soil Moisture Card (template y 81..164)
#define SLOT_MOISTURE_PCT_X 42
#define SLOT_MOISTURE_PCT_Y 99

// Raw ADC value, small, right-aligned in the free column right of the % value
// ("100%" ends at x 90, card border at x 117; "4095" is 23 px wide).
#define MOISTURE_RAW_LABEL_TEXT "raw"
#define SLOT_MOISTURE_RAW_RIGHT_X 115 // last pixel column of the text
#define SLOT_MOISTURE_RAW_LABEL_Y 98
#define SLOT_MOISTURE_RAW_Y 107 // number: capitals 108..115, next to the %

#define SLOT_MOISTURE_STATUS_X 42
#define SLOT_MOISTURE_STATUS_Y 121
// The status shows what this wake did, so it stays true until the next wake.
#define STATUS_WATERED_FORMAT "WATERED %us" // %u = pump run time in seconds
#define STATUS_OPTIMAL_TEXT "OPTIMAL"
#define STATUS_SENSOR_ERROR_TEXT "SENSOR ERR"
#define STATUS_SKIP_BROWNOUT_TEXT "SKIP: BOR"      // dry, but booted from BOR
#define STATUS_SKIP_LOW_BATTERY_TEXT "LOW BATTERY" // dry, but VSYS too low
#define STATUS_MAX_WIDTH 73 // px from SLOT_MOISTURE_STATUS_X to the card edge

// Moisture Progress Bar
#define MOISTURE_BAR_X 13
#define MOISTURE_BAR_Y 138 // inside of the template bar (137..144)
#define MOISTURE_BAR_MAX_WIDTH 96
#define MOISTURE_BAR_HEIGHT 6

// Temperature Card (template y 170..201, thermometer icon y 174..196).
// Label and value form one column next to the icon.
#define SLOT_TEMP_VAL_X 42 // same column as the moisture value
#define SLOT_TEMP_VAL_Y 182

// Footer Card
#define SLOT_FOOTER_TIME_X 30
#define SLOT_FOOTER_TIME_Y 224

#define SLOT_FOOTER_CYCLE_X 30
#define SLOT_FOOTER_CYCLE_Y 235
#define FOOTER_CYCLE_FORMAT "Cycle: %s" // %s = e.g. "1h", "2d", "1d 6h"

// Reset reason code (e.g. "BOR"), red, right-aligned on the footer time line.
// Empty on a normal RTC alarm wake. The time ends at x 84 at most.
#define SLOT_FOOTER_RESET_RIGHT_X 115
#define SLOT_FOOTER_RESET_Y SLOT_FOOTER_TIME_Y

/* =========================================================================
 * Reset reasons (read_reset_reason)
 * RESET_CODE_* is shown on the display (max 3 letters / ~22 px, "" = show
 * nothing), RESET_TEXT_* is printed over serial on every boot. Checked in
 * this order; the first match wins.
 * ========================================================================= */
// Normal wake: the DS3231 alarm pulled GP15 low (powman GPIO wake source 0)
#define RESET_CODE_ALARM ""
#define RESET_TEXT_ALARM "RTC alarm wake (GP15)"
// Another powman wake source woke the chip (GPIO wake source 1..3 or timer)
#define RESET_CODE_OTHER_WAKE "WK"
#define RESET_TEXT_OTHER_WAKE "other powman wake source"
// Brown-out: the core supply dropped below the brown-out threshold, e.g. the
// pump's start-up current on a weak battery
#define RESET_CODE_BROWNOUT "BOR"
#define RESET_TEXT_BROWNOUT "brown-out: supply dropped too low"
// Power-on: power was connected after being fully off
#define RESET_CODE_POWER_ON "POR"
#define RESET_TEXT_POWER_ON "power-on"
// RUN pin pulled low (reset button)
#define RESET_CODE_RUN_PIN "RST"
#define RESET_TEXT_RUN_PIN "RUN pin low (reset button)"
// Watchdog; the bootrom also reboots this way after a .uf2 is flashed
#define RESET_CODE_WATCHDOG "WDT"
#define RESET_TEXT_WATCHDOG "watchdog (also after flashing a .uf2)"
// Glitch detector (supply/clock glitch, security feature)
#define RESET_CODE_GLITCH "GLT"
#define RESET_TEXT_GLITCH "glitch detector"
// Debugger, rescue or software reset request
#define RESET_CODE_DEBUG "DBG"
#define RESET_TEXT_DEBUG "debugger / software reset request"
// None of the flags above was set
#define RESET_CODE_UNKNOWN "?"
#define RESET_TEXT_UNKNOWN "unknown"

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
  if (vsys >= BATTERY_3_BARS_V)
    return 3; // also USB power
  if (vsys >= BATTERY_2_BARS_V)
    return 2;
  if (vsys >= BATTERY_1_BAR_V)
    return 1;
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

  draw_text_with_bg(epd, SLOT_BATTERY_X, SLOT_BATTERY_Y, text, FONT_SMALL,
                    bars <= 1 ? RED : BLACK, 1, WHITE, canvas);
}

/** Draws text horizontally centered on cx (no background fill). */
static void draw_text_centered(epd_t *epd, int cx, int y, const char *text,
                               text_type_t font, color_t color,
                               uint8_t *canvas) {
  const int w = canvas_get_text_width(text, font, 1);
  draw_text(epd, (uint16_t)(cx - w / 2), (uint16_t)y, text, font, color, 1,
            canvas);
}

/** Draws text so that its last pixel column is right_x (no background). */
static void draw_text_right(epd_t *epd, int right_x, int y, const char *text,
                            text_type_t font, color_t color, uint8_t *canvas) {
  const int w = canvas_get_text_width(text, font, 1);
  draw_text(epd, (uint16_t)(right_x - w + 1), (uint16_t)y, text, font, color, 1,
            canvas);
}

/**
 * Draws e.g. "21.1°C" in FONT_LARGE. The font has no degree glyph, so the
 * degree sign is drawn as a small ring at the top of the digits.
 */
static void draw_temperature(epd_t *epd, int x, int y, float temp_c,
                             uint8_t *canvas) {
  char value[12];
  snprintf(value, sizeof(value), "%.1f", temp_c);
  draw_text(epd, (uint16_t)x, (uint16_t)y, value, FONT_LARGE, BLACK, 1, canvas);

  // 5x5 ring, 1 px gap after the digits, aligned with the top of the capitals
  const int rx = x + canvas_get_text_width(value, FONT_LARGE, 1) + 2;
  const int ry = y + 1;
  fill_rect(epd, rx + 1, ry, 3, 1, BLACK, canvas);
  fill_rect(epd, rx + 1, ry + 4, 3, 1, BLACK, canvas);
  fill_rect(epd, rx, ry + 1, 1, 3, BLACK, canvas);
  fill_rect(epd, rx + 4, ry + 1, 1, 3, BLACK, canvas);

  draw_text(epd, (uint16_t)(rx + 6), (uint16_t)y, "C", FONT_LARGE, BLACK, 1,
            canvas);
}

/** Runs the pump for run_ms milliseconds (0 = don't run), then switches off. */
static void pomp(uint32_t run_ms) {
  if (run_ms > 0) {
    gpio_put(POMP_PIN, POMP_ON_LEVEL);
    sleep_ms(run_ms);
  }

  gpio_put(POMP_PIN, POMP_OFF_LEVEL);
}

/** Core 1: receives the pump run time in ms from core 0, runs the pump. */
static void core1_entry(void) {
  const uint32_t run_ms = multicore_fifo_pop_blocking();
  pomp(run_ms);
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

typedef struct {
  uint16_t raw;    // averaged ADC value
  uint16_t spread; // max - min of the samples
  float humidity;  // %, not clamped
  bool ok;         // false = sensor fault, don't water
} soil_reading_t;

static soil_reading_t read_soil(void) {
  adc_select_input(HUMIDITY_ADC_PIN);

  uint32_t sum = 0;
  uint16_t min = UINT16_MAX;
  uint16_t max = 0;

  for (int i = 0; i < SENSOR_SAMPLES; i++) {
    const uint16_t v = adc_read();
    sum += v;
    if (v < min)
      min = v;
    if (v > max)
      max = v;
    sleep_us(100);
  }

  soil_reading_t r;
  r.raw = (uint16_t)(sum / SENSOR_SAMPLES);
  r.spread = max - min;
  r.humidity = (AIR_VALUE - r.raw) / (AIR_VALUE - WATER_VALUE) * 100.0f;
  r.ok = r.raw >= SENSOR_MIN_RAW && r.spread <= SENSOR_MAX_SPREAD;
  return r;
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
  if (!ds3231_oscillator_stopped(&osc_stopped) || !ds3231_read_time(&current)) {
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

typedef struct {
  const char *code;    // short code for the display, "" = normal alarm wake
  const char *text;    // description for the serial log
  uint32_t chip_reset; // raw POWMAN_CHIP_RESET flags
  uint32_t pwrup;      // raw POWMAN_LAST_SWCORE_PWRUP (0 = chip reset)
  bool brownout;       // this boot was caused by a brown-out reset
} reset_reason_t;

/**
 * Why this boot happened. LAST_SWCORE_PWRUP says what powered up the core:
 * 1..4 = powman GPIO wake source 0..3 (source 0 = DS3231 alarm on GP15),
 * 6 = powman timer, 0 = a full chip reset, whose cause is in CHIP_RESET.
 * Only the latest reset is recorded; a brown-out also clears powman scratch.
 */
static reset_reason_t read_reset_reason(void) {
  reset_reason_t r = {.chip_reset = powman_hw->chip_reset,
                      .pwrup = powman_hw->last_swcore_pwrup &
                               POWMAN_LAST_SWCORE_PWRUP_BITS};
  const uint32_t f = r.chip_reset;

  if (r.pwrup == 1) {
    r.code = RESET_CODE_ALARM;
    r.text = RESET_TEXT_ALARM;
  } else if (r.pwrup >= 2 && r.pwrup <= 6) {
    r.code = RESET_CODE_OTHER_WAKE;
    r.text = RESET_TEXT_OTHER_WAKE;
  } else if (f & POWMAN_CHIP_RESET_HAD_BOR_BITS) {
    r.code = RESET_CODE_BROWNOUT;
    r.text = RESET_TEXT_BROWNOUT;
    r.brownout = true;
  } else if (f & POWMAN_CHIP_RESET_HAD_POR_BITS) {
    r.code = RESET_CODE_POWER_ON;
    r.text = RESET_TEXT_POWER_ON;
  } else if (f & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS) {
    r.code = RESET_CODE_RUN_PIN;
    r.text = RESET_TEXT_RUN_PIN;
  } else if (f & (POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_RSM_BITS |
                  POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS |
                  POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS |
                  POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS)) {
    r.code = RESET_CODE_WATCHDOG;
    r.text = RESET_TEXT_WATCHDOG;
  } else if (f & POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS) {
    r.code = RESET_CODE_GLITCH;
    r.text = RESET_TEXT_GLITCH;
  } else if (f & (POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS |
                  POWMAN_CHIP_RESET_HAD_RESCUE_BITS |
                  POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS)) {
    r.code = RESET_CODE_DEBUG;
    r.text = RESET_TEXT_DEBUG;
  } else {
    r.code = RESET_CODE_UNKNOWN;
    r.text = RESET_TEXT_UNKNOWN;
  }
  return r;
}

typedef enum {
  POMP_NOT_NEEDED,       // soil moist enough
  POMP_RUN,              // dry: run the pump for TIME_POMP_ON
  POMP_SENSOR_FAULT,     // invalid reading
  POMP_SKIP_BROWNOUT,    // dry, but this boot came from a brown-out
  POMP_SKIP_LOW_BATTERY, // dry, but VSYS below MIN_POMP_VSYS_V
} pomp_decision_t;

/**
 * The single watering decision of this wake (core 0). The guards only apply
 * when the soil is dry: a moist reading always shows OPTIMAL.
 */
static pomp_decision_t decide_pomp(const soil_reading_t *soil, float vsys,
                                   const reset_reason_t *reset) {
  if (!soil->ok)
    return POMP_SENSOR_FAULT;
  if (soil->humidity >= MIN_HUMIDITY)
    return POMP_NOT_NEEDED;
#if SKIP_POMP_AFTER_BROWNOUT
  if (reset->brownout)
    return POMP_SKIP_BROWNOUT;
#endif
  if (MIN_POMP_VSYS_V > 0 && vsys < MIN_POMP_VSYS_V)
    return POMP_SKIP_LOW_BATTERY;
  return POMP_RUN;
}

static const char *pomp_decision_name(pomp_decision_t d) {
  switch (d) {
  case POMP_RUN:
    return "watering";
  case POMP_SENSOR_FAULT:
    return "SENSOR FAULT, no watering";
  case POMP_SKIP_BROWNOUT:
    return "dry, skipped: booted from brown-out";
  case POMP_SKIP_LOW_BATTERY:
    return "dry, skipped: battery too low";
  default:
    return "no watering needed";
  }
}

static void blink(uint32_t ms) {
  gpio_put(PICO_DEFAULT_LED_PIN, 1);
  sleep_ms(ms);
  gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

int main() {
  // Pump pin first, before anything that can take time: latch the "off"
  // level before switching the pin to output. gpio_set_dir() alone would
  // drive it LOW, which turns an active-LOW relay (and the pump) on.
  gpio_init(POMP_PIN);
  gpio_put(POMP_PIN, POMP_OFF_LEVEL);
  gpio_set_dir(POMP_PIN, GPIO_OUT);
#if USE_RELAY_GND_ON
  gpio_pull_up(POMP_PIN);
#else
  gpio_pull_down(POMP_PIN);
#endif

  stdio_init_all();
  multicore_launch_core1(core1_entry);

  const reset_reason_t reset = read_reset_reason();
  printf("Boot: %s%s%s (CHIP_RESET=0x%08lx, LAST_SWCORE_PWRUP=%lu)\n",
         reset.code, reset.code[0] ? " - " : "", reset.text,
         (unsigned long)reset.chip_reset, (unsigned long)reset.pwrup);

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

  const soil_reading_t soil = read_soil();
  float temperature = read_onboard_temperature(true);
  // Read before the pump starts so the motor load doesn't drag VSYS down.
  float vsys = read_vsys();

  // Decided once, on core 0; the display shows exactly what was done.
  const pomp_decision_t decision = decide_pomp(&soil, vsys, &reset);
  const bool water = decision == POMP_RUN;
  const uint32_t pomp_run_ms = water ? TIME_POMP_ON : 0;
  printf("Soil raw %u (spread %u) = %.1f%%, VSYS %.2f V -> %s\n", soil.raw,
         soil.spread, soil.humidity, vsys, pomp_decision_name(decision));

  char buffer[32] = {0};

#ifdef DEBUG
  printf("Current RTC time: %02d-%02d-%02d %02d:%02d:%02d\n", now.day,
         now.month, now.year, now.hour, now.min, now.sec);
  while (true) {
    const soil_reading_t dbg = read_soil();

    printf("Raw humidity sensor value: %u (spread %u, %s)\n", dbg.raw,
           dbg.spread, dbg.ok ? "ok" : "SENSOR FAULT");
    printf("Humidity: %.2f%%\n", dbg.humidity);
    printf("VSYS: %.2f V (%d bars)\n", read_vsys(), battery_bars(read_vsys()));

    sleep_ms(1000);
  }
#endif

  // 1. Draw static template background (cards and icons), then the section
  //    labels in the same font as the values. No background fill, so the
  //    card borders under the text cells stay intact.
  draw_image(&epd, 0, 0, watering_template_122x250, WATERING_TEMPLATE_WIDTH,
             WATERING_TEMPLATE_HEIGHT, canvas);
  draw_text(&epd, LABEL_PLANT_X, LABEL_PLANT_Y, LABEL_PLANT_TEXT, FONT_SMALL,
            BLACK, 1, canvas);
  draw_text(&epd, LABEL_MOISTURE_X, LABEL_MOISTURE_Y, LABEL_MOISTURE_TEXT,
            FONT_SMALL, BLACK, 1, canvas);
  draw_text_centered(&epd, LABEL_DRY_CX, LABEL_SCALE_Y, "DRY", FONT_SMALL,
                     BLACK, canvas);
  draw_text_centered(&epd, LABEL_OK_CX, LABEL_SCALE_Y, "OK", FONT_SMALL, BLACK,
                     canvas);
  draw_text_centered(&epd, LABEL_WET_CX, LABEL_SCALE_Y, "WET", FONT_SMALL,
                     BLACK, canvas);
  draw_text(&epd, LABEL_TEMP_X, LABEL_TEMP_Y, LABEL_TEMP_TEXT, FONT_SMALL,
            BLACK, 1, canvas);
  draw_text(&epd, LABEL_REFRESH_X, LABEL_REFRESH_Y, LABEL_REFRESH_TEXT,
            FONT_SMALL, BLACK, 1, canvas);

  // 2. Header title (top left) and battery / power slot (top right)
  fill_rect(&epd, SLOT_TITLE_X - 1, SLOT_TITLE_Y,
            TITLE_AREA_END_X - SLOT_TITLE_X + 2, TITLE_FONT_HEIGHT, WHITE,
            canvas);
  if (canvas_get_text_width(HEADER_TITLE_TEXT, TITLE_FONT, 1) >
      TITLE_AREA_END_X - SLOT_TITLE_X + 1) {
    printf(
        "WARNING: HEADER_TITLE_TEXT is too wide, it overlaps the battery.\n");
  }
  draw_text_with_bg(&epd, SLOT_TITLE_X, SLOT_TITLE_Y, HEADER_TITLE_TEXT,
                    TITLE_FONT, BLACK, 1, WHITE, canvas);

  draw_battery(&epd, vsys, canvas);

  // Values below are drawn without a background fill: the template is white
  // there, and a filled text cell would cut into text or lines next to it.

  // 3. Plant Identity (Hero card)
  draw_text(&epd, SLOT_PLANT_NAME_X, SLOT_PLANT_NAME_Y, PLANT_NAME_TEXT,
            FONT_MEDIUM, BLACK, 1, canvas);
  draw_text(&epd, SLOT_PLANT_SUB_X, SLOT_PLANT_SUB_Y, PLANT_SUB_TEXT,
            FONT_SMALL, RED, 1, canvas);

  // 4. Soil Moisture card
  // Moisture % and bar are the reading taken before the pump ran.
  int moisture_pct = (int)soil.humidity;
  if (moisture_pct < 0)
    moisture_pct = 0;
  if (moisture_pct > 100)
    moisture_pct = 100;
  const bool dry = soil.humidity < MIN_HUMIDITY;

  if (soil.ok) {
    snprintf(buffer, sizeof(buffer), "%d%%", moisture_pct);
  } else {
    snprintf(buffer, sizeof(buffer), "--%%");
  }
  draw_text(&epd, SLOT_MOISTURE_PCT_X, SLOT_MOISTURE_PCT_Y, buffer, FONT_LARGE,
            BLACK, 1, canvas);

  // Raw ADC value (also on a sensor fault, to help diagnose it)
  draw_text_right(&epd, SLOT_MOISTURE_RAW_RIGHT_X, SLOT_MOISTURE_RAW_LABEL_Y,
                  MOISTURE_RAW_LABEL_TEXT, FONT_SMALL, BLACK, canvas);
  snprintf(buffer, sizeof(buffer), "%u", soil.raw);
  draw_text_right(&epd, SLOT_MOISTURE_RAW_RIGHT_X, SLOT_MOISTURE_RAW_Y, buffer,
                  FONT_SMALL, BLACK, canvas);

  switch (decision) {
  case POMP_SENSOR_FAULT:
    snprintf(buffer, sizeof(buffer), STATUS_SENSOR_ERROR_TEXT);
    break;
  case POMP_RUN:
    snprintf(buffer, sizeof(buffer), STATUS_WATERED_FORMAT,
             (unsigned)((pomp_run_ms + 500) / 1000));
    break;
  case POMP_SKIP_BROWNOUT:
    snprintf(buffer, sizeof(buffer), STATUS_SKIP_BROWNOUT_TEXT);
    break;
  case POMP_SKIP_LOW_BATTERY:
    snprintf(buffer, sizeof(buffer), STATUS_SKIP_LOW_BATTERY_TEXT);
    break;
  default:
    snprintf(buffer, sizeof(buffer), STATUS_OPTIMAL_TEXT);
    break;
  }
  if (canvas_get_text_width(buffer, FONT_SMALL, 1) > STATUS_MAX_WIDTH) {
    printf("WARNING: status \"%s\" is wider than the card.\n", buffer);
  }
  draw_text(&epd, SLOT_MOISTURE_STATUS_X, SLOT_MOISTURE_STATUS_Y, buffer,
            FONT_SMALL, decision == POMP_NOT_NEEDED ? BLACK : RED, 1, canvas);

  // Fill progress gauge bar (up to MOISTURE_BAR_MAX_WIDTH)
  uint16_t fill_w = (uint16_t)(MOISTURE_BAR_MAX_WIDTH * moisture_pct / 100);
  if (soil.ok && fill_w > 0) {
    fill_rect(&epd, MOISTURE_BAR_X, MOISTURE_BAR_Y, fill_w, MOISTURE_BAR_HEIGHT,
              dry ? RED : YELLOW, canvas);
  }

  // 5. Temperature card
  draw_temperature(&epd, SLOT_TEMP_VAL_X, SLOT_TEMP_VAL_Y, temperature, canvas);

  // 6. Footer (Timestamp & Next Cycle)
  snprintf(buffer, sizeof(buffer), "%02d-%02d %02d:%02d", now.month, now.day,
           now.hour, now.min);
  draw_text(&epd, SLOT_FOOTER_TIME_X, SLOT_FOOTER_TIME_Y, buffer, FONT_SMALL,
            RED, 1, canvas);
  // Why this boot happened, if it wasn't the normal RTC alarm wake
  if (reset.code[0]) {
    draw_text_right(&epd, SLOT_FOOTER_RESET_RIGHT_X, SLOT_FOOTER_RESET_Y,
                    reset.code, FONT_SMALL, RED, canvas);
  }
  char interval[12];
  format_wake_interval(interval, sizeof(interval));
  snprintf(buffer, sizeof(buffer), FOOTER_CYCLE_FORMAT, interval);
  draw_text(&epd, SLOT_FOOTER_CYCLE_X, SLOT_FOOTER_CYCLE_Y, buffer, FONT_SMALL,
            BLACK, 1, canvas);

  // Core 1 runs the pump (0 ms = stays off) while the display refreshes.
  multicore_fifo_push_blocking(pomp_run_ms);

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