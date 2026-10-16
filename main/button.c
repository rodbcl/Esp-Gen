#include "button.h"

#define DEBOUNCE_MS     30

void button_init(button_t *b, gpio_num_t pin)
{
    *b = (button_t) { .pin = pin };
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
}

bool button_pressed(button_t *b, int64_t now_us)
{
    bool raw = gpio_get_level(b->pin) == 0;
    if (raw != b->last_raw) {
        b->last_raw = raw;
        b->changed_us = now_us;
    }
    if (raw != b->stable && (now_us - b->changed_us) >= DEBOUNCE_MS * 1000) {
        b->stable = raw;
        if (raw) {
            b->pressed_us = now_us;
            b->hold_done = false;
        }
        return raw;
    }
    return false;
}

bool button_held(button_t *b, int64_t now_us, int ms)
{
    if (b->stable && !b->hold_done && (now_us - b->pressed_us) >= (int64_t)ms * 1000) {
        b->hold_done = true;
        return true;
    }
    return false;
}

void button_ignore_hold(button_t *b)
{
    b->hold_done = true;
}
