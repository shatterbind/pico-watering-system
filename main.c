#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "ds3231.h"

// =============================================================================
// == Configuration                                                         ==
// =============================================================================

// -- I2C Configuration --
#define I2C_BAUDRATE (100 * 1000)
#define I2C_SCL_PIN 20
#define I2C_SDA_PIN 21

// -- RTC Setup Options --
#define SET_RTC_TIME_ON_BOOT 1
#define PRINT_TIME_EVERY_SEC 1

#define CONFIGURE_ALARM_1 1
#define CONFIGURE_ALARM_2 1

/**
 * @brief Select the operating mode for the INT/SQW output pin.
 * 0: Interrupt mode (for alarms).
 * 1: 1 Hz square wave.
 * 2: 1024 Hz square wave.
 * 3: 4096 Hz square wave.
 * 4: 8192 Hz square wave.
 */
#define RTC_OUTPUT_MODE 0

/**
 * @brief Delay in seconds to compensate for compilation and upload time.
 */
#define COMPILE_UPLOAD_OFFSET_SECONDS 6

// =============================================================================
// == Helper Functions                                                      ==
// =============================================================================

void setup_rtc(void)
{
#if SET_RTC_TIME_ON_BOOT
	printf("Setting RTC time from PC compile time...\n");

	struct tm compile_tm = {0};
	// Parse date "Mmm dd yyyy"
	sscanf(__DATE__, "%*s %d %d", &compile_tm.tm_mday, &compile_tm.tm_year);
	compile_tm.tm_year -= 1900; // tm_year is years since 1900

	// Parse time "hh:mm:ss"
	sscanf(__TIME__, "%d:%d:%d", &compile_tm.tm_hour, &compile_tm.tm_min, &compile_tm.tm_sec);

	// Convert month name to number
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
#endif

#if CONFIGURE_ALARM_1

	ds3231_alarm_time_t alarm_1 = {.day = 0, .hour = 0, .min = 0, .sec = 30};

	printf("Setting Alarm 1 to trigger when day=%d hour=%d minute=%d second=%d.\n",
		   alarm_1.day, alarm_1.hour, alarm_1.min, alarm_1.sec);

	if (ds3231_set_alarm1(&alarm_1, ALARM1_SECONDS_MATCH))
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

#if RTC_OUTPUT_MODE == 0
	ds3231_enable_interrupt_mode();
	printf("RTC configured for interrupt mode.\n");
#elif RTC_OUTPUT_MODE == 1
	ds3231_enable_sqw_output(SQW_FREQ_1HZ);
	printf("RTC configured for 1 Hz square wave output.\n");
#elif RTC_OUTPUT_MODE == 2
	ds3231_enable_sqw_output(SQW_FREQ_1024HZ);
	printf("RTC configured for 1024 Hz square wave output.\n");
#elif RTC_OUTPUT_MODE == 3
	ds3231_enable_sqw_output(SQW_FREQ_4096HZ);
	printf("RTC configured for 4096 Hz square wave output.\n");
#elif RTC_OUTPUT_MODE == 4
	ds3231_enable_sqw_output(SQW_FREQ_8192HZ);
	printf("RTC configured for 8192 Hz square wave output.\n");
#endif
	printf("\n");
}

// =============================================================================
// == Main Function                                                         ==
// =============================================================================

void main()
{
	stdio_init_all();

	sleep_ms(2000);

	printf("DS3231 RTC Example\n---\n");

	i2c_init(DS3231_I2C_PORT, I2C_BAUDRATE);
	gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
	gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
	gpio_pull_up(I2C_SDA_PIN);
	gpio_pull_up(I2C_SCL_PIN);

	setup_rtc();

	ds3231_datetime_t now;

	while (true)
	{
#if PRINT_TIME_EVERY_SEC
		if (ds3231_read_time(&now))
		{
			printf("Current Time: 20%02d-%02d-%02d %02d:%02d:%02d (Day %d)\n",
				   now.year, now.month, now.day, now.hour, now.min, now.sec, now.dow);
		}
		else
		{
			printf("ERROR: Failed to read time from RTC.\n");
		}
#endif
		if (ds3231_check_alarm_flag(ALARM_1))
		{
			printf("<<<<< ALARM 1 TRIGGERED! >>>>>\n");
			ds3231_clear_alarm_flag(ALARM_1); // Just clear the flag
		}

		if (ds3231_check_alarm_flag(ALARM_2))
		{
			printf("<<<<< ALARM 2 TRIGGERED! >>>>>\n");
			ds3231_clear_alarm_flag(ALARM_2); // Just clear the flag
		}

		sleep_ms(1000);
	}
}