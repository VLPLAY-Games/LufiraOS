#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define INPUT_BUFFER_SIZE 256

// Глобальные переменные
extern char input_buffer[INPUT_BUFFER_SIZE];
extern uint32_t input_buffer_index;

// Прототипы функций
void keyboard_init(void);
void keyboard_handler(void);
uint8_t keyboard_read_scancode(void);
char keyboard_scancode_to_char(uint8_t scancode);
void process_keypress(char c);

#endif