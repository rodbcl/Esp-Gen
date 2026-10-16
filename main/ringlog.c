#include "ringlog.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

static const char *TAG = "ringlog";

#define SECTOR      4096
#define EMPTY_SEQ   0xFFFFFFFF

// Registro gravado na flash: 32 bytes, 128 por setor
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint8_t data[RINGLOG_DATA];
    uint32_t crc;
} raw_t;

_Static_assert(sizeof(raw_t) == 32, "raw_t deve ter 32 bytes");

typedef enum { REC_OK, REC_EMPTY, REC_BAD } rec_state_t;

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

static rec_state_t read_slot(const ringlog_t *rl, uint32_t slot, raw_t *r)
{
    if (esp_partition_read(rl->part, slot * sizeof(raw_t), r, sizeof(*r)) != ESP_OK) {
        return REC_BAD;
    }
    if (is_erased(r)) {
        return REC_EMPTY;
    }
    return r->seq != EMPTY_SEQ && r->crc == rec_crc(r) ? REC_OK : REC_BAD;
}

esp_err_t ringlog_open(ringlog_t *rl, const char *label)
{
    *rl = (ringlog_t) { .head = -1 };
    rl->part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!rl->part) {
        ESP_LOGE(TAG, "partição '%s' não encontrada", label);
        return ESP_ERR_NOT_FOUND;
    }
    rl->slots = rl->part->size / sizeof(raw_t);

    // O registro válido de maior sequência é o mais recente
    for (uint32_t i = 0; i < rl->slots; i++) {
        raw_t r;
        if (read_slot(rl, i, &r) == REC_OK && (rl->head < 0 || r.seq > rl->seq)) {
            rl->head = i;
            rl->seq = r.seq;
        }
    }
    if (rl->head < 0) {
        // Partição nova ou com lixo: começa do zero
        ESP_RETURN_ON_ERROR(esp_partition_erase_range(rl->part, 0, rl->part->size), TAG, "erase");
        ESP_LOGI(TAG, "%s: vazio", label);
    } else {
        ESP_LOGI(TAG, "%s: último registro seq %lu", label, (unsigned long)rl->seq);
    }
    return ESP_OK;
}

esp_err_t ringlog_append(ringlog_t *rl, const void *data)
{
    if (!rl->part) {
        return ESP_ERR_INVALID_STATE;
    }
    raw_t r = { .seq = rl->head < 0 ? 0 : rl->seq + 1 };
    memcpy(r.data, data, RINGLOG_DATA);
    r.crc = rec_crc(&r);

    // Próximo espaço livre; ao entrar num setor ele é apagado inteiro
    uint32_t slot = (uint32_t)(rl->head + 1) % rl->slots;
    for (uint32_t tries = 0; tries < rl->slots; tries++, slot = (slot + 1) % rl->slots) {
        uint32_t addr = slot * sizeof(raw_t);
        if (addr % SECTOR == 0) {
            ESP_RETURN_ON_ERROR(esp_partition_erase_range(rl->part, addr, SECTOR), TAG, "erase");
        } else {
            raw_t cur;
            if (read_slot(rl, slot, &cur) != REC_EMPTY) {
                continue;   // registro cortado por queda de energia: pula
            }
        }
        ESP_RETURN_ON_ERROR(esp_partition_write(rl->part, addr, &r, sizeof(r)), TAG, "write");
        rl->head = slot;
        rl->seq = r.seq;
        return ESP_OK;
    }
    return ESP_FAIL;
}

bool ringlog_newest(const ringlog_t *rl, ringlog_pos_t *pos, void *data)
{
    raw_t r;
    if (rl->head < 0 || read_slot(rl, rl->head, &r) != REC_OK) {
        return false;
    }
    *pos = (ringlog_pos_t) { .slot = rl->head, .seq = r.seq };
    memcpy(data, r.data, RINGLOG_DATA);
    return true;
}

bool ringlog_read(const ringlog_t *rl, const ringlog_pos_t *pos, void *data)
{
    raw_t r;
    if (pos->slot >= rl->slots || read_slot(rl, pos->slot, &r) != REC_OK || r.seq != pos->seq) {
        return false;
    }
    memcpy(data, r.data, RINGLOG_DATA);
    return true;
}

bool ringlog_prev(const ringlog_t *rl, ringlog_pos_t *pos, void *data)
{
    uint32_t slot = pos->slot;
    for (uint32_t tries = 1; tries < rl->slots; tries++) {
        slot = (slot + rl->slots - 1) % rl->slots;
        raw_t r;
        rec_state_t st = read_slot(rl, slot, &r);
        if (st == REC_EMPTY || (st == REC_OK && r.seq >= pos->seq)) {
            return false;   // início dos dados (setor apagado ou já deu a volta)
        }
        if (st == REC_OK) {
            *pos = (ringlog_pos_t) { .slot = slot, .seq = r.seq };
            memcpy(data, r.data, RINGLOG_DATA);
            return true;
        }
    }
    return false;
}

bool ringlog_next(const ringlog_t *rl, ringlog_pos_t *pos, void *data)
{
    uint32_t slot = pos->slot;
    while (slot != (uint32_t)rl->head) {
        slot = (slot + 1) % rl->slots;
        raw_t r;
        if (read_slot(rl, slot, &r) == REC_OK && r.seq > pos->seq) {
            *pos = (ringlog_pos_t) { .slot = slot, .seq = r.seq };
            memcpy(data, r.data, RINGLOG_DATA);
            return true;
        }
    }
    return false;
}
