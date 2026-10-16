#include "state_log.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "state";

#define PART_LABEL  "autostate"
#define SECTOR      4096

// Registro gravado na flash: 32 bytes, 128 por setor
typedef struct __attribute__((packed)) {
    uint32_t seq;           // 0xFFFFFFFF = vazio
    uint16_t flags;
    uint16_t stops;
    uint64_t valid;
    uint32_t noise;
    uint32_t elapsed_s;
    uint32_t epoch;
    uint32_t crc;
} raw_t;

_Static_assert(sizeof(raw_t) == 32, "raw_t deve ter 32 bytes");

#define FLAG_RUNNING    0x0001
#define FLAG_TIME_OK    0x0002

static const esp_partition_t *s_part;
static uint32_t s_slots;
static int32_t s_head = -1;     // último registro válido
static uint32_t s_seq;
static raw_t s_last;

static uint32_t rec_crc(const raw_t *r)
{
    return esp_rom_crc32_le(0, (const uint8_t *)r, offsetof(raw_t, crc));
}

static bool is_erased(const raw_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    for (size_t i = 0; i < sizeof(*r); i++) {
        if (p[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

esp_err_t state_log_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    if (!s_part) {
        ESP_LOGE(TAG, "partição '%s' não encontrada", PART_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    s_slots = s_part->size / sizeof(raw_t);

    // Procura o registro válido de maior sequência
    for (uint32_t i = 0; i < s_slots; i++) {
        raw_t r;
        ESP_RETURN_ON_ERROR(esp_partition_read(s_part, i * sizeof(raw_t), &r, sizeof(r)), TAG, "read");
        if (r.seq != 0xFFFFFFFF && r.crc == rec_crc(&r) && (s_head < 0 || r.seq > s_seq)) {
            s_head = i;
            s_seq = r.seq;
            s_last = r;
        }
    }
    if (s_head < 0) {
        // Partição nova ou com lixo: começa do zero
        ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_part, 0, s_part->size), TAG, "erase");
        ESP_LOGI(TAG, "registro de estado vazio");
    } else {
        ESP_LOGI(TAG, "último estado: seq %lu, %s", (unsigned long)s_seq,
                 (s_last.flags & FLAG_RUNNING) ? "teste rodando" : "sem teste");
    }
    return ESP_OK;
}

bool state_log_last(state_rec_t *rec)
{
    if (s_head < 0) {
        return false;
    }
    *rec = (state_rec_t) {
        .running = s_last.flags & FLAG_RUNNING,
        .time_ok = s_last.flags & FLAG_TIME_OK,
        .stops = s_last.stops,
        .valid = s_last.valid,
        .noise = s_last.noise,
        .elapsed_s = s_last.elapsed_s,
        .epoch = s_last.epoch,
    };
    return true;
}

esp_err_t state_log_write(const state_rec_t *rec)
{
    if (!s_part) {
        return ESP_ERR_INVALID_STATE;
    }
    raw_t r = {
        .seq = s_seq + 1,
        .flags = (rec->running ? FLAG_RUNNING : 0) | (rec->time_ok ? FLAG_TIME_OK : 0),
        .stops = rec->stops,
        .valid = rec->valid,
        .noise = rec->noise,
        .elapsed_s = rec->elapsed_s,
        .epoch = rec->epoch,
    };
    r.crc = rec_crc(&r);

    // Próximo espaço livre; um registro cortado por queda de energia é pulado
    uint32_t slot = (uint32_t)(s_head + 1) % s_slots;
    for (uint32_t tries = 0; tries < s_slots; tries++, slot = (slot + 1) % s_slots) {
        uint32_t addr = slot * sizeof(raw_t);
        if (addr % SECTOR == 0) {
            ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_part, addr, SECTOR), TAG, "erase");
        } else {
            raw_t cur;
            ESP_RETURN_ON_ERROR(esp_partition_read(s_part, addr, &cur, sizeof(cur)), TAG, "read");
            if (!is_erased(&cur)) {
                continue;
            }
        }
        ESP_RETURN_ON_ERROR(esp_partition_write(s_part, addr, &r, sizeof(r)), TAG, "write");
        s_head = slot;
        s_seq = r.seq;
        s_last = r;
        return ESP_OK;
    }
    return ESP_FAIL;
}
