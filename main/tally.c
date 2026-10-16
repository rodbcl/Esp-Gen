#include "tally.h"

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static tally_t s_tally;

void tally_reset(const tally_t *from)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_tally = from ? *from : (tally_t) { 0 };
    portEXIT_CRITICAL_SAFE(&s_lock);
}

tally_t tally_get(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    tally_t t = s_tally;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return t;
}

void tally_add_valid(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_tally.valid++;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void tally_add_noise(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_tally.noise++;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void tally_add_stop(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_tally.stops++;
    portEXIT_CRITICAL_SAFE(&s_lock);
}
