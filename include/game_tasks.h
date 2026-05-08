#ifndef GAME_TASKS_H
#define GAME_TASKS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GAME_IDLE,
    GAME_STAGE1_INTRO,
    GAME_STAGE1,
    GAME_STAGE1_PASSED,
    GAME_STAGE2_INTRO,
    GAME_STAGE2,
    GAME_STAGE2_PASSED,
    GAME_STAGE3_INTRO,
    GAME_STAGE3,
    GAME_SUCCESS,
    GAME_FAILURE
} GameStage;

typedef struct {
    GameStage stage;
    bool      gameStarted;
    bool      stage1Unlocked;
    bool      stage2Unlocked;
    int       attemptsLeft;
    char      enteredCode[5];
    uint8_t   enteredLength;
    uint8_t   stage2Progress;
    uint8_t   stage3Successes;
} GameState;

void initGameTasks(void);

#ifdef __cplusplus
}
#endif

#endif // GAME_TASKS_H
