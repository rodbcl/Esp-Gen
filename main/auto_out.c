#include "auto_out.h"

#include "esp_check.h"
#include "board.h"
#include "rmt_out.h"

static const char *TAG = "auto_out";

static rmt_out_t *s_running;
static rmt_out_t *s_quality;
static rmt_out_t *s_report;

esp_err_t auto_out_init(void)
{
    ESP_RETURN_ON_ERROR(rmt_out_new(PIN_RUNNING_OUT, OUT_INVERTED, &s_running), TAG, "running");
    ESP_RETURN_ON_ERROR(rmt_out_new(PIN_QUALITY_OUT, OUT_INVERTED, &s_quality), TAG, "qualidade");
    ESP_RETURN_ON_ERROR(rmt_out_new(PIN_REPORT_OUT, OUT_INVERTED, &s_report), TAG, "stop report");
    return ESP_OK;
}

esp_err_t auto_out_start(const auto_profile_t *p)
{
    esp_err_t err = ESP_OK;
    if (p->running_enabled && err == ESP_OK) {
        err = rmt_out_start(s_running, auto_gen_next_running, NULL);
    }
    if (p->quality_enabled && err == ESP_OK) {
        err = rmt_out_start(s_quality, auto_gen_next_quality, NULL);
    }
    if (p->report_enabled && err == ESP_OK) {
        err = rmt_out_start(s_report, auto_gen_next_report, NULL);
    }
    if (err != ESP_OK) {
        auto_out_stop();
    }
    return err;
}

void auto_out_stop(void)
{
    rmt_out_stop(s_running);
    rmt_out_stop(s_quality);
    rmt_out_stop(s_report);
}
