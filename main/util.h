#pragma once

#include <stdint.h>
#include <stdio.h>

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Frequência em décimos de Hz como texto: "0,5Hz", "12Hz"
static inline void fmt_freq(char *buf, size_t n, uint32_t dhz)
{
    if (dhz % 10) {
        snprintf(buf, n, "%lu,%luHz", (unsigned long)(dhz / 10), (unsigned long)(dhz % 10));
    } else {
        snprintf(buf, n, "%luHz", (unsigned long)(dhz / 10));
    }
}
