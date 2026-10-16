#include "screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "lcd_i2c.h"

static char s_screen[LCD_ROWS][LCD_COLS + 1];
static char s_shadow[LCD_ROWS][LCD_COLS + 1];

void screen_begin(void)
{
    for (int r = 0; r < LCD_ROWS; r++) {
        s_screen[r][0] = 0;
    }
}

void screen_printf(int row, const char *fmt, ...)
{
    if (row < 0 || row >= LCD_ROWS) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_screen[row], LCD_COLS + 1, fmt, ap);
    va_end(ap);
}

void screen_item(int row, char mark, const char *label, const char *value)
{
    int pad = LCD_COLS - 1 - (int)strlen(label) - (int)strlen(value);
    if (pad < 1) {
        pad = 1;
    }
    screen_printf(row, "%c%s%*s%s", mark, label, pad, "", value);
}

void screen_flush(void)
{
    for (int r = 0; r < LCD_ROWS; r++) {
        size_t len = strlen(s_screen[r]);
        memset(s_screen[r] + len, ' ', LCD_COLS - len);
        s_screen[r][LCD_COLS] = 0;
        if (strcmp(s_screen[r], s_shadow[r]) != 0) {
            lcd_set_cursor(0, r);
            lcd_print(s_screen[r]);
            strcpy(s_shadow[r], s_screen[r]);
        }
    }
}
