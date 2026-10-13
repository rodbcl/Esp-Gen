/*
 * Gerador de pulsos - ESP32 DevKitC
 *
 * LCD I2C (via level shifter): SDA=GPIO21 SCL=GPIO22
 * Encoder KY-040: CLK=GPIO32 DT=GPIO33 SW=GPIO25
 * Botão START/STOP: GPIO26 (ativo em 0)
 * Saída de pulsos: GPIO27 -> Q1 (BC547) -> push-pull Q2/Q3 -> PULSO (24 V)
 *   O estágio de saída inverte: GPIO27 em 1 = saída desligada.
 */
#include "app_main.h"

/* ---------- NVS ---------- */
static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    settings_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, "s", &tmp, &len) == ESP_OK && len == sizeof(tmp) && tmp.version == SETTINGS_VERSION &&
        tmp.mode < MODE_COUNT) {
        s_cfg = tmp;
    }
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "s", &s_cfg, sizeof(s_cfg));
    nvs_commit(h);
    nvs_close(h);
}

/* ---------- Encoder (PCNT em quadratura x4) ---------- */
static void encoder_init(void)
{
    pcnt_unit_config_t ucfg = {
        .low_limit = -1000,
        .high_limit = 1000,
        .flags.accum_count = 1,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &s_pcnt));

    pcnt_glitch_filter_config_t fcfg = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &fcfg));

    pcnt_chan_config_t a_cfg = { .edge_gpio_num = PIN_ENC_CLK, .level_gpio_num = PIN_ENC_DT };
    pcnt_chan_config_t b_cfg = { .edge_gpio_num = PIN_ENC_DT, .level_gpio_num = PIN_ENC_CLK };
    pcnt_channel_handle_t ch_a, ch_b;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &a_cfg, &ch_a));
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &b_cfg, &ch_b));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(ch_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(ch_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_pcnt, ucfg.high_limit));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_pcnt, ucfg.low_limit));

    /* O KY-040 já tem pull-ups de 10k; os internos não atrapalham */
    gpio_pullup_en(PIN_ENC_CLK);
    gpio_pullup_en(PIN_ENC_DT);

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));
}

/* Retorna o número de "cliques" girados desde a última chamada (com sinal) */
static int encoder_read_detents(void)
{
    static int last_raw;
    static int acc;
    int raw = 0;
    pcnt_unit_get_count(s_pcnt, &raw);
    acc += raw - last_raw;
    last_raw = raw;
    int steps = acc / ENC_COUNTS_PER_DETENT;
    acc -= steps * ENC_COUNTS_PER_DETENT;
    return ENC_REVERSE ? -steps : steps;
}

/* ---------- Botões com debounce ---------- */
typedef struct {
    gpio_num_t pin;
    bool stable;        /* true = pressionado */
    bool last_raw;
    int64_t changed_us;
    int64_t pressed_us;  /* instante em que foi pressionado (após debounce) */
    bool hold_done;      /* pressão longa já tratada (ou ignorada) */
} button_t;

static button_t s_btn_enc = { .pin = PIN_ENC_SW };
static button_t s_btn_start = { .pin = PIN_BTN_START };

static void button_init(button_t *b)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << b->pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
}

/* Retorna true uma vez, no momento em que o botão é pressionado */
static bool button_pressed(button_t *b, int64_t now)
{
    bool raw = gpio_get_level(b->pin) == 0;
    if (raw != b->last_raw) {
        b->last_raw = raw;
        b->changed_us = now;
    }
    if (raw != b->stable && (now - b->changed_us) >= DEBOUNCE_MS * 1000) {
        b->stable = raw;
        if (raw) {
            b->pressed_us = now;
            b->hold_done = false;
        }
        return raw;
    }
    return false;
}

/* Retorna true uma vez, quando o botão completa `ms` pressionado. Chamar após button_pressed() */
static bool button_held(button_t *b, int64_t now, int ms)
{
    if (b->stable && !b->hold_done && (now - b->pressed_us) >= (int64_t)ms * 1000) {
        b->hold_done = true;
        return true;
    }
    return false;
}

/* ---------- Edição dos valores ---------- */
static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* No modo contínuo não há Tempo/Qtde */
static int item_count(void)
{
    return s_cfg.mode == MODE_CONT ? ITEM_AMOUNT : ITEM_COUNT;
}

static void apply_steps(int item, int steps, int accel)
{
    switch (item) {
    case ITEM_MODE: {
        int m = ((int)s_cfg.mode + steps) % MODE_COUNT;
        s_cfg.mode = m < 0 ? m + MODE_COUNT : m;
        break;
    }
    case ITEM_FREQ:
        s_cfg.freq = clamp((int32_t)s_cfg.freq + steps * accel, 0, FREQ_MAX);
        break;
    case ITEM_DUTY:
        s_cfg.duty = clamp((int32_t)s_cfg.duty + steps * (accel > 1 ? 5 : 1), 0, DUTY_MAX);
        break;
    case ITEM_AMOUNT:
        if (s_cfg.mode == MODE_TIME) {
            /* 1 s, 10 s ou 1 min por clique conforme a velocidade */
            int step = accel >= 100 ? 60 : accel;
            s_cfg.time_s = clamp((int32_t)s_cfg.time_s + steps * step, 0, TIME_MAX_S);
        } else {
            s_cfg.qty = clamp((int32_t)s_cfg.qty + steps * accel, 0, QTY_MAX);
        }
        break;
    }
}

/* Aceleração: girar rápido aumenta o passo (x1, x10, x100) */
static int encoder_accel(int steps, int64_t now)
{
    static int64_t last_us;
    int64_t dt = now - last_us;
    last_us = now;
    int abs_steps = steps < 0 ? -steps : steps;
    if (abs_steps > 1 || dt < 25000) {
        return 100;
    }
    if (dt < 70000) {
        return 10;
    }
    return 1;
}

/* ---------- Execução ---------- */
static void show_msg(const char *l1, const char *l2, int ms)
{
    snprintf(s_msg[0], sizeof(s_msg[0]), "%s", l1);
    snprintf(s_msg[1], sizeof(s_msg[1]), "%s", l2);
    s_msg_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
    s_state = UI_MSG;
}

static void run_start(void)
{
    if (s_cfg.freq == 0) {
        show_msg("Erro:", "Frequencia = 0", 1500);
        return;
    }
    if (s_cfg.duty == 0) {
        show_msg("Erro:", "Duty = 0%", 1500);
        return;
    }
    uint64_t periods;
    if (s_cfg.mode == MODE_CONT) {
        periods = PULSE_GEN_CONTINUOUS;
    } else {
        periods = s_cfg.mode == MODE_TIME ? (uint64_t)s_cfg.freq * s_cfg.time_s : s_cfg.qty;
    }
    if (periods == 0) {
        show_msg("Erro:", s_cfg.mode == MODE_TIME ? "Tempo = 0" : "Quantidade = 0", 1500);
        return;
    }
    if (pulse_gen_start(s_cfg.freq, s_cfg.duty, periods) != ESP_OK) {
        show_msg("Erro ao iniciar", "", 1500);
        return;
    }
    s_run_periods = periods;
    s_run_start_us = esp_timer_get_time();
    s_run_sel = ITEM_FREQ;
    s_edit_backup = s_cfg;
    /* A mesma pressão que iniciou não conta para parar, mesmo se o botão continuar apertado */
    s_btn_start.hold_done = true;
    s_state = UI_RUN;
}

/* Modo contínuo: ajusta freq/duty com a geração em andamento */
static void run_adjust(int steps, int accel)
{
    uint32_t freq = s_cfg.freq;
    uint16_t duty = s_cfg.duty;
    apply_steps(s_run_sel, steps, accel);
    /* 0 Hz ou 0% não são aceitos com a geração rodando */
    if (s_cfg.freq == 0) {
        s_cfg.freq = 1;
    }
    if (s_cfg.duty == 0) {
        s_cfg.duty = 1;
    }
    if (s_cfg.freq != freq || s_cfg.duty != duty) {
        pulse_gen_set(s_cfg.freq, s_cfg.duty);
    }
}

/* Encerra a geração; no modo contínuo salva freq/duty se foram alterados */
static void run_stop(const char *msg)
{
    pulse_gen_stop();
    if (memcmp(&s_edit_backup, &s_cfg, sizeof(s_cfg)) != 0) {
        settings_save();
    }
    show_msg(msg, "", 1500);
}

/* ---------- Tela ---------- */
static char s_screen[4][LCD_COLS + 1];
static char s_shadow[4][LCD_COLS + 1];

/* Copia cortando na largura do display */
static void copy_line(char *dst, const char *src)
{
    size_t len = strnlen(src, LCD_COLS);
    memcpy(dst, src, len);
    dst[len] = 0;
}

static void fmt_line(char *dst, char mark, const char *label, const char *value)
{
    char tmp[64];
    int pad = LCD_COLS - 1 - (int)strlen(label) - (int)strlen(value);
    if (pad < 1) {
        pad = 1;
    }
    snprintf(tmp, sizeof(tmp), "%c%s%*s%s", mark, label, pad, "", value);
    copy_line(dst, tmp);
}

static void item_text(int item, char *label, char *value, size_t n)
{
    switch (item) {
    case ITEM_MODE:
        snprintf(label, n, "Modo:");
        snprintf(value, n, "%s", s_cfg.mode == MODE_TIME ? "Tempo" : (s_cfg.mode == MODE_QTY ? "Qtde" : "Continuo"));
        break;
    case ITEM_FREQ:
        snprintf(label, n, "Freq:");
        snprintf(value, n, "%luHz", (unsigned long)s_cfg.freq);
        break;
    case ITEM_DUTY:
        snprintf(label, n, "Duty:");
        snprintf(value, n, "%u%%", s_cfg.duty);
        break;
    case ITEM_AMOUNT:
        if (s_cfg.mode == MODE_TIME) {
            snprintf(label, n, "Tempo:");
            snprintf(value, n, "%02lu:%02lu", (unsigned long)(s_cfg.time_s / 60), (unsigned long)(s_cfg.time_s % 60));
        } else {
            snprintf(label, n, "Qtde:");
            snprintf(value, n, "%lu", (unsigned long)s_cfg.qty);
        }
        break;
    }
}

static void render(void)
{
    for (int r = 0; r < LCD_ROWS; r++) {
        memset(s_screen[r], ' ', LCD_COLS);
        s_screen[r][LCD_COLS] = 0;
    }

    if (s_state == UI_NAV || s_state == UI_EDIT) {
        if (s_cursor < s_top) {
            s_top = s_cursor;
        } else if (s_cursor >= s_top + LCD_ROWS) {
            s_top = s_cursor - LCD_ROWS + 1;
        }
        for (int r = 0; r < LCD_ROWS && s_top + r < item_count(); r++) {
            int item = s_top + r;
            char label[12], value[12];
            item_text(item, label, value, sizeof(label));
            char mark = ' ';
            if (item == s_cursor) {
                mark = s_state == UI_EDIT ? '*' : '>';
            }
            fmt_line(s_screen[r], mark, label, value);
        }
    } else if (s_state == UI_RUN && s_cfg.mode == MODE_CONT) {
        int64_t secs = (esp_timer_get_time() - s_run_start_us) / 1000000;
        snprintf(s_screen[0], LCD_COLS + 1, "CONT %02ld:%02ld:%02ld", (long)(secs / 3600), (long)(secs / 60 % 60),
                 (long)(secs % 60));
        snprintf(s_screen[1], LCD_COLS + 1, "%c%luHz %c%u%%", s_run_sel == ITEM_FREQ ? '>' : ' ',
                 (unsigned long)s_cfg.freq, s_run_sel == ITEM_DUTY ? '>' : ' ', s_cfg.duty);
        if (LCD_ROWS >= 4) {
            snprintf(s_screen[3], LCD_COLS + 1, "Segure START: parar");
        }
    } else if (s_state == UI_RUN) {
        int64_t elapsed = esp_timer_get_time() - s_run_start_us;
        snprintf(s_screen[0], LCD_COLS + 1, "RUN %luHz %u%%", (unsigned long)s_cfg.freq, s_cfg.duty);
        if (s_cfg.mode == MODE_TIME) {
            int64_t left = (int64_t)s_cfg.time_s - elapsed / 1000000;
            if (left < 0) {
                left = 0;
            }
            snprintf(s_screen[1], LCD_COLS + 1, "Resta %02ld:%02ld", (long)(left / 60), (long)(left % 60));
        } else {
            uint64_t done = (uint64_t)elapsed * s_cfg.freq / 1000000;
            if (done > s_run_periods) {
                done = s_run_periods;
            }
            snprintf(s_screen[1], LCD_COLS + 1, "%llu/%llu", (unsigned long long)done,
                     (unsigned long long)s_run_periods);
        }
        if (LCD_ROWS >= 4) {
            snprintf(s_screen[3], LCD_COLS + 1, "Segure START: parar");
        }
    } else if (s_state == UI_MSG) {
        copy_line(s_screen[0], s_msg[0]);
        copy_line(s_screen[1], s_msg[1]);
    }

    /* Completa com espaços e envia só as linhas que mudaram */
    for (int r = 0; r < LCD_ROWS; r++) {
        size_t len = strlen(s_screen[r]);
        memset(s_screen[r] + len, ' ', LCD_COLS - len);
        s_screen[r][LCD_COLS] = 0;
        if (strcmp(s_screen[r], s_shadow[r]) != 0) {
            lcd_set_cursor(0, r);
            lcd_print(s_screen[r]);
            strcpy(s_shadow[r], s_screen[r]);
        }
    }
}

/* ---------- Loop principal ---------- */
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
                s_cursor = clamp(s_cursor + steps, 0, item_count() - 1);
            }
            if (enc_click) {
                s_edit_backup = s_cfg;
                s_state = UI_EDIT;
            }
            if (start_click) {
                run_start();
            }
            break;

        case UI_EDIT:
            if (steps) {
                apply_steps(s_cursor, steps, encoder_accel(steps, now));
            }
            if (enc_click) {
                if (memcmp(&s_edit_backup, &s_cfg, sizeof(s_cfg)) != 0) {
                    settings_save();
                }
                ESP_LOGI(TAG, "modo=%s freq=%lu duty=%u tempo=%lus qtde=%lu",
                         s_cfg.mode == MODE_TIME ? "tempo" : (s_cfg.mode == MODE_QTY ? "qtde" : "continuo"),
                         (unsigned long)s_cfg.freq,
                         s_cfg.duty, (unsigned long)s_cfg.time_s, (unsigned long)s_cfg.qty);
                s_state = UI_NAV;
            }
            break;

        case UI_RUN:
            if (s_cfg.mode == MODE_CONT) {
                if (steps) {
                    run_adjust(steps, encoder_accel(steps, now));
                }
                if (enc_click) {
                    s_run_sel = s_run_sel == ITEM_FREQ ? ITEM_DUTY : ITEM_FREQ;
                }
            }
            /* Toque curto é ignorado: só para segurando START */
            if (start_hold) {
                run_stop("Interrompido");
            } else if (!pulse_gen_running()) {
                show_msg("Concluido!", "", 1500);
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

void app_main(void)
{
    /* Saída desligada o quanto antes (GPIO27 em 1 = Q1 conduz = PULSO em 0 V) */
    ESP_ERROR_CHECK(pulse_gen_init(PIN_PULSE_OUT, PULSE_INVERTED));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();

    button_init(&s_btn_enc);
    button_init(&s_btn_start);
    encoder_init();

    if (lcd_init(PIN_SDA, PIN_SCL, LCD_COLS, LCD_ROWS) == ESP_OK) {
        lcd_set_cursor(0, 0);
        lcd_print("Gerador Pulsos");
        vTaskDelay(pdMS_TO_TICKS(1000));
        lcd_clear();
    }

    xTaskCreate(ui_task, "ui", 4096, NULL, 5, NULL);
}
