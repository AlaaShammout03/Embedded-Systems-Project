# Interactive Escape Room Simulator Using ESP32 and FreeRTOS

### Project Context

This was a 3-person embedded systems team project.

### My Contributions

- Implemented FreeRTOS task structure for input handling, logic, and display updates.
- Worked on shared-state synchronization using a mutex.
- Integrated keypad, joystick, potentiometer, LEDs, buzzer, and LCD behavior.
- Helped design and document the state-based game flow.

---

## Project Description

This project is an ESP32-based interactive escape room simulator developed using FreeRTOS and simulated in Wokwi. The game challenges the player to complete three puzzle stages before the total game timer expires.

The system uses multiple input devices, including a 4x4 keypad, analog joystick, slide potentiometer, start/reset button, and three color-matching reaction buttons. It gives feedback using a 20x4 I2C LCD, three LEDs, and a buzzer.

The player starts the game using the start button. Before each stage begins, the LCD displays instructions and the player must press # on the keypad to continue. This prevents the timer for the stage from starting before the player is ready.

The game has three stages:

1. **Stage 1: Slider and Keypad Challenge**

   * The player moves the slide potentiometer to match four target percentages.
   * The targets used are: `70%`, `25%`, `85%`, and `40%`.
   * The player must hold the potentiometer within the target range for a short time to reveal each digit.
   * After revealing all digits, the player enters the code using the keypad and submits it using `#`.

2. **Stage 2: Joystick Sequence Challenge**

   * The system generates a 5-step joystick movement sequence.
   * Some steps are shown as `REVERSE` meaning the player must move the joystick in the opposite direction.
   * The player must complete each movement before the stage timer expires.

3. **Stage 3: LED Reaction Challenge**

   * One of the colored LEDs lights up randomly.
   * The player must press the matching color button quickly.
   * The player must complete 5 correct reaction rounds to win.

If the player completes all three stages successfully, the LCD displays a success message and the buzzer plays victory feedback. If the player runs out of time or attempts, the system enters the failure state and gives error feedback using the red LED and buzzer.

---

## System Diagram

The system was designed and simulated using Wokwi.

![System Diagram](images/system_diagram.png)


### Main Components Used

| Component                    | Purpose                                                                                           |
| ---------------------------- | ------------------------------------------------------------------------------------------------- |
| ESP32 DevKit                 | Main microcontroller running the game and FreeRTOS tasks                                          |
| 4x4 Keypad                   | Used to start stages using `#`, enter the Stage 1 code, submit answers, and clear input using `*` |
| Slide Potentiometer          | Used in Stage 1 to match target percentages                                                       |
| Analog Joystick              | Used in Stage 2 to enter movement directions                                                      |
| Start/Reset Button           | Starts the game from idle or resets the game during play                                          |
| Red, Green, and Blue Buttons | Used in Stage 3 to match the active LED color                                                     |
| Red, Green, and Blue LEDs    | Visual feedback and Stage 3 reaction targets                                                      |
| Buzzer                       | Audio feedback for correct steps, errors, and victory                                             |
| 20x4 I2C LCD                 | Displays instructions, timer, stage progress, attempts, success, and failure messages             |

---

## How it works

The system follows a state-based game flow:

1. **Idle State**

   * The LCD displays the escape room welcome screen.
   * The player presses the start button to begin.

2. **Stage 1 Intro**

   * The LCD explains the slider challenge.
   * The player presses `#` to start Stage 1.

3. **Stage 1: Slider and Keypad**

   * The player adjusts the slide potentiometer to each target percentage.
   * Each correct hold reveals one digit of the code.
   * The final revealed code is entered using the keypad.
   * `*` clears or resets Stage 1 input.
   * `#` submits the code.

4. **Stage 2 Intro**

   * The LCD explains the joystick challenge.
   * The player presses `#` to start Stage 2.

5. **Stage 2: Joystick Sequence**

   * The player follows a 5-step direction sequence.
   * If the LCD says `REVERSE`, the player moves in the opposite direction.
   * A wrong move or timeout uses one attempt and restarts the stage.

6. **Stage 3 Intro**

   * The LCD explains the LED reaction challenge.
   * The player presses `#` to start Stage 3.

7. **Stage 3: Reaction Challenge**

   * A random LED turns on.
   * The player presses the matching color button.
   * The player must complete 5 correct rounds.

8. **Success State**

   * The LCD displays `YOU ESCAPED!`.
   * All LEDs and the buzzer provide victory feedback.

9. **Failure State**

   * The system displays either `TIME EXPIRED` or `GAME OVER`.
   * The red LED and buzzer provide failure feedback.

![State Machine Diagram](images/state_machine_diagram.png)

---

## Project Impact

This project demonstrates how FreeRTOS can be used to build an interactive real-time embedded system. It shows how multiple tasks can work together to read user inputs, process game logic, update a display, and generate feedback without blocking the entire system.

The project is useful as an educational embedded systems application because it combines:

* FreeRTOS multitasking
* Shared-state synchronization using a mutex
* LCD communication using I2C
* ADC input from a joystick and slide potentiometer
* GPIO input and output
* PWM buzzer control
* Real-time timing and stage-based game logic

The same idea can be extended into interactive training systems, escape room games, reaction-time games, or simple security-style puzzle systems.

---

## FreeRTOS Implementation

### Tasks

The final implementation uses three FreeRTOS tasks.

| Task Name     | Priority | What it does                                                                                                      | Timing / Delay    |
| ------------- | -------: | ----------------------------------------------------------------------------------------------------------------- | ----------------- |
| `InputTask`   |        2 | Reads keypad input, start/reset button, slide potentiometer, joystick directions, and Stage 3 reaction buttons    | Runs every 30 ms  |
| `LogicTask`   |        3 | Controls the main game logic, stage transitions, attempts, timers, success/failure states, and feedback decisions | Runs every 20 ms  |
| `DisplayTask` |        1 | Updates the 20x4 LCD based on the current game state, timer, attempts, and progress                               | Runs every 100 ms |

The `LogicTask` has the highest priority because it controls the main decision-making of the game. The `InputTask` has medium priority because it must frequently read user actions. The `DisplayTask` has the lowest priority because small LCD update delays do not affect the correctness of the game.

---

### Task Communication and Synchronization

The final project does **not** use FreeRTOS queues or event groups. It also does **not** use a binary semaphore for button interrupts.

Instead, the tasks communicate through shared global game variables protected by a FreeRTOS mutex.

### Mutex Used

The project creates one mutex called `gameMutex`:

```c
gameMutex = xSemaphoreCreateMutex();
```

The mutex is used with:

```c
xSemaphoreTake(gameMutex, portMAX_DELAY);
xSemaphoreGive(gameMutex);
```

This protects the shared game state from being accessed by multiple tasks at the same time.

### Shared State Protected by the Mutex

The mutex protects variables such as:

* Current game stage
* Game started flag
* Attempts left
* Entered keypad code
* Stage 1 progress
* Potentiometer percentage
* Stage 2 joystick progress
* Pending joystick direction
* Stage 3 target color
* Stage 3 button press state
* Game timer values
* Success and failure state

### Communication Method

| Task          | Communication Method                                       | Purpose                                |
| ------------- | ---------------------------------------------------------- | -------------------------------------- |
| `InputTask`   | Updates shared variables while holding the mutex           | Sends user actions to the game state   |
| `LogicTask`   | Reads and updates shared variables while holding the mutex | Checks correctness and changes stages  |
| `DisplayTask` | Reads shared variables while holding the mutex             | Displays the correct screen on the LCD |

This approach is simple and suitable for the simulation because the project only has three main tasks and one shared game state structure.

---

## Screenshots

![Stage 1](images/stage_1.png)
*Stage 1: Slider and Keypad Challenge. The player has matched a target percentage and the LCD reveals one digit of the unlock code.*

![Stage 2](images/stage_2.png)
*Stage 2: Joystick Sequence Challenge. The LCD shows the current step in the 5-step sequence, including a REVERSE prompt where the player must move opposite to the indicated direction.*

![Stage 3](images/stage_3.png)
*Stage 3: LED Reaction Challenge. A random LED is lit and the player must press the matching color button before the timer runs out.*

![Success](images/Success.png)
*Success state: all three stages cleared within the time limit. The LCD displays `YOU ESCAPED!` and the buzzer plays victory feedback.*

![Failure](images/failure.png)
*Failure state: triggered when the player runs out of attempts or the global timer expires. The red LED and buzzer provide error feedback.*
