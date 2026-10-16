#include "wallclock.h"

#include "esp_timer.h"

static uint32_t s_base_epoch;
static int64_t s_base_us;
static bool s_confirmed;

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
