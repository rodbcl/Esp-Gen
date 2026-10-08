#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lcd_i2c.h"
#include "pulse_gen.h"

static const char *TAG = "app";

/* ---------- Configuração de hardware ---------- */
#define PIN_SDA         21
#define PIN_SCL         22
#define PIN_ENC_CLK     32
#define PIN_ENC_DT      33
#define PIN_ENC_SW      25
#define PIN_BTN_START   26
#define PIN_PULSE_OUT   27
#define PULSE_INVERTED  true    /* Q1 inverte o sinal */

#define LCD_COLS        16      /* ajuste para 20 se o display for 20x4 */
#define LCD_ROWS        2       /* ajuste para 4 se o display for 20x4 */

#define ENC_COUNTS_PER_DETENT 2 /* transições por "clique" (KY-040: 2 ou 4, depende do modelo) */
#define ENC_REVERSE     false   /* troque se o sentido ficar invertido */

/* ---------- Limites ---------- */
#define FREQ_MAX        10000
#define DUTY_MAX        100
#define TIME_MAX_S      (60 * 60)
#define QTY_MAX         10000

#define LOOP_MS         5
#define DEBOUNCE_MS     30

/* ---------- Configurações do usuário ---------- */
typedef enum { MODE_TIME = 0, MODE_QTY = 1 } run_mode_t;

typedef struct {
    uint8_t version;
    uint8_t mode;
    uint16_t duty;
    uint32_t freq;
    uint32_t time_s;
    uint32_t qty;
} settings_t;

#define SETTINGS_VERSION 1

static settings_t s_cfg = {
    .version = SETTINGS_VERSION,
    .mode = MODE_TIME,
    .duty = 50,
    .freq = 1000,
    .time_s = 10,
    .qty = 100,
};

enum { ITEM_MODE, ITEM_FREQ, ITEM_DUTY, ITEM_AMOUNT, ITEM_COUNT };

typedef enum { UI_NAV, UI_EDIT, UI_RUN, UI_MSG } ui_state_t;

static ui_state_t s_state = UI_NAV;
static int s_cursor;
static int s_top;
static settings_t s_edit_backup;
static int64_t s_run_start_us;
static uint64_t s_run_periods;
static int64_t s_msg_until_us;
static char s_msg[2][21];

static pcnt_unit_handle_t s_pcnt;