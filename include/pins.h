#ifndef PINS_H
#define PINS_H

// LCD I2C 
#define LCD_SDA 21
#define LCD_SCL 22

// Buzzer 
#define BUZZER_PIN 4

// LEDs 
#define RED_LED_PIN   17
#define GREEN_LED_PIN 23
#define BLUE_LED_PIN  16

// Buttons: one start/reset button plus three Stage 3 color buttons 
#define START_BTN_PIN 18   // start/reset button 
#define REACT_BTN_PIN 19   // blue button        
#define BTN_A_PIN      2   // green button       
#define BTN_B_PIN      5   // red button         

// Joystick GPIO 
#define JOY_X_PIN   35
#define JOY_Y_PIN   34
#define JOY_SEL_PIN 32

//Joystick ADC channels (ESP-IDF ADC1)
//GPIO34 --> ADC1_CHANNEL_6
//GPIO35 --> ADC1_CHANNEL_7
 
#define JOY_X_CH 7
#define JOY_Y_CH 6

// Slide potentiometer: GPIO39 (VN) --> ADC1_CHANNEL_3 
#define POT_PIN 39
#define POT_CH   3

// Keypad rows (outputs) 
#define ROW1_PIN 13
#define ROW2_PIN 12
#define ROW3_PIN 14
#define ROW4_PIN 15

// Keypad columns (inputs with pull-up) 
#define COL1_PIN 27
#define COL2_PIN 26
#define COL3_PIN 25
#define COL4_PIN 33

#endif // PINS_H 

