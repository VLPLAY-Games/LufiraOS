#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define INPUT_BUFFER_SIZE 256

#define KEY_LEFT_ARROW   0x01
#define KEY_RIGHT_ARROW  0x02
#define KEY_UP_ARROW     0x03
#define KEY_DOWN_ARROW   0x04

extern char input_buffer[INPUT_BUFFER_SIZE];
extern uint32_t input_buffer_index;

void keyboard_init(void);
void keyboard_irq_handler(void);           // обработчик прерывания клавиатуры
uint8_t keyboard_read_scancode(void);
int keyboard_scancode_to_key(uint8_t scancode);
void process_keypress(int key);
int keyboard_is_initialized(void);

#endif