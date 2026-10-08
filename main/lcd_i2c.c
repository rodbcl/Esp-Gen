#include "lcd_i2c.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd";

/* Mapeamento padrão do backpack PCF8574: P0=RS P1=RW P2=EN P3=BL P4..P7=D4..D7 */
#define PIN_RS 0x01
#define PIN_EN 0x04
#define PIN_BL 0x08

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static uint8_t s_bl = PIN_BL;
static uint8_t s_cols, s_rows;

static void write_nibble(uint8_t nibble, uint8_t mode)
{
    uint8_t d = (nibble << 4) | mode | s_bl;
    uint8_t buf[2] = { d | PIN_EN, d };
    i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static void write_byte(uint8_t val, uint8_t mode)
{
    uint8_t hi = (val & 0xF0) | mode | s_bl;
    uint8_t lo = ((val << 4) & 0xF0) | mode | s_bl;
    uint8_t buf[4] = { hi | PIN_EN, hi, lo | PIN_EN, lo };
    i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static void cmd(uint8_t c)
{
    write_byte(c, 0);
    if (c <= 0x03) {
        vTaskDelay(pdMS_TO_TICKS(2)); /* clear / home são lentos */
    }
}

esp_err_t lcd_init(int sda_gpio, int scl_gpio, uint8_t cols, uint8_t rows)
{
    s_cols = cols;
    s_rows = rows;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    /* Procura o PCF8574 (0x20-0x27) ou PCF8574A (0x38-0x3F) */
    static const uint8_t candidates[] = { 0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                                          0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E };
    int addr = -1;
    for (size_t i = 0; i < sizeof(candidates); i++) {
        if (i2c_master_probe(s_bus, candidates[i], 50) == ESP_OK) {
            addr = candidates[i];
            break;
        }
    }
    if (addr < 0) {
        ESP_LOGE(TAG, "LCD não encontrado no barramento I2C (SDA=%d SCL=%d)", sda_gpio, scl_gpio);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "LCD encontrado no endereço 0x%02X", addr);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));
    s_present = true;

    /* Sequência de inicialização do HD44780 em 4 bits */
    vTaskDelay(pdMS_TO_TICKS(50));
    write_nibble(0x03, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    write_nibble(0x03, 0);
    esp_rom_delay_us(150);
    write_nibble(0x03, 0);
    esp_rom_delay_us(150);
    write_nibble(0x02, 0);

    cmd(0x28); /* 4 bits, 2 linhas, 5x8 */
    cmd(0x0C); /* display on, cursor off */
    cmd(0x06); /* incremento automático */
    lcd_clear();
    return ESP_OK;
}

bool lcd_present(void)
{
    return s_present;
}

void lcd_clear(void)
{
    if (s_present) {
        cmd(0x01);
    }
}

void lcd_set_cursor(uint8_t col, uint8_t row)
{
    static const uint8_t offsets[] = { 0x00, 0x40, 0x14, 0x54 };
    if (!s_present || row >= s_rows || row > 3) {
        return;
    }
    cmd(0x80 | (offsets[row] + col));
}

void lcd_print(const char *s)
{
    if (!s_present) {
        return;
    }
    while (*s) {
        write_byte((uint8_t)*s++, PIN_RS);
    }
}

void lcd_backlight(bool on)
{
    s_bl = on ? PIN_BL : 0;
    if (s_present) {
        uint8_t d = s_bl;
        i2c_master_transmit(s_dev, &d, 1, 50);
    }
}
