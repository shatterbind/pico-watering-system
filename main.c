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

// ---------------------------------------------------------------------------
// Soil moisture sensor
// ---------------------------------------------------------------------------
#define HUMIDITY_GPIO 26     // GP26
#define HUMIDITY_ADC_INPUT 0 // ADC0 == GP26

/**
 * @brief GPIO that powers the sensor's VCC, or -1 if VCC is wired to 3V3.
 * Powering the sensor from a GPIO lets it be switched off between wakes
 * (a capacitive sensor draws ~5 mA, which dominates sleep current).
 */
#define SENSOR_POWER_PIN -1
#define SENSOR_WARMUP_MS 200

#define AIR_VALUE 4095.0f   // raw ADC in dry air
#define WATER_VALUE 1100.0f // raw ADC in water

#define ADC_SAMPLES 16
/** Max spread (max - min) of the samples; more means a floating/broken wire. */
#define SENSOR_MAX_SPREAD 300
/** Raw values below this are "wetter than water": shorted or disconnected. */
#define SENSOR_MIN_RAW 300

// ---------------------------------------------------------------------------
// Pump
// ---------------------------------------------------------------------------
#define MIN_HUMIDITY 40.0f

#define TIME_POMP_ON 5000
/** Never run the pump twice within this many minutes (guards wake loops). */
#define MIN_POMP_INTERVAL_MIN 50

#define POMP_PIN 22
#define USE_RELAY_GND_ON true // true: relay is active-LOW

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------
#define TEMP_SENSOR_CHANNEL 4
#define WAKEUP_GPIO 15
#define EPD_DC_PIN 8

#define I2C_BAUDRATE (100 * 1000)
#define I2C_SDA_PIN 20 // GP20 is I2C0 SDA (fixed by hardware)
#define I2C_SCL_PIN 21 // GP21 is I2C0 SCL

/**
 * POWMAN scratch registers keep their value across the power-off state (but
 * not across a loss of power), so they remember the last pump run.
 */
#define SCRATCH_MAGIC_IDX 6
#define SCRATCH_LAST_POMP_IDX 7
#define SCRATCH_MAGIC 0x504F4D50u // "POMP"

/**
 * @brief When 1, the DS3231 is set from the firmware compile time if its
 * oscillator-stop flag is set (fresh module / dead backup battery) or it is
 * behind the compile time. Waking from the power-off state is a full reboot,
 * so unconditionally setting the time here would roll the clock back on
 * every wake.
 */
#define CONFIGURE_RTC 1
#define CONFIGURE_ALARM_1 1
#define CONFIGURE_ALARM_2 0

// #define DEBUG

/**
 * @brief Delay in seconds to compensate for compilation and upload time.
 */
#define COMPILE_UPLOAD_OFFSET_SECONDS 6

static powman_power_state off_state = POWMAN_POWER_STATE_NONE;
static powman_power_state on_state = POWMAN_POWER_STATE_NONE;

typedef struct {
  uint16_t raw;
  bool ok;
} soil_reading_t;

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

static void pomp_pin_init(void) {
  // Latch the "pump off" level before enabling the output driver so an
  // active-low relay does not click on at boot.
  gpio_init(POMP_PIN);
  gpio_put(POMP_PIN, USE_RELAY_GND_ON);
  gpio_set_dir(POMP_PIN, GPIO_OUT);

#if USE_RELAY_GND_ON
  gpio_pull_up(POMP_PIN);
#else
  gpio_pull_down(POMP_PIN);
#endif
}

static void humidity_sensor_init(uint gpio) {
  adc_init();
  adc_set_temp_sensor_enabled(true);
  adc_gpio_init(gpio);

#if SENSOR_POWER_PIN >= 0
  gpio_init(SENSOR_POWER_PIN);
  gpio_put(SENSOR_POWER_PIN, 0);
  gpio_set_dir(SENSOR_POWER_PIN, GPIO_OUT);
#endif
}

/** Core 1: receives the pump run time in ms (0 = don't run), then acks. */
static void core1_entry(void) {
  uint32_t run_ms = multicore_fifo_pop_blocking();

  if (run_ms > 0) {
    gpio_put(POMP_PIN, !USE_RELAY_GND_ON);
    sleep_ms(run_ms);
  }

  gpio_put(POMP_PIN, USE_RELAY_GND_ON);
  multicore_fifo_push_blocking(1); // signal done
}

static float read_onboard_temperature(bool toCelsius) {
  float adc;
  float tempC;
  const float conversionFactor = 3.3f / (1 << 12);

  adc_select_input(TEMP_SENSOR_CHANNEL);

  adc = (float)adc_read() * conversionFactor;
  tempC = 27.0f - (adc - 0.706f) / 0.001721f;

  return toCelsius ? tempC : tempC * 9 / 5 + 32;
}

/**
 * Averages ADC_SAMPLES readings. A disconnected (floating) input is noisy and
 * a shorted one reads near 0; both are reported as not ok so the pump never
 * runs on a bogus "dry" value.
 */
static soil_reading_t read_soil(void) {
#if SENSOR_POWER_PIN >= 0
  gpio_put(SENSOR_POWER_PIN, 1);
  sleep_ms(SENSOR_WARMUP_MS);
#endif

  adc_select_input(HUMIDITY_ADC_INPUT);

  uint32_t sum = 0;
  uint16_t min = UINT16_MAX;
  uint16_t max = 0;

  for (int i = 0; i < ADC_SAMPLES; i++) {
    uint16_t v = adc_read();
    sum += v;
    if (v < min) {
      min = v;
    }
    if (v > max) {
      max = v;
    }
    sleep_us(200);
  }

#if SENSOR_POWER_PIN >= 0
  gpio_put(SENSOR_POWER_PIN, 0);
#endif

  soil_reading_t r = {.raw = (uint16_t)(sum / ADC_SAMPLES)};
  r.ok = (max - min) <= SENSOR_MAX_SPREAD && r.raw >= SENSOR_MIN_RAW;
  return r;
}

static float raw_to_humidity(uint16_t raw) {
  float humidity = (AIR_VALUE - raw) / (AIR_VALUE - WATER_VALUE) * 100.0f;

  if (humidity < 0.0f) {
    return 0.0f;
  }
  if (humidity > 100.0f) {
    return 100.0f;
  }
  return humidity;
}

/** Packs a datetime into a monotonically comparable integer. */
static uint64_t datetime_key(const ds3231_datetime_t *dt) {
  return ((uint64_t)dt->year << 40) | ((uint64_t)dt->month << 32) |
         ((uint64_t)dt->day << 24) | ((uint64_t)dt->hour << 16) |
         ((uint64_t)dt->min << 8) | dt->sec;
}

/** Minutes since the epoch, for interval checks. */
static bool datetime_to_minutes(const ds3231_datetime_t *dt, uint32_t *out) {
  struct tm t = {.tm_year = dt->year + 100, // years since 1900
                 .tm_mon = dt->month - 1,
                 .tm_mday = dt->day,
                 .tm_hour = dt->hour,
                 .tm_min = dt->min,
                 .tm_sec = 0};
  time_t s = mktime(&t);

  if (s == (time_t)-1) {
    return false;
  }
  *out = (uint32_t)(s / 60);
  return true;
}

#if CONFIGURE_RTC
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
  bool osf_ok = ds3231_oscillator_stopped(&osc_stopped);

  ds3231_datetime_t current;
  bool rtc_ok = ds3231_read_time(&current);

  if (!osf_ok || !rtc_ok || osc_stopped ||
      datetime_key(&current) < datetime_key(&dt_to_set)) {
    printf("RTC time invalid or behind compile time, setting it...\n");
    if (ds3231_set_time(&dt_to_set) && ds3231_clear_oscillator_stop_flag()) {
      printf("Time set successfully!\n");
    } else {
      printf("ERROR: Failed to set time.\n");
    }
  }

#if CONFIGURE_ALARM_1

  ds3231_alarm_time_t alarm_1 = {.day = 0, .hour = 0, .min = 0, .sec = 0};

  printf(
      "Setting Alarm 1 to trigger when day=%d hour=%d minute=%d second=%d.\n",
      alarm_1.day, alarm_1.hour, alarm_1.min, alarm_1.sec);

  if (ds3231_set_alarm1(&alarm_1, ALARM1_MINUTES_SECONDS_MATCH)) {
    printf("Alarm 1 configured successfully.\n");
  } else {
    printf("ERROR: Failed to configure Alarm 1.\n");
  }

#endif

#if CONFIGURE_ALARM_2

  ds3231_alarm_time_t alarm_2 = {.day = 0, .hour = 0, .min = 18};

  printf("Setting Alarm 2 to trigger when day=%d hour=%d minute=%d.\n",
         alarm_2.day, alarm_2.hour, alarm_2.min);

  if (ds3231_set_alarm2(&alarm_2, ALARM2_MINUTES_MATCH)) {
    printf("Alarm 2 configured successfully.\n");
  } else {
    printf("ERROR: Failed to configure Alarm 2.\n");
  }

#endif

  printf("RTC configure for interrupt mode.\n");
  if (!ds3231_enable_interrupt_mode()) {
    printf("ERROR: Failed to enable RTC interrupt mode.\n");
  }
}
#endif

/**
 * Clears both alarm flags so INT/SQW is released (it holds the wake line low
 * until then) and verifies they really cleared. If this fails the board would
 * re-wake immediately after powering off.
 */
static bool clear_alarm_flags(void) {
  bool ok = true;
  const ds3231_alarm_num_t alarms[] = {ALARM_1, ALARM_2};

  for (size_t i = 0; i < 2; i++) {
    if (ds3231_check_alarm_flag(alarms[i])) {
      printf("<<<<< ALARM %d TRIGGERED! >>>>>\n", (int)i + 1);
      if (!ds3231_clear_alarm_flag(alarms[i]) ||
          ds3231_check_alarm_flag(alarms[i])) {
        printf("ERROR: Failed to clear alarm %d flag.\n", (int)i + 1);
        ok = false;
      }
    }
  }

  return ok;
}

/** True if the pump ran less than MIN_POMP_INTERVAL_MIN minutes ago. */
static bool pomp_ran_recently(uint32_t now_min) {
  if (powman_hw->scratch[SCRATCH_MAGIC_IDX] != SCRATCH_MAGIC) {
    return false;
  }

  uint32_t last_min = powman_hw->scratch[SCRATCH_LAST_POMP_IDX];

  // If the clock went backwards, don't trust the stored value.
  return now_min >= last_min && (now_min - last_min) < MIN_POMP_INTERVAL_MIN;
}

static void remember_pomp_run(uint32_t now_min) {
  powman_hw->scratch[SCRATCH_LAST_POMP_IDX] = now_min;
  powman_hw->scratch[SCRATCH_MAGIC_IDX] = SCRATCH_MAGIC;
}

static void blink(uint32_t ms) {
  gpio_put(PICO_DEFAULT_LED_PIN, 1);
  sleep_ms(ms);
  gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

int main() {
  // Pump pin first: it must be in the "off" state as early as possible.
  pomp_pin_init();

  stdio_init_all();
  multicore_launch_core1(core1_entry);

  static uint8_t canvas[EPD_2_13_CANVAS_SIZE] = {0};
  epd_t epd = epd_create(EPD_DC_PIN);
  hard_assert(epd_canvas_size(&epd) <= sizeof(canvas));

  if (!epd_init(&epd)) {
    printf("ERROR: EPD init timed out (BUSY stuck).\n");
  }

  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

  powman_timer_start();
  powman_set_debug_power_request_ignored(true);

  on_state = powman_power_state_with_domain_on(
      on_state, POWMAN_POWER_DOMAIN_SWITCHED_CORE);
  on_state = powman_power_state_with_domain_on(on_state,
                                               POWMAN_POWER_DOMAIN_XIP_CACHE);

  humidity_sensor_init(HUMIDITY_GPIO);

  gpio_init(WAKEUP_GPIO);
  gpio_set_dir(WAKEUP_GPIO, GPIO_IN);
  gpio_pull_up(WAKEUP_GPIO);

  blink(100);

  fill_background(&epd, WHITE, canvas);

  i2c_init(DS3231_I2C_PORT, I2C_BAUDRATE);

  gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
  gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);

  gpio_pull_up(I2C_SDA_PIN);
  gpio_pull_up(I2C_SCL_PIN);

#if CONFIGURE_RTC
  setup_rtc();
#endif

  // Release the wake line early; the pump is only allowed if this worked.
  bool alarms_ok = clear_alarm_flags();

  ds3231_datetime_t now;
  char buffer[32] = {0};
  bool rtc_ok = ds3231_read_time(&now);

  draw_text(&epd, 0, 0, "Last update:", FONT_TAHOMA_8, BLACK, 3, canvas);

  if (rtc_ok) {
    snprintf(buffer, sizeof(buffer), "%02d-%02d-%02d", now.day, now.month,
             now.year);
    draw_text(&epd, 0, 15, buffer, FONT_TAHOMA_16, BLACK, 3, canvas);

    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", now.hour, now.min,
             now.sec);
    draw_text(&epd, 0, 40, buffer, FONT_TAHOMA_16, BLACK, 3, canvas);
  } else {
    printf("ERROR: Failed to read RTC time.\n");
    draw_text(&epd, 0, 15, "RTC ERROR", FONT_TAHOMA_16, BLACK, 3, canvas);
  }

#ifdef DEBUG
  printf("Current RTC time: %02d-%02d-%02d %02d:%02d:%02d\n", now.day,
         now.month, now.year, now.hour, now.min, now.sec);
  while (true) {
    soil_reading_t soil = read_soil();
    float humidity = raw_to_humidity(soil.raw);

    printf("Raw humidity sensor value: %d (%s)\n", soil.raw,
           soil.ok ? "ok" : "SENSOR FAULT");
    printf("Humidity: %.2f%%\n", humidity);

    sleep_ms(1000);
  }
#endif

  soil_reading_t soil = read_soil();
  float humidity = raw_to_humidity(soil.raw);

  if (soil.ok) {
    snprintf(buffer, sizeof(buffer), "%.2f%%", humidity);
    draw_text(&epd, 10, 120, buffer, FONT_TAHOMA_24, BLACK, 3, canvas);
  } else {
    printf("ERROR: Soil sensor fault (raw=%d).\n", soil.raw);
    draw_text(&epd, 0, 120, "SENSOR", FONT_TAHOMA_16, BLACK, 3, canvas);
    draw_text(&epd, 0, 142, "ERROR", FONT_TAHOMA_16, BLACK, 3, canvas);
  }

  // Decide whether to water. Every failure mode fails safe: no pumping.
  uint32_t now_min = 0;
  bool time_ok = rtc_ok && datetime_to_minutes(&now, &now_min);
  uint32_t run_ms = 0;
  const char *pomp_status = "Pump: off";

  if (soil.ok && humidity < MIN_HUMIDITY) {
    if (!alarms_ok || !time_ok) {
      pomp_status = "Pump: RTC err";
    } else if (pomp_ran_recently(now_min)) {
      pomp_status = "Pump: wait";
    } else {
      run_ms = TIME_POMP_ON;
      remember_pomp_run(now_min);
      snprintf(buffer, sizeof(buffer), "Pump: %ds", TIME_POMP_ON / 1000);
      pomp_status = buffer;
    }
  }

  printf("%s\n", pomp_status);
  draw_text(&epd, 0, 180, pomp_status, FONT_TAHOMA_8, BLACK, 3, canvas);

  // Run the pump on core 1 while core 0 refreshes the display.
  multicore_fifo_push_blocking(run_ms);

  snprintf(buffer, sizeof(buffer), "%d", soil.raw);
  draw_text(&epd, 0, 200, buffer, FONT_TAHOMA_8, BLACK, 3, canvas);

  float temperature = read_onboard_temperature(true);

  snprintf(buffer, sizeof(buffer), "%.2f", temperature);

  draw_text(&epd, 0, 236, "Temperature:", FONT_TAHOMA_8, BLACK, 3, canvas);
  draw_text(&epd, 90, 236, buffer, FONT_TAHOMA_8, BLACK, 3, canvas);

  if (!epd_display(&epd, canvas)) {
    printf("ERROR: EPD refresh timed out.\n");
  }
  if (!epd_sleep(&epd)) {
    printf("ERROR: EPD sleep timed out.\n");
  }

  // Wait for core 1 pump to finish before powering off
  multicore_fifo_pop_blocking();

  powman_enable_gpio_wakeup(0, WAKEUP_GPIO, false, false);

  int rc = enter_power_off_state();
  hard_assert(rc == PICO_OK);

  // Should never be reached: blink to signal a failed power-off.
  while (true) {
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
    sleep_ms(200);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
    sleep_ms(200);
  }

  return 0;
}
