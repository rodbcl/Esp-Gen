#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "button.h"
#include "encoder.h"
#include "lcd_i2c.h"
#include "run.h"
#include "screen.h"
#include "settings.h"
#include "util.h"

static const char *TAG = "ui";

#define LOOP_MS         5
#define MSG_MS          1500
#define STOP_HOLD_MS    2000    // tempo segurando START para parar a geração

typedef enum { UI_NAV, UI_EDIT, UI_RUN, UI_MSG } ui_state_t;

static ui_state_t s_state = UI_NAV;
static settings_t *s_cfg;
static int s_cursor;
static int s_top;
static settings_t s_edit_backup;
static settings_field_t s_run_sel;  // modo contínuo: campo ajustado pelo encoder (FIELD_FREQ ou FIELD_DUTY)
static int64_t s_msg_until_us;
static char s_msg[2][21];
static button_t s_btn_enc;
static button_t s_btn_start;

static void show_msg(const char *l1, const char *l2, int ms)
{
    snprintf(s_msg[0], sizeof(s_msg[0]), "%s", l1);
    snprintf(s_msg[1], sizeof(s_msg[1]), "%s", l2);
    s_msg_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
    s_state = UI_MSG;
}

static void start(void)
{
    const char *err = run_start();
    if (err) {
        show_msg("Erro:", err, MSG_MS);
        return;
    }
    s_run_sel = FIELD_FREQ;
    //A mesma pressão que iniciou não conta para parar, mesmo se o botão continuar apertado
    button_ignore_hold(&s_btn_start);
    s_state = UI_RUN;
}

//Tela
static void item_text(int item, char *label, char *value, size_t n)
{
    switch (item) {
    case FIELD_MODE:
        snprintf(label, n, "Modo:");
        snprintf(value, n, "%s", s_cfg->mode == MODE_TIME ? "Tempo" : (s_cfg->mode == MODE_QTY ? "Qtde" : "Continuo"));
        break;
    case FIELD_FREQ:
        snprintf(label, n, "Freq:");
        snprintf(value, n, "%luHz", (unsigned long)s_cfg->freq);
        break;
    case FIELD_DUTY:
        snprintf(label, n, "Duty:");
        snprintf(value, n, "%u%%", s_cfg->duty);
        break;
    case FIELD_AMOUNT:
        if (s_cfg->mode == MODE_TIME) {
            snprintf(label, n, "Tempo:");
            snprintf(value, n, "%02lu:%02lu", (unsigned long)(s_cfg->time_s / 60), (unsigned long)(s_cfg->time_s % 60));
        } else {
            snprintf(label, n, "Qtde:");
            snprintf(value, n, "%lu", (unsigned long)s_cfg->qty);
        }
        break;
    }
}

static void render(void)
{
    screen_begin();

    if (s_state == UI_NAV || s_state == UI_EDIT) {
        if (s_cursor < s_top) {
            s_top = s_cursor;
        } else if (s_cursor >= s_top + LCD_ROWS) {
            s_top = s_cursor - LCD_ROWS + 1;
        }
        for (int r = 0; r < LCD_ROWS && s_top + r < settings_field_count(); r++) {
            int item = s_top + r;
            char label[12], value[12];
            item_text(item, label, value, sizeof(label));
            char mark = ' ';
            if (item == s_cursor) {
                mark = s_state == UI_EDIT ? '*' : '>';
            }
            screen_item(r, mark, label, value);
        }
    } else if (s_state == UI_RUN && s_cfg->mode == MODE_CONT) {
        int64_t secs = run_elapsed_us() / 1000000;
        screen_printf(0, "CONT %02ld:%02ld:%02ld", (long)(secs / 3600), (long)(secs / 60 % 60), (long)(secs % 60));
        screen_printf(1, "%c%luHz %c%u%%", s_run_sel == FIELD_FREQ ? '>' : ' ', (unsigned long)s_cfg->freq,
                      s_run_sel == FIELD_DUTY ? '>' : ' ', s_cfg->duty);
        screen_printf(3, "Segure START: parar");
    } else if (s_state == UI_RUN) {
        int64_t elapsed = run_elapsed_us();
        screen_printf(0, "RUN %luHz %u%%", (unsigned long)s_cfg->freq, s_cfg->duty);
        if (s_cfg->mode == MODE_TIME) {
            int64_t left = (int64_t)s_cfg->time_s - elapsed / 1000000;
            if (left < 0) {
                left = 0;
            }
            screen_printf(1, "Resta %02ld:%02ld", (long)(left / 60), (long)(left % 60));
        } else {
            uint64_t done = (uint64_t)elapsed * s_cfg->freq / 1000000;
            if (done > run_periods()) {
                done = run_periods();
            }
            screen_printf(1, "%llu/%llu", (unsigned long long)done, (unsigned long long)run_periods());
        }
        screen_printf(3, "Segure START: parar");
    } else if (s_state == UI_MSG) {
        screen_printf(0, "%s", s_msg[0]);
        screen_printf(1, "%s", s_msg[1]);
    }

    screen_flush();
}

//Loop principal
static void ui_task(void *arg)
{
    int64_t last_render = 0;
    while (1) {
        int64_t now = esp_timer_get_time();
        int steps = encoder_read_detents();
        bool enc_click = button_pressed(&s_btn_enc, now);
        bool start_click = button_pressed(&s_btn_start, now);
        bool start_hold = button_held(&s_btn_start, now, STOP_HOLD_MS);

        switch (s_state) {
        case UI_NAV:
            if (steps) {
                s_cursor = clamp_i32(s_cursor + steps, 0, settings_field_count() - 1);
            }
            if (enc_click) {
                s_edit_backup = *s_cfg;
                s_state = UI_EDIT;
            }
            if (start_click) {
                start();
            }
            break;

        case UI_EDIT:
            if (steps) {
                settings_adjust(s_cursor, steps, encoder_accel(steps, now));
            }
            if (enc_click) {
                if (memcmp(&s_edit_backup, s_cfg, sizeof(*s_cfg)) != 0) {
                    settings_save();
                }
                ESP_LOGI(TAG, "modo=%s freq=%lu duty=%u tempo=%lus qtde=%lu",
                         s_cfg->mode == MODE_TIME ? "tempo" : (s_cfg->mode == MODE_QTY ? "qtde" : "continuo"),
                         (unsigned long)s_cfg->freq,
                         s_cfg->duty, (unsigned long)s_cfg->time_s, (unsigned long)s_cfg->qty);
                s_state = UI_NAV;
            }
            break;

        case UI_RUN:
            if (s_cfg->mode == MODE_CONT) {
                if (steps) {
                    run_adjust(s_run_sel, steps, encoder_accel(steps, now));
                }
                if (enc_click) {
                    s_run_sel = s_run_sel == FIELD_FREQ ? FIELD_DUTY : FIELD_FREQ;
                }
            }
            //Toque curto é ignorado: só para segurando START
            if (start_hold) {
                run_stop();
                show_msg("Interrompido", "", MSG_MS);
            } else if (run_finished()) {
                show_msg("Concluido!", "", MSG_MS);
            }
            break;

        case UI_MSG:
            if (now >= s_msg_until_us) {
                s_state = UI_NAV;
            }
            break;
        }

        if (now - last_render >= 50000 || steps || enc_click || start_click || start_hold) {
            render();
            last_render = now;
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}

void ui_start(void)
{
    s_cfg = settings_get();

    button_init(&s_btn_enc, PIN_ENC_SW);
    button_init(&s_btn_start, PIN_BTN_START);
    encoder_init(PIN_ENC_CLK, PIN_ENC_DT, ENC_COUNTS_PER_DETENT, ENC_REVERSE);

    if (lcd_init(PIN_SDA, PIN_SCL, LCD_COLS, LCD_ROWS) == ESP_OK) {
        lcd_set_cursor(0, 0);
        lcd_print("Gerador Pulsos");
        vTaskDelay(pdMS_TO_TICKS(1000));
        lcd_clear();
    }

    xTaskCreate(ui_task, "ui", 4096, NULL, 5, NULL);
}
