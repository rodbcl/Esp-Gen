#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

/* Botão ativo em 0 com pull-up interno e debounce por software (chamar periodicamente). */

typedef struct {
    gpio_num_t pin;
    bool stable;        /* true = pressionado */
    bool last_raw;
    int64_t changed_us;
    int64_t pressed_us;  /* instante em que foi pressionado (após debounce) */
    bool hold_done;      /* pressão longa já tratada (ou ignorada) */
} button_t;

void button_init(button_t *b, gpio_num_t pin);
/* Retorna true uma vez, no momento em que o botão é pressionado */
bool button_pressed(button_t *b, int64_t now_us);
/* Retorna true uma vez, quando o botão completa `ms` pressionado. Chamar após button_pressed() */
bool button_held(button_t *b, int64_t now_us, int ms);
/* A pressão atual não gera mais button_held(), mesmo se o botão continuar apertado */
void button_ignore_hold(button_t *b);
