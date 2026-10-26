#include "agent.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "agent_server.h"

static const char *TAG = "agent";

static SemaphoreHandle_t s_mtx;
static bool s_active;
static bool s_failed;

static void lock(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
}

void agent_enter(void)
{
    lock();
    if (!s_active) {
        // Sem Wi-Fi não há pilha de rede: o servidor só sobe se ele ligou
        s_failed = wifi_sta_start() != ESP_OK;
        if (s_failed) {
            ESP_LOGE(TAG, "falha ao ligar o Wi-Fi");
        } else {
            agent_server_start();
        }
        s_active = true;
    }
    xSemaphoreGive(s_mtx);
}

void agent_exit(void)
{
    lock();
    agent_server_stop();
    wifi_sta_stop();
    s_active = false;
    s_failed = false;
    xSemaphoreGive(s_mtx);
}

agent_status_t agent_status(void)
{
    lock();
    agent_status_t st = { .active = s_active, .failed = s_failed, .wifi = wifi_sta_state() };
    strncpy(st.ip, wifi_sta_ip(), sizeof(st.ip) - 1);
    xSemaphoreGive(s_mtx);
    return st;
}
