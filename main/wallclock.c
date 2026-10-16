#include "wallclock.h"

#include <stdio.h>
#include <time.h>

#include "esp_timer.h"

static uint32_t s_base_epoch;
static int64_t s_base_us;
static bool s_confirmed;
static int32_t s_tz_min;

void wallclock_set(uint32_t epoch, bool confirmed)
{
    s_base_epoch = epoch;
    s_base_us = esp_timer_get_time();
    s_confirmed = confirmed;
}

uint32_t wallclock_now(void)
{
    if (s_base_epoch == 0) {
        return 0;
    }
    return s_base_epoch + (uint32_t)((esp_timer_get_time() - s_base_us) / 1000000);
}

bool wallclock_confirmed(void)
{
    return s_confirmed;
}

void wallclock_set_tz(int32_t offset_min)
{
    s_tz_min = offset_min;
}

void wallclock_format(uint32_t epoch, char *buf, int len)
{
    time_t t = (time_t)epoch + (time_t)s_tz_min * 60;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, len, "%02d/%02d/%04d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
}
