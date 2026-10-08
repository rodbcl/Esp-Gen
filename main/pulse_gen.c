#include "pulse_gen.h"

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pulse";

/* 10 MHz -> 0,1 us de resolução (a 10 kHz, 1% de duty = 10 ticks) */
#define RMT_RES_HZ    10000000UL
/*
 * Duração máxima de cada meio-símbolo. O limite do hardware é 32767, mas
 * mantê-lo curto garante que a memória do RMT (64 símbolos) esvazie
 * rapidamente quando a geração é interrompida.
 */
#define MAX_HALF_TICKS 2000

typedef struct {
    uint32_t freq;
    uint32_t duty;
    uint32_t period_base;   /* RMT_RES_HZ / freq */
    uint32_t period_rem;    /* RMT_RES_HZ % freq, distribuído via acumulador */
    uint32_t err;
    uint64_t periods_left;
    uint32_t cur_toff;
    bool off_pending;
    uint32_t seg_level;
    uint32_t seg_left;
    volatile bool abort;
} gen_state_t;

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static gen_state_t s_gen;
static volatile bool s_running;

/* Fornece o próximo meio-símbolo (nível, duração). Retorna false quando acabou. */
static bool next_half(gen_state_t *g, uint32_t *level, uint32_t *dur)
{
    while (g->seg_left == 0) {
        if (g->abort) {
            return false;
        }
        if (g->off_pending) {
            g->off_pending = false;
            if (g->cur_toff > 0) {
                g->seg_level = 0;
                g->seg_left = g->cur_toff;
            }
            continue;
        }
        if (g->periods_left == 0) {
            return false;
        }
        g->periods_left--;

        uint32_t p = g->period_base;
        g->err += g->period_rem;
        if (g->err >= g->freq) {
            g->err -= g->freq;
            p++;
        }
        uint32_t ton = (uint32_t)(((uint64_t)p * g->duty) / 100);
        g->cur_toff = p - ton;
        g->off_pending = true;
        if (ton > 0) {
            g->seg_level = 1;
            g->seg_left = ton;
        }
    }
    if (g->abort) {
        g->seg_left = 0;
        return false;
    }
    uint32_t chunk = g->seg_left > MAX_HALF_TICKS ? MAX_HALF_TICKS : g->seg_left;
    g->seg_left -= chunk;
    *level = g->seg_level;
    *dur = chunk;
    return true;
}

static size_t encode_cb(const void *data, size_t data_size, size_t symbols_written, size_t symbols_free,
                        rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    gen_state_t *g = arg;
    size_t n = 0;
    while (n < symbols_free) {
        uint32_t l0, d0, l1, d1;
        if (!next_half(g, &l0, &d0)) {
            *done = true;
            break;
        }
        bool last = false;
        if (!next_half(g, &l1, &d1)) {
            l1 = 0; /* completa o símbolo com 0,1 us em nível inativo */
            d1 = 1;
            last = true;
        }
        symbols[n++] = (rmt_symbol_word_t) {
            .level0 = l0, .duration0 = d0, .level1 = l1, .duration1 = d1,
        };
        if (last) {
            *done = true;
            break;
        }
    }
    return n;
}

static bool on_done(rmt_channel_handle_t chan, const rmt_tx_done_event_data_t *ev, void *ctx)
{
    s_running = false;
    return false;
}

esp_err_t pulse_gen_init(int gpio, bool invert)
{
    rmt_tx_channel_config_t cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
        .flags.invert_out = invert,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&cfg, &s_chan), TAG, "rmt channel");

    rmt_tx_event_callbacks_t cbs = { .on_trans_done = on_done };
    ESP_RETURN_ON_ERROR(rmt_tx_register_event_callbacks(s_chan, &cbs, NULL), TAG, "rmt cbs");

    rmt_simple_encoder_config_t enc_cfg = {
        .callback = encode_cb,
        .arg = &s_gen,
        .min_chunk_size = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_simple_encoder(&enc_cfg, &s_enc), TAG, "rmt encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "rmt enable");
    return ESP_OK;
}

esp_err_t pulse_gen_start(uint32_t freq_hz, uint32_t duty_pct, uint64_t periods)
{
    if (s_running || freq_hz == 0 || periods == 0 || freq_hz > RMT_RES_HZ / 20) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duty_pct > 100) {
        duty_pct = 100;
    }

    s_gen = (gen_state_t) {
        .freq = freq_hz,
        .duty = duty_pct,
        .period_base = RMT_RES_HZ / freq_hz,
        .period_rem = RMT_RES_HZ % freq_hz,
        .periods_left = periods,
    };

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags.eot_level = 0, /* nível inativo ao terminar */
    };
    s_running = true;
    esp_err_t err = rmt_transmit(s_chan, s_enc, &s_gen, sizeof(s_gen), &tx_cfg);
    if (err != ESP_OK) {
        s_running = false;
        return err;
    }
    ESP_LOGI(TAG, "start: %lu Hz, %lu %%, %llu períodos",
             (unsigned long)freq_hz, (unsigned long)duty_pct, (unsigned long long)periods);
    return ESP_OK;
}

void pulse_gen_stop(void)
{
    if (!s_running) {
        return;
    }
    s_gen.abort = true;
    /* O encoder encerra na próxima recarga; a memória do RMT esvazia em poucos ms */
    if (rmt_tx_wait_all_done(s_chan, 500) != ESP_OK) {
        ESP_LOGW(TAG, "stop forçado");
        rmt_disable(s_chan);
        rmt_enable(s_chan);
    }
    s_running = false;
    ESP_LOGI(TAG, "stop");
}

bool pulse_gen_running(void)
{
    return s_running;
}
