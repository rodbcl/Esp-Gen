#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"

/*
 * Registro em anel numa partição de dados da flash. Cada registro tem RINGLOG_DATA bytes
 * de dados, protegidos por sequência e CRC; ao encher, os mais antigos são apagados um
 * setor por vez. Um registro cortado por queda de energia é descartado na leitura.
 * Não é thread-safe: quem usa deve serializar o acesso.
 */

#define RINGLOG_DATA    56

typedef struct {
    const esp_partition_t *part;
    uint32_t slots;
    int32_t head;           // posição do registro mais recente; -1 = vazio
    uint32_t seq;           // sequência do registro mais recente
} ringlog_t;

// Posição de leitura (ver ringlog_newest/prev/next)
typedef struct {
    uint32_t slot;
    uint32_t seq;
} ringlog_pos_t;

esp_err_t ringlog_open(ringlog_t *rl, const char *label);
esp_err_t ringlog_append(ringlog_t *rl, const void *data);

// Leitura: cada função preenche `data` e `pos` e retorna false quando não há registro
bool ringlog_newest(const ringlog_t *rl, ringlog_pos_t *pos, void *data);
// Lê o registro em `pos`; false se ele não existe mais (foi sobrescrito)
bool ringlog_read(const ringlog_t *rl, const ringlog_pos_t *pos, void *data);
bool ringlog_prev(const ringlog_t *rl, ringlog_pos_t *pos, void *data);
bool ringlog_next(const ringlog_t *rl, ringlog_pos_t *pos, void *data);
