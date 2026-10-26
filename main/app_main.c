/*
 * Gerador de pulsos - ESP32 DevKitC
 * Pinos e parâmetros do hardware: board.h
 */
#include "nvs_flash.h"

#include "auto_out.h"
#include "automation.h"
#include "board.h"
#include "pulse_gen.h"
#include "settings.h"
#include "ui.h"

void app_main(void)
{
    /* Saídas desligadas o quanto antes (GPIO em 1 = Q1 conduz = saída em 0 V) */
    ESP_ERROR_CHECK(pulse_gen_init(PIN_PULSE_OUT, PULSE_INVERTED));
    ESP_ERROR_CHECK(auto_out_init());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    automation_init();

    ui_start();
}
