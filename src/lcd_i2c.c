#include "lcd_i2c.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// PCF8574 bit masks */
#define LCD_RS  0x01
#define LCD_RW  0x02
#define LCD_EN  0x04
#define LCD_BL  0x08

// HD44780 commands
#define CMD_CLEAR      0x01
#define CMD_HOME       0x02
#define CMD_ENTRY      0x06  // cursor increment, no display shift
#define CMD_DISPLAY_ON 0x0C  // display on, cursor off, blink off
#define CMD_4BIT_2LINE 0x28  // 4-bit bus, 2 lines, 5x8 font

// Row address offsets for HD44780
static const uint8_t ROW_OFFSETS[4] = {0x00, 0x40, 0x14, 0x54};

// low-level I2C write */
static void i2c_write_byte(lcd_i2c_t *lcd, uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (lcd->address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(lcd->port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
}

// pulse the Enable pin
static void pulse_enable(lcd_i2c_t *lcd, uint8_t data)
{
    i2c_write_byte(lcd, data | LCD_EN);
    vTaskDelay(pdMS_TO_TICKS(1));
    i2c_write_byte(lcd, data & ~LCD_EN);
    vTaskDelay(pdMS_TO_TICKS(1));
}

// send one 4-bit nibble
static void write_nibble(lcd_i2c_t *lcd, uint8_t nibble, uint8_t rs)
{
    uint8_t data = (uint8_t)((nibble << 4) | lcd->backlight_val | (rs ? LCD_RS : 0));
    pulse_enable(lcd, data);
}

// send a full byte
static void send_byte(lcd_i2c_t *lcd, uint8_t byte, uint8_t rs)
{
    write_nibble(lcd, byte >> 4,   rs);  // high nibble first
    write_nibble(lcd, byte & 0x0F, rs);  // then low nibble
}

static void lcd_cmd(lcd_i2c_t *lcd, uint8_t cmd)  { send_byte(lcd, cmd, 0); }
static void lcd_dat(lcd_i2c_t *lcd, uint8_t data) { send_byte(lcd, data, 1); }

 //Public API
esp_err_t lcd_i2c_init(lcd_i2c_t *lcd, const lcd_i2c_config_t *cfg)
{
    esp_err_t ret;

    lcd->port          = cfg->i2c_port;
    lcd->address       = cfg->address;
    lcd->cols          = cfg->cols;
    lcd->rows          = cfg->rows;
    lcd->backlight_val = cfg->backlight ? LCD_BL : 0;

    // Configure and install I2C driver
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = cfg->sda_gpio,
        .scl_io_num       = cfg->scl_gpio,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = cfg->clk_speed_hz,
    };
    ret = i2c_param_config(cfg->i2c_port, &conf);
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(cfg->i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) return ret;

    // HD44780 4-bit initialisation sequence
    vTaskDelay(pdMS_TO_TICKS(50));

    write_nibble(lcd, 0x03, 0); vTaskDelay(pdMS_TO_TICKS(5));
    write_nibble(lcd, 0x03, 0); vTaskDelay(pdMS_TO_TICKS(1));
    write_nibble(lcd, 0x03, 0); vTaskDelay(pdMS_TO_TICKS(1));
    write_nibble(lcd, 0x02, 0); vTaskDelay(pdMS_TO_TICKS(1));

    lcd_cmd(lcd, CMD_4BIT_2LINE);
    lcd_cmd(lcd, CMD_DISPLAY_ON);
    lcd_cmd(lcd, CMD_CLEAR);     vTaskDelay(pdMS_TO_TICKS(2));
    lcd_cmd(lcd, CMD_ENTRY);

    return ESP_OK;
}

void lcd_i2c_clear(lcd_i2c_t *lcd)
{
    lcd_cmd(lcd, CMD_CLEAR);
    vTaskDelay(pdMS_TO_TICKS(2));
}

void lcd_i2c_set_cursor(lcd_i2c_t *lcd, uint8_t col, uint8_t row)
{
    lcd_cmd(lcd, (uint8_t)(0x80 | (col + ROW_OFFSETS[row])));
}

void lcd_i2c_write_str(lcd_i2c_t *lcd, const char *str)
{
    while (*str) {
        lcd_dat(lcd, (uint8_t)*str++);
    }
}

void lcd_i2c_write_char(lcd_i2c_t *lcd, char c)
{
    lcd_dat(lcd, (uint8_t)c);
}
