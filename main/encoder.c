#include "encoder.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"

static pcnt_unit_handle_t s_pcnt;
static int s_counts_per_detent;
static bool s_reverse;

void encoder_init(int clk_gpio, int dt_gpio, int counts_per_detent, bool reverse)
{
    s_counts_per_detent = counts_per_detent;
    s_reverse = reverse;

    pcnt_unit_config_t ucfg = {
        .low_limit = -1000,
        .high_limit = 1000,
        .flags.accum_count = 1,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &s_pcnt));

    pcnt_glitch_filter_config_t fcfg = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &fcfg));

    pcnt_chan_config_t a_cfg = { .edge_gpio_num = clk_gpio, .level_gpio_num = dt_gpio };
    pcnt_chan_config_t b_cfg = { .edge_gpio_num = dt_gpio, .level_gpio_num = clk_gpio };
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
    gpio_pullup_en(clk_gpio);
    gpio_pullup_en(dt_gpio);

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));
}

int encoder_read_detents(void)
{
    static int last_raw;
    static int acc;
    int raw = 0;
    pcnt_unit_get_count(s_pcnt, &raw);
    acc += raw - last_raw;
    last_raw = raw;
    int steps = acc / s_counts_per_detent;
    acc -= steps * s_counts_per_detent;
    return s_reverse ? -steps : steps;
}

int encoder_accel(int steps, int64_t now_us)
{
    static int64_t last_us;
    int64_t dt = now_us - last_us;
    last_us = now_us;
    int abs_steps = steps < 0 ? -steps : steps;
    if (abs_steps > 1 || dt < 25000) {
        return 100;
    }
    if (dt < 70000) {
        return 10;
    }
    return 1;
}
