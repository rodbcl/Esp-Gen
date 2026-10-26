#pragma once

#include <stdint.h>

// Configurações do usuário, persistidas na NVS. Requer nvs_flash_init() antes de settings_load().

#define FREQ_DHZ_MAX    100000  // 10 kHz, em décimos de Hz
#define DUTY_MAX        100
#define TIME_MAX_S      (60 * 60)
#define QTY_MAX         10000

// MODE_AUTO e MODE_AGENT só existem no menu: ao confirmar, entram no modo e nunca são salvos
typedef enum { MODE_TIME = 0, MODE_QTY = 1, MODE_CONT = 2, MODE_AUTO = 3, MODE_AGENT = 4, MODE_COUNT } run_mode_t;

typedef struct {
    uint8_t version;
    uint8_t mode;
    uint16_t duty;
    uint32_t freq_dhz;      // décimos de Hz (10 = 1 Hz)
    uint32_t time_s;
    uint32_t qty;
} settings_t;

//Campos editáveis pelo menu, na ordem em que aparecem
typedef enum { FIELD_MODE, FIELD_FREQ, FIELD_DUTY, FIELD_AMOUNT, FIELD_COUNT } settings_field_t;

void settings_load(void);
void settings_save(void);
settings_t *settings_get(void);
// Número de campos visíveis no modo atual (no Contínuo não há Tempo/Qtde; na Automação e no Agente só o Modo)
int settings_field_count(void);
// Altera um campo em `steps` cliques do encoder; `accel` = 1, 10 ou 100
void settings_adjust(settings_field_t field, int steps, int accel);
