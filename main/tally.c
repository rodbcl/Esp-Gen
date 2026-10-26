#include "tally.h"

#include "freertos/FreeRTOS.h"
#include "esp_attr.h"

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

void IRAM_ATTR tally_add_valid(bool reported)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    if (reported) {
        s_tally.valid_rep++;
    } else {
        s_tally.valid++;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void IRAM_ATTR tally_add_reject(bool reported)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    if (reported) {
        s_tally.rejects_rep++;
    } else {
        s_tally.rejects++;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void IRAM_ATTR tally_add_noise(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_tally.noise++;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void IRAM_ATTR tally_add_stop(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_tally.stops++;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void IRAM_ATTR tally_add_report(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_tally.reports++;
    portEXIT_CRITICAL_SAFE(&s_lock);
}
