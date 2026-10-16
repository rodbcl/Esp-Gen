#include "automation.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "pulse_gen.h"
#include "state_log.h"
#include "wallclock.h"
#include "wifi_ap.h"

static const char *TAG = "auto";

#define NVS_NS              "auto"
#define PROFILE_VERSION     1
#define SAVE_PERIOD_MS      1000    // estado gravado a cada segundo enquanto há mudança
#define SAVE_IDLE_S         60      // sem mudança (máquina parada), grava ao menos a cada minuto

typedef struct {
    uint8_t version;
    auto_profile_t p;
} stored_profile_t;

static SemaphoreHandle_t s_mtx;
static auto_profile_t s_prof;
static bool s_active;
static bool s_running;
static bool s_resume;
static state_rec_t s_resume_rec;
static uint32_t s_elapsed_base;     // tempo de teste acumulado até o início desta sessão
static int64_t s_since_us;          // início desta sessão de geração
static uint32_t s_started_at;
static state_rec_t s_saved;         // último estado gravado
static int64_t s_saved_us;

/* ---------- NVS ---------- */
static void load_profile(void)
{
    auto_gen_default(&s_prof);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    stored_profile_t sp;
    size_t len = sizeof(sp);
    if (nvs_get_blob(h, "prof", &sp, &len) == ESP_OK && len == sizeof(sp) && sp.version == PROFILE_VERSION &&
        auto_gen_validate(&sp.p) == NULL) {
        s_prof = sp.p;
    }
    nvs_get_u32(h, "start", &s_started_at);
    nvs_close(h);
}

static void save_profile(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    stored_profile_t sp = { .version = PROFILE_VERSION, .p = s_prof };
    nvs_set_blob(h, "prof", &sp, sizeof(sp));
    nvs_commit(h);
    nvs_close(h);
}

static void save_started_at(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, "start", s_started_at);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------- Estado do teste ---------- */
static uint32_t elapsed_s(void)
{
    if (!s_running) {
        return s_elapsed_base;
    }
    return s_elapsed_base + (uint32_t)((esp_timer_get_time() - s_since_us) / 1000000);
}

static state_rec_t current_rec(void)
{
    tally_t t = tally_get();
    return (state_rec_t) {
        .running = s_running,
        .time_ok = wallclock_confirmed(),
        .stops = t.stops > UINT16_MAX ? UINT16_MAX : t.stops,
        .valid = t.valid,
        .noise = (uint32_t)t.noise,
        .elapsed_s = elapsed_s(),
        .epoch = wallclock_now(),
    };
}

static void save_state(void)
{
    state_rec_t rec = current_rec();
    if (state_log_write(&rec) == ESP_OK) {
        s_saved = rec;
        s_saved_us = esp_timer_get_time();
    } else {
        ESP_LOGW(TAG, "falha ao gravar o estado");
    }
}

static void begin_generation(void)
{
    auto_gen_begin(&s_prof, esp_random());
    if (pulse_gen_start_source(auto_gen_next, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "falha ao iniciar a geração");
        return;
    }
    s_since_us = esp_timer_get_time();
    s_running = true;
}

static void resume_test(void)
{
    const state_rec_t *r = &s_resume_rec;
    // A hora segue a partir do último registro, sem saber quanto tempo ficou desligado
    if (r->epoch) {
        wallclock_set(r->epoch, false);
    }
    tally_t t = { .valid = r->valid, .noise = r->noise, .stops = r->stops };
    tally_reset(&t);
    s_elapsed_base = r->elapsed_s;
    begin_generation();
    s_resume = false;
    ESP_LOGW(TAG, "teste retomado após queda de energia: %llu pulsos válidos", (unsigned long long)r->valid);
    save_state();
}

static void stop_generation(void)
{
    if (!s_running) {
        return;
    }
    pulse_gen_stop();
    s_elapsed_base = elapsed_s();
    s_running = false;
    save_state();
}

// Grava o estado a cada segundo enquanto o teste roda
static void save_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SAVE_PERIOD_MS));
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (s_running) {
            state_rec_t rec = current_rec();
            bool changed = rec.valid != s_saved.valid || rec.noise != s_saved.noise || rec.stops != s_saved.stops;
            if (changed || esp_timer_get_time() - s_saved_us >= SAVE_IDLE_S * 1000000LL) {
                save_state();
            }
        }
        xSemaphoreGive(s_mtx);
    }
}

/* ---------- API ---------- */
void automation_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    load_profile();
    if (state_log_init() == ESP_OK && state_log_last(&s_resume_rec) && s_resume_rec.running) {
        s_resume = true;
    }
    xTaskCreate(save_task, "auto_save", 3072, NULL, 4, NULL);
}

bool automation_resume_pending(void)
{
    return s_resume;
}

void automation_enter(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_active) {
        if (wifi_ap_start() != ESP_OK) {
            ESP_LOGE(TAG, "falha ao ligar o AP");
        }
        s_active = true;
        if (s_resume) {
            resume_test();
        }
    }
    xSemaphoreGive(s_mtx);
}

void automation_exit(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    stop_generation();
    wifi_ap_stop();
    s_active = false;
    xSemaphoreGive(s_mtx);
}

const char *automation_start(void)
{
    const char *err = NULL;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_active) {
        err = "O modo Automação não está ativo.";
    } else if (s_running) {
        err = "Já existe um teste rodando.";
    } else if ((err = auto_gen_validate(&s_prof)) == NULL) {
        tally_reset(NULL);
        s_elapsed_base = 0;
        s_started_at = wallclock_now();
        save_started_at();
        begin_generation();
        if (!s_running) {
            err = "Falha ao iniciar a geração.";
        } else {
            save_state();
            ESP_LOGI(TAG, "teste iniciado");
        }
    }
    xSemaphoreGive(s_mtx);
    return err;
}

void automation_stop(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    stop_generation();
    ESP_LOGI(TAG, "teste interrompido");
    xSemaphoreGive(s_mtx);
}

void automation_get_profile(auto_profile_t *p)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *p = s_prof;
    xSemaphoreGive(s_mtx);
}

const char *automation_set_profile(const auto_profile_t *p)
{
    const char *err = auto_gen_validate(p);
    if (err) {
        return err;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_running) {
        err = "Pare o teste para alterar o perfil.";
    } else {
        s_prof = *p;
        save_profile();
    }
    xSemaphoreGive(s_mtx);
    return err;
}

void automation_set_time(uint32_t epoch)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    wallclock_set(epoch, true);
    xSemaphoreGive(s_mtx);
}

automation_status_t automation_status(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    automation_status_t st = {
        .active = s_active,
        .running = s_running,
        .time_ok = wallclock_confirmed(),
        .tally = tally_get(),
        .elapsed_s = elapsed_s(),
        .started_at = s_started_at,
        .gen = s_running ? auto_gen_status() : (auto_gen_status_t) { 0 },
    };
    xSemaphoreGive(s_mtx);
    return st;
}
