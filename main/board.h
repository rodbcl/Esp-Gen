#pragma once

/*
 * Pinos e parâmetros do hardware - ESP32 DevKitC
 *
 * LCD I2C (via level shifter): SDA=GPIO21 SCL=GPIO22
 * Encoder KY-040: CLK=GPIO32 DT=GPIO33 SW=GPIO25
 * Botão START/STOP: GPIO26 (ativo em 0)
 * Saída de pulsos: GPIO27 -> Q1 (BC547) -> push-pull Q2/Q3 -> PULSO (24 V)
 *   O estágio de saída inverte: GPIO27 em 1 = saída desligada.
 */

#define PIN_SDA         21
#define PIN_SCL         22
#define PIN_ENC_CLK     32
#define PIN_ENC_DT      33
#define PIN_ENC_SW      25
#define PIN_BTN_START   26
#define PIN_PULSE_OUT   27
#define PULSE_INVERTED  true    /* Q1 inverte o sinal */

#define LCD_COLS        16      /* ajuste para 20 se o display for 20x4 */
#define LCD_ROWS        2       /* ajuste para 4 se o display for 20x4 */

#define ENC_COUNTS_PER_DETENT 2 /* transições por "clique" (KY-040: 2 ou 4, depende do modelo) */
#define ENC_REVERSE     false   /* troque se o sentido ficar invertido */
