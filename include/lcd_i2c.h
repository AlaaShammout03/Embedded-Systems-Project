#ifndef LCD_I2C_H
#define LCD_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c.h"
#include "esp_err.h"

typedef struct {
    i2c_port_t i2c_port;
    int        sda_gpio;
    int        scl_gpio;
    uint8_t    address;
    uint8_t    cols;
    uint8_t    rows;
    uint32_t   clk_speed_hz;
    bool       backlight;
} lcd_i2c_config_t;

typedef struct {
    i2c_port_t port;
    uint8_t    address;
    uint8_t    cols;
    uint8_t    rows;
    uint8_t    backlight_val;
} lcd_i2c_t;

esp_err_t lcd_i2c_init(lcd_i2c_t *lcd, const lcd_i2c_config_t *cfg);
void      lcd_i2c_clear(lcd_i2c_t *lcd);
void      lcd_i2c_set_cursor(lcd_i2c_t *lcd, uint8_t col, uint8_t row);
void      lcd_i2c_write_str(lcd_i2c_t *lcd, const char *str);
void      lcd_i2c_write_char(lcd_i2c_t *lcd, char c);

#endif // LCD_I2C_H
