#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diag_internal.h"
#include "esp_console.h"
#include "pcf85063.h"
#include "power.h"
#include "sensors.h"
#include "timekeeping.h"
#include "timekeeping_iso.h"

static int usage(const char *text)
{
    printf("usage: %s\n", text);
    return 1;
}

static void format_local(time_t t, char *out, size_t size)
{
    struct tm local;
    localtime_r(&t, &local);
    strftime(out, size, "%a %Y-%m-%d %H:%M:%S %Z", &local);
}

static int sensors_body(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = sensors_sample_env(time(NULL));
    if (err != ESP_OK) {
        printf("sensors: AHT20 read failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    sensors_env_t env = sensors_env();
    int t = abs(env.temp_c100);
    printf("sensors: %s%d.%02d C, %d.%02d %%RH\n", env.temp_c100 < 0 ? "-" : "", t / 100, t % 100, env.hum_pct100 / 100,
           env.hum_pct100 % 100);
    return 0;
}

static const char *battery_state_name(battery_state_t state)
{
    switch (state) {
    case BATTERY_DISCHARGING:
        return "discharging";
    case BATTERY_CHARGING:
        return "charging";
    case BATTERY_FULL:
        return "full";
    default:
        return "unknown";
    }
}

static int battery_body(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "learn") == 0) { /* D21: from the next full discharge */
        if (argc != 3 || (strcmp(argv[2], "start") != 0 && strcmp(argv[2], "stop") != 0)) {
            printf("usage: battery learn start|stop\n");
            return 1;
        }
        if (strcmp(argv[2], "start") == 0) {
            sensors_learn_start();
        } else {
            sensors_learn_stop();
        }
    }
    time_t now = time(NULL);
    esp_err_t err = sensors_sample_battery(now);
    if (err != ESP_OK) {
        printf("battery: ADC read failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    sensors_battery_t b = sensors_battery(now);
    bool usb = power_tethered();
    if (usb && b.state == BATTERY_UNKNOWN) {
        printf("battery: %d mV now, %d mV smoothed, %d %%, usb\n", b.last_mv, b.smoothed_mv, b.level);
    } else if (usb) {
        printf("battery: %d mV now, %d mV smoothed, %d %%, %s (usb)\n", b.last_mv, b.smoothed_mv, b.level,
               battery_state_name(b.state));
    } else {
        printf("battery: %d mV now, %d mV smoothed, %d %%, %s\n", b.last_mv, b.smoothed_mv, b.level,
               battery_state_name(b.state));
    }
    const battery_learn_t *l = sensors_learn();
    printf("learning: %s, %u h of discharge, %d points\n", battery_learn_state_name(battery_learn_state(l)),
           (unsigned)battery_learn_hours(l), battery_learn_points(l));
    return 0;
}

static int rtc_body(int argc, char **argv)
{
    static const char *const k_usage = "rtc get | rtc set <YYYY-MM-DDTHH:MM[:SS][Z|+HH:MM]>";
    if (argc == 2 && strcmp(argv[1], "get") == 0) {
        time_t utc;
        bool valid;
        esp_err_t err = pcf85063_read(&utc, &valid);
        if (err != ESP_OK) {
            printf("rtc: %s\n", esp_err_to_name(err));
            return 1;
        }
        char iso[32], local[48];
        struct tm tm_utc;
        gmtime_r(&utc, &tm_utc);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        format_local(utc, local, sizeof(local));
        printf("rtc: %s (%s), local %s\n", iso, valid ? "valid" : "INVALID: oscillator stopped, set the time",
               local);
        const rtc_trim_t *trim = timekeeping_trim(); /* spec §7, D25 */
        int drift10 = timekeeping_trim_drift_s10_per_day(trim);
        printf("trim %d steps (%+.2f ppm)%s", trim->offset, -trim->offset * TRIM_STEP_PPB / 1000.0,
               trim->set_at_ms != 0 ? "" : "; not set to the millisecond since the last manual set");
        if (trim->drift_ppb != TRIM_NO_DRIFT) {
            printf("; last drift %s%d.%d s a day", drift10 < 0 ? "-" : "+", abs(drift10) / 10, abs(drift10) % 10);
        }
        printf("\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        time_t utc;
        struct tm year_check;
        if (!timekeeping_parse_iso8601(argv[2], &utc) || gmtime_r(&utc, &year_check) == NULL ||
            year_check.tm_year < 100 || year_check.tm_year > 199) { /* the RTC holds 2000-2099 */
            return usage(k_usage);
        }
        esp_err_t err = timekeeping_set_utc(utc);
        if (err != ESP_OK) {
            printf("rtc: %s\n", esp_err_to_name(err));
            return 1;
        }
        char local[48];
        format_local(utc, local, sizeof(local));
        printf("rtc: set, local %s\n", local);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "drift") == 0) {
        const rtc_trim_t *trim = timekeeping_trim();
        if (trim->drift_ppb == TRIM_NO_DRIFT) {
            printf("rtc drift: unmeasured\n");
            return 0;
        }
        int drift10 = timekeeping_trim_drift_s10_per_day(trim);
        double pct = (double)trim->drift_ppb / 10000000.0;
        printf("rtc drift: %ld ppb (%+.4f%%, %s%d.%d s/day)",
               (long)trim->drift_ppb, pct,
               drift10 < 0 ? "-" : "+", abs(drift10) / 10, abs(drift10) % 10);
        int16_t cal_t = timekeeping_cal_temp_c100();
        int16_t cur_t = timekeeping_cur_temp_c100();
        if (cal_t != TIMEKEEPING_TEMP_UNSET && cur_t != TIMEKEEPING_TEMP_UNSET) {
            printf(" @ cal %.1f C, now %.1f C", cal_t / 100.0, cur_t / 100.0);
            if (timekeeping_temp_excursion_active()) {
                printf(" [thermal >=5C: 2h SNTP]");
            }
        } else if (cal_t == TIMEKEEPING_TEMP_UNSET) {
            printf(" (cal temp: unset until next SNTP");
            if (cur_t != TIMEKEEPING_TEMP_UNSET) {
                printf(", now %.1f C)", cur_t / 100.0);
            } else {
                printf(")");
            }
        } else {
            printf(" @ cal %.1f C (now temp: unset)", cal_t / 100.0);
        }
        printf("\n");
        return 0;
    }
    return usage(k_usage);
}

static int cmd_sensors(int argc, char **argv)
{
    return diag_on_owner(sensors_body, argc, argv);
}

static int cmd_battery(int argc, char **argv)
{
    return diag_on_owner(battery_body, argc, argv);
}

static int cmd_rtc(int argc, char **argv)
{
    return diag_on_owner(rtc_body, argc, argv);
}

esp_err_t diag_register_sensor_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "sensors", .help = "Read the AHT20 now", .func = &cmd_sensors },
        { .command = "battery", .help = "Read the battery now and show the gauge; battery learn start|stop",
          .func = &cmd_battery },
        { .command = "rtc", .help = "rtc get | rtc set <ISO 8601 time; Z, +HH:MM or local> | rtc drift", .func = &cmd_rtc },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
