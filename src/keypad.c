#include "keypad.h"
#include "pins.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ROWS 4
#define COLS 4

static const int row_pins[ROWS] = {ROW1_PIN, ROW2_PIN, ROW3_PIN, ROW4_PIN};
static const int col_pins[COLS] = {COL1_PIN, COL2_PIN, COL3_PIN, COL4_PIN};

static const char key_map[ROWS][COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

void keypad_init(void)
{
    int i;
    for (i = 0; i < ROWS; i++) {
        gpio_reset_pin(row_pins[i]);
        gpio_set_direction(row_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(row_pins[i], 1);
    }
    for (i = 0; i < COLS; i++) {
        gpio_reset_pin(col_pins[i]);
        gpio_set_direction(col_pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(col_pins[i], GPIO_PULLUP_ONLY);
    }
}

char keypad_get_key(void)
{
    int r, c;
    for (r = 0; r < ROWS; r++) {
        gpio_set_level(row_pins[r], 0); // drive row LOW
        vTaskDelay(pdMS_TO_TICKS(5)); // settle time

        for (c = 0; c < COLS; c++) {
            if (gpio_get_level(col_pins[c]) == 0) {
                gpio_set_level(row_pins[r], 1); // restore row
                return key_map[r][c];
            }
        }

        gpio_set_level(row_pins[r], 1); // restore row
    }
    return '\0';
}
