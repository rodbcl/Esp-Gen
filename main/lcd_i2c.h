#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Driver mínimo para LCD HD44780 com backpack I2C PCF8574 (modo 4 bits). */

esp_err_t lcd_init(int sda_gpio, int scl_gpio, uint8_t cols, uint8_t rows);
bool lcd_present(void);
void lcd_clear(void);
void lcd_set_cursor(uint8_t col, uint8_t row);
void lcd_print(const char *s);
void lcd_backlight(bool on);
