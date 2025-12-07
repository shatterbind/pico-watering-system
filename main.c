#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/xosc.h"
#include "hardware/powman.h"
#include "hardware/sync.h"

#include "ds3231.h"
#include "epd.h"
#include "canvas.h"
#include "epd_2_13.h"

#define AIR_VALUE 4095.0f
#define WATER_VALUE 700.0f

#define MIN_HUMIDITY 40.0f

#define TIME_POMP_ON 5000
#define POMP_PIN 22

#define TEMP_SENSOR_CHANNEL 4
#define WAKEUP_GPIO 15

static powman_power_state off_state = POWMAN_POWER_STATE_NONE;
static powman_power_state on_state = POWMAN_POWER_STATE_NONE;

#define I2C_BAUDRATE (100 * 1000)
#define I2C_SCL_PIN 20
#define I2C_SDA_PIN 21


#define CONFIGURE_RTC 0
#define CONFIGURE_ALARM_1 1
#define CONFIGURE_ALARM_2 0

// #define DEBUG

/**
 * @brief Delay in seconds to compensate for compilation and upload time.
 */
#define COMPILE_UPLOAD_OFFSET_SECONDS 6

static int enter_power_off_state(void)
{
	stdio_flush();

	bool valid_state = powman_configure_wakeup_state(off_state, on_state);

	if (!valid_state)
	{
		return PICO_ERROR_INVALID_STATE;
	}

	powman_hw->boot[0] = 0;
	powman_hw->boot[1] = 0;
	powman_hw->boot[2] = 0;
	powman_hw->boot[3] = 0;

	int rc = powman_set_power_state(off_state);

	if (rc != PICO_OK)
	{
		return rc;
	}

	while (true)
	{
		__wfi();
	}

	return rc;
}

void humidity_sensor_init(uint gpio)
{
	adc_init();
	adc_set_temp_sensor_enabled(true);
	adc_gpio_init(gpio);
}

void pomp(const float humidity)
{

	if (humidity < MIN_HUMIDITY)
	{
		gpio_put(POMP_PIN, 1);
		sleep_ms(TIME_POMP_ON);
		gpio_put(POMP_PIN, 0);
	}
}

static float read_onboard_temperature(bool toCelsius)
{
	float adc;
	float tempC;
	const float conversionFactor = 3.3f / (1 << 12);

	adc_select_input(4);

	adc = (float)adc_read() * conversionFactor;
	tempC = 27.0f - (adc - 0.706f) / 0.001721f;

	return toCelsius ? tempC : tempC * 9 / 5 + 32;
}

static uint16_t read_humiduty()
{
	uint16_t raw;

	adc_select_input(0);
	raw = adc_read();

	return raw;
}

static int setup_rtc(void)
{
	printf("Setting RTC time from PC compile time...\n");

	struct tm compile_tm = {0};

	sscanf(__DATE__, "%*s %d %d", &compile_tm.tm_mday, &compile_tm.tm_year);

	compile_tm.tm_year -= 1900; // tm_year is years since 1900

	sscanf(__TIME__, "%d:%d:%d", &compile_tm.tm_hour, &compile_tm.tm_min, &compile_tm.tm_sec);

	const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	char month_str[4];

	sscanf(__DATE__, "%3s", month_str);

	for (int i = 0; i < 12; i++)
	{
		if (strcmp(month_str, months[i]) == 0)
		{
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

	if (ds3231_set_time(&dt_to_set))
	{
		printf("Time set successfully!\n");
	}
	else
	{
		printf("ERROR: Failed to set time.\n");
	}

#if CONFIGURE_ALARM_1

	ds3231_alarm_time_t alarm_1 = {.day = 0, .hour = 0, .min = 0, .sec = 0};

	printf("Setting Alarm 1 to trigger when day=%d hour=%d minute=%d second=%d.\n",
		   alarm_1.day, alarm_1.hour, alarm_1.min, alarm_1.sec);

	if (ds3231_set_alarm1(&alarm_1, ALARM1_MINUTES_SECONDS_MATCH))
	{
		printf("Alarm 1 configured successfully.\n");
	}
	else
	{
		printf("ERROR: Failed to configure Alarm 1.\n");
	}

#endif

#if CONFIGURE_ALARM_2

	ds3231_alarm_time_t alarm_2 = {.day = 0, .hour = 0, .min = 18};

	printf("Setting Alarm 2 to trigger when day=%d hour=%d minute=%d.\n",
		   alarm_2.day, alarm_2.hour, alarm_2.min);

	if (ds3231_set_alarm2(&alarm_2, ALARM2_MINUTES_MATCH))
	{
		printf("Alarm 2 configured successfully.\n");
	}
	else
	{
		printf("ERROR: Failed to configure Alarm 2.\n");
	}

#endif

	printf("RTC configure for interrupt mode.\n");
	ds3231_enable_interrupt_mode();
}

static void blink(uint32_t ms)
{
	gpio_put(PICO_DEFAULT_LED_PIN, 1);
	sleep_ms(ms);
	gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

void main()
{
	stdio_init_all();

	static uint8_t canvas[EPD_2_13_WIDTH * EPD_2_13_HEIGHT / 4] = {0};
	epd_t epd = epd_create(8);

	epd_init(&epd);

	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

	powman_timer_start();
	powman_set_debug_power_request_ignored(true);

	on_state = powman_power_state_with_domain_on(on_state, POWMAN_POWER_DOMAIN_SWITCHED_CORE);
	on_state = powman_power_state_with_domain_on(on_state, POWMAN_POWER_DOMAIN_XIP_CACHE);

	humidity_sensor_init(26);

	gpio_init(POMP_PIN);
	gpio_set_dir(POMP_PIN, GPIO_OUT);
	gpio_pull_down(POMP_PIN);

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

	ds3231_datetime_t now;

	char buffer[32] = {0};
	ds3231_read_time(&now);

	snprintf(buffer, sizeof(buffer), "%02d-%02d-%02d", now.day, now.month, now.year);

	draw_text(&epd, 0, 0, "Last update:", FONT_TAHOMA_8, BLACK, 3, canvas);
	draw_text(&epd, 0, 15, buffer, FONT_TAHOMA_16, BLACK, 3, canvas);

	snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", now.hour, now.min, now.sec);

	draw_text(&epd, 0, 40, buffer, FONT_TAHOMA_16, BLACK, 3, canvas);

#ifdef DEBUG
	printf("Current RTC time: %02d-%02d-%02d %02d:%02d:%02d\n",
		   now.day, now.month, now.year, now.hour, now.min, now.sec);
	while (true)
	{
		const uint16_t raw = read_humiduty();
		float humidity = (AIR_VALUE - raw) / (AIR_VALUE - WATER_VALUE) * 100.0f;

		snprintf(buffer, sizeof(buffer), "%.2f%%", humidity);

		printf("Raw humidity sensor value: %d\n", raw);
		printf("Humidity: %s\n", buffer);

		sleep_ms(1000);
	}
#endif

	const uint16_t raw = read_humiduty();
	float humidity = (AIR_VALUE - raw) / (AIR_VALUE - WATER_VALUE) * 100.0f;

	snprintf(buffer, sizeof(buffer), "%.2f%%", humidity);
	draw_text(&epd, 10, 120, buffer, FONT_TAHOMA_24, BLACK, 3, canvas);

	snprintf(buffer, sizeof(buffer), "%d", raw);
	draw_text(&epd, 0, 200, buffer, FONT_TAHOMA_8, BLACK, 3, canvas);

	float temperature = read_onboard_temperature(true);

	snprintf(buffer, sizeof(buffer), "%.2f", temperature);

	draw_text(&epd, 0, 240, "Temperature:", FONT_TAHOMA_8, BLACK, 3, canvas);
	draw_text(&epd, 90, 240, buffer, FONT_TAHOMA_8, BLACK, 3, canvas);

	epd_display(&epd, canvas);
	epd_sleep(&epd);

	if (ds3231_check_alarm_flag(ALARM_1))
	{
		printf("<<<<< ALARM 1 TRIGGERED! >>>>>\n");
		ds3231_clear_alarm_flag(ALARM_1);
	}

	if (ds3231_check_alarm_flag(ALARM_2))
	{
		printf("<<<<< ALARM 2 TRIGGERED! >>>>>\n");
		ds3231_clear_alarm_flag(ALARM_2);
	}

	powman_enable_gpio_wakeup(0, WAKEUP_GPIO, false, false);

	int rc = enter_power_off_state();
	hard_assert(rc == PICO_OK);

	// shall never to loop
	while (true)
	{
		gpio_put(PICO_DEFAULT_LED_PIN, 1);
		sleep_ms(200);
		gpio_put(PICO_DEFAULT_LED_PIN, 0);
		sleep_ms(200);
	}
}