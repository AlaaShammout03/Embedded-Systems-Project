#include "game_tasks.h"
#include "lcd_i2c.h"
#include "keypad.h"
#include "pins.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/ledc.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "GAME";

// LCD instance 
static lcd_i2c_t lcd;

// Shared State 
static GameState         gameState;
static SemaphoreHandle_t gameMutex = NULL;

typedef enum {
    FAIL_NONE,
    FAIL_STAGE,
    FAIL_GAME_TIMEOUT
} FailureReason;

#define GAME_TIME_MS 120000u

static TickType_t   gameStartedAt = 0;
static uint32_t     gamePausedElapsedMs = 0;
static bool         gameTimerRunning = false;
static uint32_t     gameFinishedElapsedMs = 0;
static FailureReason failureReason = FAIL_NONE;

// Stage 1: slider puzzle 
typedef enum {
    S1_REVEAL_DIGITS,
    S1_ENTER_CODE
} Stage1Phase;

static const uint8_t S1_TARGETS_PCT[4] = {70, 25, 85, 40};
static const char    S1_DIGITS[5]      = "2580";
#define S1_TOLERANCE_PCT 4u
#define S1_HOLD_MS       800u
#define S1_DIGIT_TIME_MS 10000u
#define S1_ADC_SAMPLES   8u

static int        potValue    = 0;   // latest ADC reading, 0–4095  
static uint8_t    s1Step      = 0;   // revealed digits so far 
static bool       s1InZone    = false;
static TickType_t s1HoldStart = 0;

// Stage 1 code submission 
static char submittedCode[5] = {0};
static bool codeSubmitted    = false;
static uint8_t     potPercent        = 0;   // averaged pot position, 0-100 
static Stage1Phase s1Phase           = S1_REVEAL_DIGITS;
static TickType_t  s1DigitStartedAt  = 0;
static char        s1RevealedCode[5] = {0};
static uint8_t     s1KeypadLength    = 0;

// Stage 2 
typedef enum {
    DIR_NONE,
    DIR_UP,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT
} JoyDirection;

static JoyDirection pendingDirection = DIR_NONE;
static bool         directionReady   = false;

typedef struct {
    JoyDirection displayDir;
    JoyDirection expectedDir;
    bool         reverse;
} Stage2Command;

#define STAGE2_STEPS          5u
#define STAGE2_TIME_MS        3000u
#define STAGE2_REVERSE_COUNT  2u

static Stage2Command stage2Sequence[STAGE2_STEPS];

// Stage 3 
typedef enum {
    S3_COLOR_NONE,
    S3_COLOR_RED,
    S3_COLOR_GREEN,
    S3_COLOR_BLUE
} Stage3Color;

typedef enum {
    S3_STATUS_ACTIVE,
    S3_STATUS_CORRECT,
    S3_STATUS_WRONG,
    S3_STATUS_TIMEOUT
} Stage3Status;

static TickType_t  stage2StartedAt = 0;
static Stage3Color s3TargetColor   = S3_COLOR_NONE;
static Stage3Color s3PressedColor  = S3_COLOR_NONE;
static bool        s3ButtonReady   = false;
static TickType_t  s3RoundStartedAt = 0;
static Stage3Status s3Status       = S3_STATUS_ACTIVE;

#define STAGE3_ROUNDS            5
#define S3_ROUND_TIME_MS         1000u
#define S3_BUTTON_RELEASE_MS     40u

// Task Handles 
static TaskHandle_t inputTaskHandle   = NULL;
static TaskHandle_t logicTaskHandle   = NULL;
static TaskHandle_t displayTaskHandle = NULL;

//Helper Functions
static void clearEnteredCode(void)
{
    memset(gameState.enteredCode, 0, sizeof(gameState.enteredCode));
    gameState.enteredLength = 0;
}

static uint32_t gameElapsedMs(TickType_t now);

static uint32_t gameRemainingMs(TickType_t now)
{
    uint32_t elapsed_ms;

    elapsed_ms = gameElapsedMs(now);
    if (elapsed_ms >= GAME_TIME_MS) {
        return 0;
    }
    return GAME_TIME_MS - elapsed_ms;
}

static uint32_t gameElapsedMs(TickType_t now)
{
    uint32_t elapsed_ms = gamePausedElapsedMs;

    if (gameTimerRunning && gameStartedAt != 0) {
        elapsed_ms += (uint32_t)((now - gameStartedAt) * portTICK_PERIOD_MS);
    }

    return elapsed_ms;
}

static void resetGameTimer(void)
{
    gameStartedAt = 0;
    gamePausedElapsedMs = 0;
    gameTimerRunning = false;
}

static void pauseGameTimer(TickType_t now)
{
    if (gameTimerRunning) {
        gamePausedElapsedMs = gameElapsedMs(now);
        gameStartedAt = 0;
        gameTimerRunning = false;
    }
}

static void resumeGameTimer(TickType_t now)
{
    if (!gameTimerRunning) {
        gameStartedAt = now;
        gameTimerRunning = true;
    }
}

static bool useStageAttempt(void)
{
    if (gameState.attemptsLeft > 0) {
        gameState.attemptsLeft--;
    }

    if (gameState.attemptsLeft <= 0) {
        gameState.stage       = GAME_FAILURE;
        gameState.gameStarted = false;
        pauseGameTimer(xTaskGetTickCount());
        failureReason         = FAIL_STAGE;
        return false;
    }

    return true;
}

static void formatTimeMMSS(uint32_t ms, char *out, size_t out_size)
{
    uint32_t total_s = (ms + 999u) / 1000u;
    snprintf(out, out_size, "%02lu:%02lu",
             (unsigned long)(total_s / 60u),
             (unsigned long)(total_s % 60u));
}

static uint8_t adcToPercent(int raw)
{
    int pct = (raw * 100 + 2047) / 4095;
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return (uint8_t)pct;
}

static int readAveragedPotRaw(void)
{
    uint32_t total = 0;
    uint8_t i;

    for (i = 0; i < S1_ADC_SAMPLES; i++) {
        total += (uint32_t)adc1_get_raw((adc1_channel_t)POT_CH);
    }

    return (int)(total / S1_ADC_SAMPLES);
}

static void clearSubmittedCode(void)
{
    memset(submittedCode, 0, sizeof(submittedCode));
    s1KeypadLength = 0;
    codeSubmitted  = false;
}

static void resetStage1Progress(TickType_t now)
{
    s1Phase          = S1_REVEAL_DIGITS;
    s1Step           = 0;
    s1InZone         = false;
    s1HoldStart      = 0;
    s1DigitStartedAt = now;
    memset(s1RevealedCode, 0, sizeof(s1RevealedCode));
    clearSubmittedCode();
    clearEnteredCode();
}

static void buzzerTone(uint32_t freq_hz)
{
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 512); 
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void buzzerOff(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void resetOutputs(void)
{
    gpio_set_level(RED_LED_PIN,   0);
    gpio_set_level(GREEN_LED_PIN, 0);
    gpio_set_level(BLUE_LED_PIN,  0);
    buzzerOff();
}

static void resetToIdle(void)
{
    gameState.stage           = GAME_IDLE;
    gameState.gameStarted     = false;
    gameState.stage1Unlocked  = false;
    gameState.stage2Unlocked  = false;
    gameState.attemptsLeft    = 3;
    gameState.stage2Progress  = 0;
    gameState.stage3Successes = 0;
    resetGameTimer();
    gameFinishedElapsedMs     = 0;
    failureReason             = FAIL_NONE;
    clearEnteredCode();
    clearSubmittedCode();
    memset(s1RevealedCode, 0, sizeof(s1RevealedCode));
    pendingDirection = DIR_NONE;
    directionReady   = false;
    stage2StartedAt  = 0;
    s3TargetColor    = S3_COLOR_NONE;
    s3PressedColor   = S3_COLOR_NONE;
    s3ButtonReady    = false;
    s3RoundStartedAt = 0;
    s3Status         = S3_STATUS_ACTIVE;
    potValue         = 0;
    potPercent       = 0;
    s1Phase          = S1_REVEAL_DIGITS;
    s1Step           = 0;
    s1InZone         = false;
    s1HoldStart      = 0;
    s1DigitStartedAt = 0;
}

static bool isStartPressed(void)
{
    return gpio_get_level(START_BTN_PIN) == 0;
}

static void shortBeep(uint32_t ms)
{
    buzzerTone(2000);
    vTaskDelay(pdMS_TO_TICKS(ms));
    buzzerOff();
}

static void successFeedback(void)
{
    resetOutputs();
    gpio_set_level(GREEN_LED_PIN, 1);
    buzzerTone(1200);
    vTaskDelay(pdMS_TO_TICKS(120));
    buzzerOff();
    vTaskDelay(pdMS_TO_TICKS(60));
    buzzerTone(1800);
    vTaskDelay(pdMS_TO_TICKS(180));
    buzzerOff();
}

static void victoryMusic(void)
{
    static const uint16_t melody[] = {
        523, 659, 784, 1047, 784, 1047, 1319
    };
    static const uint16_t durations_ms[] = {
        120, 120, 120, 220, 120, 160, 320
    };
    uint8_t i;

    resetOutputs();
    gpio_set_level(RED_LED_PIN, 1);
    gpio_set_level(GREEN_LED_PIN, 1);
    gpio_set_level(BLUE_LED_PIN, 1);

    for (i = 0; i < sizeof(melody) / sizeof(melody[0]); i++) {
        buzzerTone(melody[i]);
        vTaskDelay(pdMS_TO_TICKS(durations_ms[i]));
        buzzerOff();
        vTaskDelay(pdMS_TO_TICKS(45));
    }
}

static void errorFeedback(void)
{
    resetOutputs();
    gpio_set_level(RED_LED_PIN, 1);
    buzzerTone(350);
    vTaskDelay(pdMS_TO_TICKS(300));
    buzzerOff();
}

static JoyDirection readJoystickDirection(void)
{
    int x = adc1_get_raw((adc1_channel_t)JOY_X_CH);
    int y = adc1_get_raw((adc1_channel_t)JOY_Y_CH);

    const int LOW_TH  = 1200;
    const int HIGH_TH = 2800;

    if (y < LOW_TH)  return DIR_UP;
    if (y > HIGH_TH) return DIR_DOWN;
    if (x < LOW_TH)  return DIR_LEFT;
    if (x > HIGH_TH) return DIR_RIGHT;
    return DIR_NONE;
}

static bool joystickCentered(void)
{
    int x = adc1_get_raw((adc1_channel_t)JOY_X_CH);
    int y = adc1_get_raw((adc1_channel_t)JOY_Y_CH);
    return (x > 1700 && x < 2400 && y > 1700 && y < 2400);
}

static const char *directionToText(JoyDirection dir)
{
    switch (dir) {
        case DIR_UP:    return "UP";
        case DIR_DOWN:  return "DOWN";
        case DIR_LEFT:  return "LEFT";
        case DIR_RIGHT: return "RIGHT";
        default:        return "NONE";
    }
}

static JoyDirection oppositeDirection(JoyDirection dir)
{
    switch (dir) {
        case DIR_UP:    return DIR_DOWN;
        case DIR_DOWN:  return DIR_UP;
        case DIR_LEFT:  return DIR_RIGHT;
        case DIR_RIGHT: return DIR_LEFT;
        default:        return DIR_NONE;
    }
}

static JoyDirection randomDirection(void)
{
    return (JoyDirection)((esp_random() % 4u) + 1u);
}

static bool stage2ShouldReverse(uint8_t index, const bool reverseSlots[STAGE2_STEPS])
{
    return reverseSlots[index];
}

static void stage2InstructionText(uint8_t index, char *out, size_t out_size)
{
    if (index >= STAGE2_STEPS) {
        snprintf(out, out_size, "DONE");
        return;
    }

    if (stage2Sequence[index].reverse) {
        snprintf(out, out_size, "REVERSE %s",
                 directionToText(stage2Sequence[index].displayDir));
    } else {
        snprintf(out, out_size, "%s",
                 directionToText(stage2Sequence[index].displayDir));
    }
}

static void generateStage2Sequence(void)
{
    bool reverseSlots[STAGE2_STEPS] = {false};
    uint8_t reverseCount = 0;
    uint8_t i;

    while (reverseCount < STAGE2_REVERSE_COUNT) {
        uint8_t slot = (uint8_t)(esp_random() % STAGE2_STEPS);
        if (!reverseSlots[slot]) {
            reverseSlots[slot] = true;
            reverseCount++;
        }
    }

    for (i = 0; i < STAGE2_STEPS; i++) {
        JoyDirection dir = randomDirection();
        uint8_t guard = 0;

        while (i > 0 && dir == stage2Sequence[i - 1].displayDir && guard < 8) {
            dir = randomDirection();
            guard++;
        }

        stage2Sequence[i].displayDir  = dir;
        stage2Sequence[i].reverse     = stage2ShouldReverse(i, reverseSlots);
        stage2Sequence[i].expectedDir = stage2Sequence[i].reverse
                                          ? oppositeDirection(dir)
                                          : dir;
    }
}


static const char *stage3ColorToText(Stage3Color color)
{
    switch (color) {
        case S3_COLOR_RED:   return "RED";
        case S3_COLOR_GREEN: return "GREEN";
        case S3_COLOR_BLUE:  return "BLUE";
        default:             return "NONE";
    }
}

static void setStage3Led(Stage3Color color)
{
    gpio_set_level(RED_LED_PIN,   color == S3_COLOR_RED);
    gpio_set_level(GREEN_LED_PIN, color == S3_COLOR_GREEN);
    gpio_set_level(BLUE_LED_PIN,  color == S3_COLOR_BLUE);
}

static Stage3Color randomStage3Color(void)
{
    return (Stage3Color)((esp_random() % 3u) + 1u);
}

static void resetStage2Progress(TickType_t now)
{
    gameState.stage2Progress = 0;
    pendingDirection         = DIR_NONE;
    directionReady           = false;
    stage2StartedAt         = now;
    generateStage2Sequence();
}

static void startStage3Round(TickType_t now)
{
    s3TargetColor    = randomStage3Color();
    s3PressedColor   = S3_COLOR_NONE;
    s3ButtonReady    = false;
    s3RoundStartedAt = now;
    s3Status         = S3_STATUS_ACTIVE;
    setStage3Led(s3TargetColor);
}

static void resetStage3Progress(TickType_t now)
{
    gameState.stage3Successes = 0;
    startStage3Round(now);
}

// Write text to one LCD row, padding to 20 chars to erase leftovers 
static void lcd_print_line(uint8_t row, const char *text)
{
    uint8_t i;
    uint8_t len = (uint8_t)strlen(text);
    lcd_i2c_set_cursor(&lcd, 0, row);
    lcd_i2c_write_str(&lcd, text);
    for (i = len; i < 20; i++) {
        lcd_i2c_write_char(&lcd, ' ');
    }
}

// Tasks
static void InputTask(void *pvParameters)
{
    bool joystickLatched = false;
    char key;
    char lastKey      = '\0';
    bool startBtnLast = false;
    const Stage3Color s3ButtonColors[3] = {
        S3_COLOR_RED, S3_COLOR_GREEN, S3_COLOR_BLUE
    };
    const int s3ButtonPins[3] = {
        BTN_B_PIN, BTN_A_PIN, REACT_BTN_PIN
    };
    bool s3LastRawPressed[3] = {false, false, false};
    bool s3ButtonArmed[3]    = {true, true, true};
    TickType_t s3ReleasedAt[3] = {0, 0, 0};
    (void)pvParameters;

    while (1) {
        key = keypad_get_key();

        xSemaphoreTake(gameMutex, portMAX_DELAY);

        // Start from idle, or reset during an active game 
        bool startPressed = isStartPressed();
        if (startPressed && !startBtnLast) {
            if (!gameState.gameStarted) {
                gameState.gameStarted     = true;
                gameState.stage           = GAME_STAGE1_INTRO;
                gameState.attemptsLeft    = 3;
                gameState.stage1Unlocked  = false;
                gameState.stage2Unlocked  = false;
                gameState.stage2Progress  = 0;
                gameState.stage3Successes = 0;
                resetGameTimer();
                gameFinishedElapsedMs     = 0;
                failureReason             = FAIL_NONE;
                clearEnteredCode();
                clearSubmittedCode();
                pendingDirection = DIR_NONE;
                directionReady   = false;
                stage2StartedAt  = 0;
                s3TargetColor    = S3_COLOR_NONE;
                s3PressedColor   = S3_COLOR_NONE;
                s3ButtonReady    = false;
                s3RoundStartedAt = 0;
                s3Status         = S3_STATUS_ACTIVE;
                potValue         = 0;
                potPercent       = 0;
            } else {
                resetToIdle();
                ESP_LOGI(TAG, "Game reset by START button");
            }

            xSemaphoreGive(gameMutex);
            lastKey      = '\0';
            startBtnLast = startPressed;
            shortBeep(80);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        startBtnLast = startPressed;

        // Confirm stage instructions with # before timers start 
        if (key == '#' && key != lastKey) {
            TickType_t now = xTaskGetTickCount();

            if (gameState.stage == GAME_STAGE1_INTRO) {
                resetOutputs();
                gameState.stage        = GAME_STAGE1;
                gameState.attemptsLeft = 3;
                resetStage1Progress(now);
                resumeGameTimer(now);
                ESP_LOGI(TAG, "Stage 1 started");

                xSemaphoreGive(gameMutex);
                lastKey = key;
                shortBeep(80);
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            } else if (gameState.stage == GAME_STAGE2_INTRO) {
                resetOutputs();
                gameState.stage        = GAME_STAGE2;
                gameState.attemptsLeft = 3;
                resetStage2Progress(now);
                resumeGameTimer(now);
                ESP_LOGI(TAG, "Stage 2 started");

                xSemaphoreGive(gameMutex);
                lastKey = key;
                shortBeep(80);
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            } else if (gameState.stage == GAME_STAGE3_INTRO) {
                resetOutputs();
                gameState.stage        = GAME_STAGE3;
                gameState.attemptsLeft = 3;
                resetStage3Progress(now);
                resumeGameTimer(now);
                ESP_LOGI(TAG, "Stage 3 started");

                xSemaphoreGive(gameMutex);
                lastKey = key;
                shortBeep(80);
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }
        }

        // Stage 1: read potentiometer and keypad entry 
        if (gameState.stage == GAME_STAGE1) {
            potValue   = readAveragedPotRaw();
            potPercent = adcToPercent(potValue);

            if (key == '*' && key != lastKey) {
                if (s1Phase == S1_ENTER_CODE) {
                    clearSubmittedCode();
                    clearEnteredCode();
                    ESP_LOGI(TAG, "S1: Keypad entry cleared by *");
                } else {
                    resetStage1Progress(xTaskGetTickCount());
                    ESP_LOGI(TAG, "S1: Progress reset by *");
                }
            } else if (s1Phase == S1_ENTER_CODE && key != '\0' && key != lastKey) {
                if (key >= '0' && key <= '9' && s1KeypadLength < 4) {
                    submittedCode[s1KeypadLength++] = key;
                    submittedCode[s1KeypadLength]   = '\0';
                    strncpy(gameState.enteredCode, submittedCode,
                            sizeof(gameState.enteredCode) - 1);
                    gameState.enteredCode[sizeof(gameState.enteredCode) - 1] = '\0';
                    gameState.enteredLength = s1KeypadLength;

                } else if (key == '#' && s1KeypadLength == 4) {
                    codeSubmitted = true;
                }
            }
        }

        // Stage 2: joystick direction 
        if (gameState.stage == GAME_STAGE2) {
            JoyDirection dir = readJoystickDirection();

            if (!joystickLatched && dir != DIR_NONE) {
                pendingDirection = dir;
                directionReady   = true;
                joystickLatched  = true;
                ESP_LOGI(TAG, "Joystick: %s", directionToText(dir));
            }
            if (joystickLatched && joystickCentered()) {
                joystickLatched = false;
            }
        }

        // Stage 3: debounced colored button press 
        if (gameState.stage == GAME_STAGE3) {
            TickType_t now = xTaskGetTickCount();
            int i;

            for (i = 0; i < 3; i++) {
                bool rawPressed = (gpio_get_level(s3ButtonPins[i]) == 0);

                if (rawPressed && s3ButtonArmed[i] && !s3ButtonReady) {
                    s3PressedColor = s3ButtonColors[i];
                    s3ButtonReady  = true;
                    s3ButtonArmed[i] = false;
                    s3ReleasedAt[i] = 0;
                    ESP_LOGI(TAG, "S3 button: %s",
                             stage3ColorToText(s3PressedColor));
                }

                if (!rawPressed && s3LastRawPressed[i]) {
                    s3ReleasedAt[i] = now;
                }

                if (!rawPressed && !s3ButtonArmed[i] && s3ReleasedAt[i] != 0 &&
                    (now - s3ReleasedAt[i]) >= pdMS_TO_TICKS(S3_BUTTON_RELEASE_MS)) {
                    s3ButtonArmed[i] = true;
                    s3ReleasedAt[i] = 0;
                }

                s3LastRawPressed[i] = rawPressed;
            }
        } else {
            int i;
            for (i = 0; i < 3; i++) {
                s3LastRawPressed[i] = false;
                s3ButtonArmed[i]    = true;
                s3ReleasedAt[i]     = 0;
            }
        }

        xSemaphoreGive(gameMutex);
        lastKey = key;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void LogicTask(void *pvParameters)
{
    TickType_t stage1PassedAt     = 0;
    bool       stage1TimerStarted = false;
    bool       runSuccess;
    bool       runError;
    bool       runStep;
    bool       runStage2Complete;
    bool       runStage3Complete;
    bool       restartStage3AfterError;
    (void)pvParameters;

    while (1) {
        runSuccess        = false;
        runError          = false;
        runStep           = false;
        runStage2Complete = false;
        runStage3Complete = false;
        restartStage3AfterError = false;

        xSemaphoreTake(gameMutex, portMAX_DELAY);

        if (gameState.gameStarted && gameRemainingMs(xTaskGetTickCount()) == 0) {
            gameState.stage       = GAME_FAILURE;
            gameState.gameStarted = false;
            pauseGameTimer(xTaskGetTickCount());
            failureReason         = FAIL_GAME_TIMEOUT;
            ESP_LOGI(TAG, "Game timer expired.");
            runError = true;
        }

        // Stage 1: slider zone detection 
        if (gameState.stage == GAME_STAGE1 &&
            s1Phase == S1_REVEAL_DIGITS && s1Step < 4) {
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed_ms =
                (uint32_t)((now - s1DigitStartedAt) * portTICK_PERIOD_MS);
            uint8_t target = S1_TARGETS_PCT[s1Step];
            uint8_t low    = (target > S1_TOLERANCE_PCT)
                               ? (uint8_t)(target - S1_TOLERANCE_PCT)
                               : 0;
            uint8_t high   = (target + S1_TOLERANCE_PCT < 100)
                               ? (uint8_t)(target + S1_TOLERANCE_PCT)
                               : 100;
            bool inZone = (potPercent >= low && potPercent <= high);

            if (s1DigitStartedAt == 0) {
                s1DigitStartedAt = now;
                elapsed_ms = 0;
            }

            if (elapsed_ms >= S1_DIGIT_TIME_MS) {
                ESP_LOGI(TAG, "S1: Digit %u timed out. Resetting.",
                         (unsigned int)s1Step + 1);
                if (useStageAttempt()) {
                    resetStage1Progress(now);
                }
                runError = true;
                runStep  = false;
            } else {
                if (inZone && !s1InZone) {
                    s1InZone    = true;
                    s1HoldStart = now;
                    ESP_LOGI(TAG, "S1: Holding target %u%% (pot=%u%%)",
                             (unsigned int)target, (unsigned int)potPercent);
                } else if (!inZone) {
                    s1InZone = false;
                }

                if (s1InZone) {
                    uint32_t held_ms =
                        (uint32_t)((now - s1HoldStart) * portTICK_PERIOD_MS);

                    if (held_ms >= S1_HOLD_MS) {
                        s1RevealedCode[s1Step] = S1_DIGITS[s1Step];
                        s1Step++;
                        s1RevealedCode[s1Step] = '\0';
                        gameState.enteredLength = s1Step;
                        s1InZone = false;
                        s1HoldStart = 0;
                        s1DigitStartedAt = now;
                        runStep = true;
                        ESP_LOGI(TAG, "S1: Revealed digit %u/%u = %c",
                                 (unsigned int)s1Step, 4u,
                                 S1_DIGITS[s1Step - 1]);

                        if (s1Step >= 4) {
                            s1Phase = S1_ENTER_CODE;
                            clearSubmittedCode();
                            clearEnteredCode();
                            ESP_LOGI(TAG, "S1: All digits revealed. Awaiting keypad.");
                        }
                    }
                }
            }
        }

        // Stage 1: code verification 
        if (gameState.stage == GAME_STAGE1 &&
            s1Phase == S1_ENTER_CODE && codeSubmitted) {
            codeSubmitted = false;

            if (strcmp(submittedCode, s1RevealedCode) == 0) {
                ESP_LOGI(TAG, "Correct code!");
                gameState.stage1Unlocked = true;
                gameState.stage          = GAME_STAGE1_PASSED;
                stage1PassedAt           = xTaskGetTickCount();
                stage1TimerStarted       = true;
                runSuccess               = true;
                runStep                  = false; // success overrides the step beep 
            } else {
                ESP_LOGI(TAG, "Wrong code - resetting stage 1.");
                if (useStageAttempt()) {
                    resetStage1Progress(xTaskGetTickCount());
                }
                runError = true;
                runStep  = false;
            }
            clearSubmittedCode();
        }

        // Stage 1 Passed -> Stage 2 (1.5 s delay) 
        if (gameState.stage == GAME_STAGE1_PASSED && stage1TimerStarted) {
            TickType_t now = xTaskGetTickCount();

            if ((now - stage1PassedAt) >= pdMS_TO_TICKS(1500)) {
                gameState.stage        = GAME_STAGE2_INTRO;
                gameState.attemptsLeft = 3;
                pauseGameTimer(now);
                stage1TimerStarted     = false;
                resetOutputs();
            }
        }

        // Stage 2 timer 
        if (gameState.stage == GAME_STAGE2) {
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed_ms =
                (uint32_t)((now - stage2StartedAt) * portTICK_PERIOD_MS);

            if (stage2StartedAt == 0) {
                resetStage2Progress(now);
                elapsed_ms = 0;
            }

            if (elapsed_ms >= STAGE2_TIME_MS) {
                ESP_LOGI(TAG, "S2: Timer expired. Restarting sequence.");
                if (useStageAttempt()) {
                    resetStage2Progress(now);
                }
                runError = true;
            }
        }

        // Stage 2 check 
        if (gameState.stage == GAME_STAGE2 && directionReady) {
            JoyDirection expected =
                stage2Sequence[gameState.stage2Progress].expectedDir;
            JoyDirection received = pendingDirection;
            directionReady   = false;
            pendingDirection = DIR_NONE;

            if (received == expected) {
                gameState.stage2Progress++;
                ESP_LOGI(TAG, "Correct move. Progress: %d",
                         gameState.stage2Progress);

                if (gameState.stage2Progress >= STAGE2_STEPS) {
                    gameState.stage2Unlocked = true;
                    gameState.stage          = GAME_STAGE2_PASSED;
                    runStage2Complete        = true;
                } else {
                    stage2StartedAt = xTaskGetTickCount();
                    runStep = true;
                }
            } else {
                ESP_LOGI(TAG, "Wrong move. Expected: %s  Got: %s",
                         directionToText(expected), directionToText(received));
                if (useStageAttempt()) {
                    resetStage2Progress(xTaskGetTickCount());
                }
                runError = true;
            }
        }

        // Stage 3 check 
        if (gameState.stage == GAME_STAGE3) {
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed_ms =
                (uint32_t)((now - s3RoundStartedAt) * portTICK_PERIOD_MS);

            if (s3RoundStartedAt == 0 || s3TargetColor == S3_COLOR_NONE) {
                startStage3Round(now);
                elapsed_ms = 0;
            }

            if (s3ButtonReady) {
                Stage3Color pressed = s3PressedColor;
                s3ButtonReady  = false;
                s3PressedColor = S3_COLOR_NONE;

                if (pressed == s3TargetColor && elapsed_ms <= S3_ROUND_TIME_MS) {
                    gameState.stage3Successes++;
                    s3Status = S3_STATUS_CORRECT;
                    ESP_LOGI(TAG, "S3: Correct %s. Round %u/%u",
                             stage3ColorToText(pressed),
                             (unsigned int)gameState.stage3Successes,
                             (unsigned int)STAGE3_ROUNDS);

                    if (gameState.stage3Successes >= STAGE3_ROUNDS) {
                        gameFinishedElapsedMs = gameElapsedMs(now);
                        gameState.stage       = GAME_SUCCESS;
                        gameState.gameStarted = false;
                        pauseGameTimer(now);
                        failureReason         = FAIL_NONE;
                        setStage3Led(S3_COLOR_NONE);
                        runStage3Complete     = true;
                    } else {
                        startStage3Round(now);
                        runStep = true;
                    }
                } else {
                    s3Status = S3_STATUS_WRONG;
                    setStage3Led(S3_COLOR_NONE);
                    ESP_LOGI(TAG, "S3: Wrong button. Expected %s, got %s.",
                             stage3ColorToText(s3TargetColor),
                             stage3ColorToText(pressed));
                    restartStage3AfterError = useStageAttempt();
                    runError = true;
                    runStep  = false;
                }
            } else if (elapsed_ms >= S3_ROUND_TIME_MS) {
                s3Status = S3_STATUS_TIMEOUT;
                setStage3Led(S3_COLOR_NONE);
                ESP_LOGI(TAG, "S3: Timeout on %s. Restarting stage 3.",
                         stage3ColorToText(s3TargetColor));
                restartStage3AfterError = useStageAttempt();
                runError = true;
                runStep  = false;
            }
        }

        xSemaphoreGive(gameMutex);

        // Post-mutex feedback (blocking ops outside critical section) 
        if (runStage3Complete) {
            victoryMusic();
        } else if (runStage2Complete) {
            successFeedback();
            resetOutputs();
            xSemaphoreTake(gameMutex, portMAX_DELAY);
            gameState.stage        = GAME_STAGE3_INTRO;
            gameState.attemptsLeft = 3;
            pauseGameTimer(xTaskGetTickCount());
            s3TargetColor    = S3_COLOR_NONE;
            s3PressedColor   = S3_COLOR_NONE;
            s3ButtonReady    = false;
            s3RoundStartedAt = 0;
            s3Status         = S3_STATUS_ACTIVE;
            xSemaphoreGive(gameMutex);
            ESP_LOGI(TAG, "Stage 3 instructions shown");
        } else if (runSuccess) {
            successFeedback();
        } else if (runStep) {
            shortBeep(40);
        } else if (runError) {
            errorFeedback();
            if (restartStage3AfterError) {
                xSemaphoreTake(gameMutex, portMAX_DELAY);
                if (gameState.stage == GAME_STAGE3) {
                    resetStage3Progress(xTaskGetTickCount());
                }
                xSemaphoreGive(gameMutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void DisplayTask(void *pvParameters)
{
    char lastLine1[21] = "";
    char lastLine2[21] = "";
    char lastLine3[21] = "";
    char lastLine4[21] = "";
    char line1[21];
    char line2[21];
    char line3[21];
    char line4[21];
    (void)pvParameters;

    while (1) {
        memset(line1, 0, sizeof(line1));
        memset(line2, 0, sizeof(line2));
        memset(line3, 0, sizeof(line3));
        memset(line4, 0, sizeof(line4));

        xSemaphoreTake(gameMutex, portMAX_DELAY);
        char gameTime[6];
        formatTimeMMSS(gameRemainingMs(xTaskGetTickCount()),
                       gameTime, sizeof(gameTime));

        switch (gameState.stage) {

            case GAME_IDLE:
                snprintf(line1, 21, "  ** ESCAPE ROOM ** ");
                snprintf(line2, 21, "Time Limit: %s  ", gameTime);
                snprintf(line3, 21, "   Press START to   ");
                snprintf(line4, 21, "      begin!        ");
                resetOutputs();
                break;

            case GAME_STAGE1_INTRO:
                snprintf(line1, 21, "Stage 1: Slider    ");
                snprintf(line2, 21, "Find target %% hold ");
                snprintf(line3, 21, "Reveal 4 code nums ");
                snprintf(line4, 21, "Press # to start   ");
                resetOutputs();
                break;

            case GAME_STAGE1: {
                char s1DispDigits[5];
                char s1DispEntry[5];
                int  idx;

                for (idx = 0; idx < 4; idx++) {
                    s1DispDigits[idx] = (idx < (int)s1Step) ? s1RevealedCode[idx] : '_';
                    s1DispEntry[idx]  = (idx < (int)s1KeypadLength) ? submittedCode[idx] : '_';
                }
                s1DispDigits[4] = '\0';
                s1DispEntry[4]  = '\0';

                if (s1Phase == S1_REVEAL_DIGITS) {
                    TickType_t now = xTaskGetTickCount();
                    uint32_t elapsed_ms = (uint32_t)(
                        (now - s1DigitStartedAt) * portTICK_PERIOD_MS);
                    uint32_t left_ms = (elapsed_ms >= S1_DIGIT_TIME_MS)
                                         ? 0
                                         : (S1_DIGIT_TIME_MS - elapsed_ms);
                    uint32_t left_s = (left_ms + 999u) / 1000u;

                    snprintf(line1, 21, "S1 Tgt:%3u%% D:%2lu",
                             (unsigned int)S1_TARGETS_PCT[s1Step],
                             (unsigned long)left_s);
                    snprintf(line2, 21, "Game:%s  Att:%d",
                             gameTime, gameState.attemptsLeft);
                    snprintf(line3, 21, "Pot:%3u%% %s %u/4",
                             (unsigned int)potPercent,
                             s1InZone ? "HOLD" : "MOVE",
                             (unsigned int)s1Step);
                    snprintf(line4, 21, "Code: [%c][%c][%c][%c]",
                             s1DispDigits[0], s1DispDigits[1],
                             s1DispDigits[2], s1DispDigits[3]);
                } else {
                    snprintf(line1, 21, "S1 Code G:%s A:%d",
                             gameTime, gameState.attemptsLeft);
                    snprintf(line2, 21, "Enter code: %c%c%c%c",
                             s1DispEntry[0], s1DispEntry[1],
                             s1DispEntry[2], s1DispEntry[3]);
                    snprintf(line3, 21, "Stored: [%c][%c][%c][%c]",
                             s1DispDigits[0], s1DispDigits[1],
                             s1DispDigits[2], s1DispDigits[3]);
                    snprintf(line4, 21, "* Clear   # Submit ");
                }
                break;

            }

            case GAME_STAGE1_PASSED:
                snprintf(line1, 21, "  Stage 1 Passed!   ");
                snprintf(line2, 21, "  Code Accepted!    ");
                snprintf(line3, 21, "Game: %s        ", gameTime);
                snprintf(line4, 21, " Loading Stage 2... ");
                break;

            case GAME_STAGE2_INTRO:
                snprintf(line1, 21, "Stage 2: Joystick  ");
                snprintf(line2, 21, "Move shown dir     ");
                snprintf(line3, 21, "REVERSE=opposite   ");
                snprintf(line4, 21, "Press # to start   ");
                resetOutputs();
                break;

            case GAME_STAGE2: {
                TickType_t now = xTaskGetTickCount();
                char instruction[14];
                char stepChar = (char)('1' + gameState.stage2Progress);
                uint32_t elapsed_ms = (uint32_t)(
                    (now - stage2StartedAt) * portTICK_PERIOD_MS);
                uint32_t left_ms = (elapsed_ms >= STAGE2_TIME_MS)
                                     ? 0
                                     : (STAGE2_TIME_MS - elapsed_ms);
                uint32_t left_s = (left_ms + 999u) / 1000u;

                stage2InstructionText(gameState.stage2Progress,
                                      instruction, sizeof(instruction));
                snprintf(line1, 21, "Stage 2: Joystick   ");
                snprintf(line2, 21, "Move: %-14s",
                         instruction);
                snprintf(line3, 21, "Time:%02lus  Att:%d",
                         (unsigned long)left_s,
                         gameState.attemptsLeft);
                snprintf(line4, 21, "Game:%s Step:%c/5",
                         gameTime,
                         stepChar);
                break;
            }

            case GAME_STAGE2_PASSED:
                snprintf(line1, 21, "  Stage 2 Passed!   ");
                snprintf(line2, 21, "  Sequence Done!    ");
                snprintf(line3, 21, "Game: %s        ", gameTime);
                snprintf(line4, 21, " Loading Stage 3... ");
                break;

            case GAME_STAGE3_INTRO:
                snprintf(line1, 21, "Stage 3: LEDs      ");
                snprintf(line2, 21, "Press matching btn ");
                snprintf(line3, 21, "Red  Green  Blue   ");
                snprintf(line4, 21, "Press # to start   ");
                resetOutputs();
                break;

            case GAME_STAGE3: {
                TickType_t now = xTaskGetTickCount();
                uint32_t elapsed_ms = (uint32_t)(
                    (now - s3RoundStartedAt) * portTICK_PERIOD_MS);
                uint32_t left_ms = (elapsed_ms >= S3_ROUND_TIME_MS)
                                     ? 0
                                     : (S3_ROUND_TIME_MS - elapsed_ms);
                const char *statusText =
                    (s3Status == S3_STATUS_CORRECT) ? "Correct!" :
                    (s3Status == S3_STATUS_WRONG)   ? "Wrong - restart" :
                    (s3Status == S3_STATUS_TIMEOUT) ? "Timeout - restart" :
                                                       "Press matching btn";

                snprintf(line1, 21, "Stage 3 Match Att:%d",
                         gameState.attemptsLeft);
                snprintf(line2, 21, "LED:%-5s T:%lu.%01lu",
                         stage3ColorToText(s3TargetColor),
                         (unsigned long)(left_ms / 1000u),
                         (unsigned long)((left_ms % 1000u) / 100u));
                snprintf(line3, 21, "Game:%s R:%u/%u",
                         gameTime,
                         (unsigned int)gameState.stage3Successes + 1u,
                         (unsigned int)STAGE3_ROUNDS);
                snprintf(line4, 21, "%-20s", statusText);
                break;
            }

            case GAME_SUCCESS:
                formatTimeMMSS(gameFinishedElapsedMs, gameTime, sizeof(gameTime));
                snprintf(line1, 21, "********************");
                snprintf(line2, 21, "*  YOU  ESCAPED!  * ");
                snprintf(line3, 21, "* Congratulations!* ");
                snprintf(line4, 21, "Solved in: %s  ", gameTime);
                break;

            case GAME_FAILURE:
                snprintf(line1, 21, "--------------------");
                snprintf(line2, 21, "%s",
                         failureReason == FAIL_GAME_TIMEOUT
                             ? "    TIME EXPIRED    "
                             : "    GAME  OVER      ");
                snprintf(line3, 21, "--------------------");
                snprintf(line4, 21, "  Press START btn   ");
                break;

            default:
                snprintf(line1, 21, "Unknown State       ");
                snprintf(line2, 21, "Check Code          ");
                snprintf(line3, 21, "                    ");
                snprintf(line4, 21, "                    ");
                break;
        }

        xSemaphoreGive(gameMutex);

        // Only redraw when content changes to avoid flicker 
        if (strcmp(line1, lastLine1) != 0 || strcmp(line2, lastLine2) != 0 ||
            strcmp(line3, lastLine3) != 0 || strcmp(line4, lastLine4) != 0) {
            lcd_print_line(0, line1);
            lcd_print_line(1, line2);
            lcd_print_line(2, line3);
            lcd_print_line(3, line4);
            strncpy(lastLine1, line1, sizeof(lastLine1) - 1);
            strncpy(lastLine2, line2, sizeof(lastLine2) - 1);
            strncpy(lastLine3, line3, sizeof(lastLine3) - 1);
            strncpy(lastLine4, line4, sizeof(lastLine4) - 1);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

//Init
void initGameTasks(void)
{
    // Output pins (LEDs) 
    gpio_reset_pin(RED_LED_PIN);
    gpio_set_direction(RED_LED_PIN,   GPIO_MODE_OUTPUT);
    gpio_reset_pin(GREEN_LED_PIN);
    gpio_set_direction(GREEN_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(BLUE_LED_PIN);
    gpio_set_direction(BLUE_LED_PIN,  GPIO_MODE_OUTPUT);

    // Input pins with internal pull-up 
    gpio_reset_pin(START_BTN_PIN);
    gpio_set_direction(START_BTN_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(START_BTN_PIN, GPIO_PULLUP_ONLY);

    gpio_reset_pin(REACT_BTN_PIN);
    gpio_set_direction(REACT_BTN_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(REACT_BTN_PIN, GPIO_PULLUP_ONLY);

    gpio_reset_pin(BTN_A_PIN);
    gpio_set_direction(BTN_A_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_A_PIN, GPIO_PULLUP_ONLY);

    gpio_reset_pin(BTN_B_PIN);
    gpio_set_direction(BTN_B_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_B_PIN, GPIO_PULLUP_ONLY);

    gpio_reset_pin(JOY_SEL_PIN);
    gpio_set_direction(JOY_SEL_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(JOY_SEL_PIN, GPIO_PULLUP_ONLY);

    // ADC: joystick axes + slide potentiometer 
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten((adc1_channel_t)JOY_X_CH, ADC_ATTEN_DB_12);
    adc1_config_channel_atten((adc1_channel_t)JOY_Y_CH, ADC_ATTEN_DB_12);
    adc1_config_channel_atten((adc1_channel_t)POT_CH,   ADC_ATTEN_DB_12);

    // Buzzer (LEDC PWM tone generator, 10-bit, TIMER_1 / CHANNEL_1) 
    ledc_timer_config_t buzzer_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = 2000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&buzzer_timer));

    ledc_channel_config_t buzzer_channel = {
        .gpio_num   = BUZZER_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&buzzer_channel));

    // Keypad 
    keypad_init();

    // LCD (20×4, I2C, PCF8574 at 0x27) 
    lcd_i2c_config_t lcd_cfg = {
        .i2c_port     = I2C_NUM_0,
        .sda_gpio     = LCD_SDA,
        .scl_gpio     = LCD_SCL,
        .address      = 0x27,
        .cols         = 20,
        .rows         = 4,
        .clk_speed_hz = 100000,
        .backlight    = true,
    };
    ESP_ERROR_CHECK(lcd_i2c_init(&lcd, &lcd_cfg));

    resetOutputs();

    // FreeRTOS mutex and initial state 
    gameMutex = xSemaphoreCreateMutex();
    xSemaphoreTake(gameMutex, portMAX_DELAY);
    resetToIdle();
    xSemaphoreGive(gameMutex);

    // Spawn tasks 
    xTaskCreate(InputTask,   "InputTask",   4096, NULL, 2, &inputTaskHandle);
    xTaskCreate(LogicTask,   "LogicTask",   4096, NULL, 3, &logicTaskHandle);
    xTaskCreate(DisplayTask, "DisplayTask", 4096, NULL, 1, &displayTaskHandle);
}
