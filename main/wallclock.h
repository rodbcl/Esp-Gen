#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Hora real (epoch Unix, em segundos). O ESP não tem RTC: a hora vem do navegador e
 * é mantida pelo esp_timer. Após uma queda de energia ela é estimada e fica
 * "não confirmada" até o navegador enviá-la de novo.
 */

// Define a hora atual; `confirmed` = veio de fonte confiável (navegador)
void wallclock_set(uint32_t epoch, bool confirmed);
// Hora atual, ou 0 se nunca foi definida
uint32_t wallclock_now(void);
bool wallclock_confirmed(void);

// Fuso do navegador, em minutos em relação ao UTC (Brasília = -180)
void wallclock_set_tz(int32_t offset_min);
// Escreve `epoch` em hora local como "dd/mm/aaaa hh:mm"
void wallclock_format(uint32_t epoch, char *buf, int len);
