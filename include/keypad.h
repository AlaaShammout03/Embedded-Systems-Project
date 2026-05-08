#ifndef KEYPAD_H
#define KEYPAD_H

void keypad_init(void);
char keypad_get_key(void);  //returns '\0' when no key is pressed

#endif // KEYPAD_H
