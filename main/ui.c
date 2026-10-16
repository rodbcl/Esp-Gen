#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "automation.h"
#include "board.h"
#include "button.h"
#include "encoder.h"
#include "lcd_i2c.h"
#include "run.h"
#include "screen.h"
#include "settings.h"
#include "util.h"
#include "wifi_ap.h"

static const char *TAG = "ui";

#define LOOP_MS         5
#define MSG_MS          1500
#define STOP_HOLD_MS    2000    // tempo segurando START para parar a geração
#define AUTO_ROTATE_MS  3000    // modo Automação: troca da informação na 2ª linha

typedef enum { UI_NAV, UI_EDIT, UI_RUN, UI_MSG, UI_AUTO } ui_state_t;

static const char *const MODE_NAMES[MODE_COUNT] = { "Tempo", "Qtde", "Continuo", "Automacao" };

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

static void enter_auto(void)
{
    automation_enter();
    s_state = UI_AUTO;
}

//Tela
static void item_text(int item, char *label, char *value, size_t n)
{
    switch (item) {
    case FIELD_MODE:
        snprintf(label, n, "Modo:");
        snprintf(value, n, "%s", MODE_NAMES[s_cfg->mode]);
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

/*
 * Automação sem teste: como conectar (nome do AP, senha e IP se alternam).
 * Com teste: pulsos válidos enviados e, alternando, ritmo/parada, tempo de teste e AP.
 */
static void render_auto(void)
{
    automation_status_t st = automation_status();
    int slot = (int)(esp_timer_get_time() / (AUTO_ROTATE_MS * 1000LL)) % 3;

    if (!st.running) {
        screen_printf(0, "Conecte-se ao AP");
        if (slot == 0) {
            screen_printf(1, "%s", wifi_ap_ssid());
        } else if (slot == 1) {
            screen_printf(1, "Senha %s", WIFI_AP_PASS);
        } else {
            screen_printf(1, "IP %s", WIFI_AP_IP);
        }
    } else {
        screen_printf(0, "Pulsos%10llu", (unsigned long long)st.tally.valid);
        if (slot == 0 && st.gen.stopped) {
            unsigned long left = st.gen.stop_left_s;
            if (left >= 3600) {
                screen_printf(1, "Maq parada %luh%02lu", left / 3600, left / 60 % 60);
            } else {
                screen_printf(1, "Maq parada %02lu:%02lu", left / 60, left % 60);
            }
        } else if (slot == 0) {
            screen_printf(1, "Ritmo %lu,%02lu Hz", (unsigned long)(st.gen.rate_mhz / 1000),
                          (unsigned long)(st.gen.rate_mhz % 1000 / 10));
        } else if (slot == 1) {
            unsigned long e = st.elapsed_s;
            if (e >= 86400) {
                screen_printf(1, "Tempo %lud%02lu:%02lu:%02lu", e / 86400, e / 3600 % 24, e / 60 % 60, e % 60);
            } else {
                screen_printf(1, "Tempo %02lu:%02lu:%02lu", e / 3600, e / 60 % 60, e % 60);
            }
        } else {
            screen_printf(1, "AP %s", wifi_ap_ssid());
        }
    }
    screen_printf(2, "%s", wifi_ap_ssid());
    screen_printf(3, "Segure START:sair");
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
    } else if (s_state == UI_AUTO) {
        render_auto();
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
            if (enc_click && s_cfg->mode == MODE_AUTO) {
                // A Automação não é salva: ao sair dela o menu volta no modo anterior
                s_cfg->mode = s_edit_backup.mode;
                enter_auto();
            } else if (enc_click) {
                if (memcmp(&s_edit_backup, s_cfg, sizeof(*s_cfg)) != 0) {
                    settings_save();
                }
                ESP_LOGI(TAG, "modo=%s freq=%lu duty=%u tempo=%lus qtde=%lu", MODE_NAMES[s_cfg->mode],
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

        case UI_AUTO:
            // Segurar START sai do modo, interrompendo o teste se houver
            if (start_hold) {
                bool was_running = automation_status().running;
                automation_exit();
                show_msg(was_running ? "Teste parado" : "Automacao", "Wi-Fi desligado", MSG_MS);
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

// Texto curto do motivo do último reset, ou NULL se foi um reset normal
static const char *reset_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_BROWNOUT:  return "Reset: brownout";
    case ESP_RST_PANIC:     return "Reset: panic";
    case ESP_RST_INT_WDT:   return "Reset: int WDT";
    case ESP_RST_TASK_WDT:  return "Reset: task WDT";
    case ESP_RST_WDT:       return "Reset: WDT";
    case ESP_RST_SW:        return "Reset: software";
    default:                return NULL;
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
        // Se o último reset não foi normal, mostra o motivo por mais tempo
        const char *why = reset_text(esp_reset_reason());
        if (why) {
            ESP_LOGW(TAG, "reiniciou por: %s", why);
            lcd_set_cursor(0, 1);
            lcd_print(why);
        }
        vTaskDelay(pdMS_TO_TICKS(why ? 4000 : 1000));
        lcd_clear();
    }

    // Faltou energia com um teste rodando: volta direto para a Automação e retoma
    if (automation_resume_pending()) {
        enter_auto();
    }

    // Pilha maior: a entrada na Automação liga o Wi-Fi (e calibra o rádio) a partir desta task
    xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
}
