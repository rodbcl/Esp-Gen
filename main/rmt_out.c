#include "rmt_out.h"

#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "rmt_out";

#define RMT_RES_HZ    (1000000UL * RMT_OUT_TICKS_PER_US)
/*
 * Duração máxima de cada meio-símbolo. O limite do hardware é 32767, mas
 * mantê-lo curto garante que a memória do RMT (64 símbolos) esvazie
 * rapidamente quando a geração é interrompida.
 */
#define MAX_HALF_TICKS 2000

struct rmt_out {
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
    volatile bool running;
    // Motor: pede trechos à fonte e os entrega ao RMT em pedaços de até MAX_HALF_TICKS
    rmt_out_source_t src;
    void *ctx;
    uint32_t seg_level;
    uint32_t seg_left;
    volatile bool abort;
};

static rmt_out_t s_pool[RMT_OUT_MAX];
static int s_used;

// Fornece o próximo meio-símbolo (nível, duração). Retorna false quando acabou.
static bool IRAM_ATTR next_half(rmt_out_t *o, uint32_t *level, uint32_t *dur)
{
    while (o->seg_left == 0) {
        if (o->abort || !o->src(o->ctx, &o->seg_level, &o->seg_left)) {
            return false;
        }
    }
    if (o->abort) {
        o->seg_left = 0;
        return false;
    }
    uint32_t chunk = o->seg_left > MAX_HALF_TICKS ? MAX_HALF_TICKS : o->seg_left;
    o->seg_left -= chunk;
    *level = o->seg_level;
    *dur = chunk;
    return true;
}

static size_t IRAM_ATTR encode_cb(const void *data, size_t data_size, size_t symbols_written, size_t symbols_free,
                        rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    rmt_out_t *o = arg;
    size_t n = 0;
    while (n < symbols_free) {
        uint32_t l0, d0, l1, d1;
        if (!next_half(o, &l0, &d0)) {
            *done = true;
            break;
        }
        bool last = false;
        if (!next_half(o, &l1, &d1)) {
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

static bool IRAM_ATTR on_done(rmt_channel_handle_t chan, const rmt_tx_done_event_data_t *ev, void *ctx)
{
    rmt_out_t *o = ctx;
    o->running = false;
    return false;
}

esp_err_t rmt_out_new(int gpio, bool invert, rmt_out_t **out)
{
    if (s_used >= RMT_OUT_MAX) {
        return ESP_ERR_NO_MEM;
    }
    rmt_out_t *o = &s_pool[s_used];
    rmt_tx_channel_config_t cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
        .flags.invert_out = invert,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&cfg, &o->chan), TAG, "rmt channel");

    rmt_tx_event_callbacks_t cbs = { .on_trans_done = on_done };
    ESP_RETURN_ON_ERROR(rmt_tx_register_event_callbacks(o->chan, &cbs, o), TAG, "rmt cbs");

    rmt_simple_encoder_config_t enc_cfg = {
        .callback = encode_cb,
        .arg = o,
        .min_chunk_size = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_simple_encoder(&enc_cfg, &o->enc), TAG, "rmt encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(o->chan), TAG, "rmt enable");
    s_used++;
    *out = o;
    return ESP_OK;
}

esp_err_t rmt_out_start(rmt_out_t *o, rmt_out_source_t src, void *ctx)
{
    if (o->running || src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    o->src = src;
    o->ctx = ctx;
    o->seg_level = 0;
    o->seg_left = 0;
    o->abort = false;

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags.eot_level = 0, /* nível inativo ao terminar */
    };
    o->running = true;
    // O encoder ignora os dados: tudo vem da fonte
    esp_err_t err = rmt_transmit(o->chan, o->enc, o, sizeof(*o), &tx_cfg);
    if (err != ESP_OK) {
        o->running = false;
    }
    return err;
}

void rmt_out_stop(rmt_out_t *o)
{
    if (!o->running) {
        return;
    }
    o->abort = true;
    /* O encoder encerra na próxima recarga; a memória do RMT esvazia em poucos ms */
    if (rmt_tx_wait_all_done(o->chan, 500) != ESP_OK) {
        ESP_LOGW(TAG, "stop forçado");
        rmt_disable(o->chan);
        rmt_enable(o->chan);
    }
    o->running = false;
}

bool rmt_out_running(const rmt_out_t *o)
{
    return o->running;
}
