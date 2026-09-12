#pragma once

#include "lib/types.h"

#define INPUT_BUFFER_SIZE 256

#define KEY_LEFT_ARROW   0x01
#define KEY_RIGHT_ARROW  0x02
#define KEY_UP_ARROW     0x03
#define KEY_DOWN_ARROW   0x04
#define KEY_CTRL_C       0x05

extern char input_buffer[INPUT_BUFFER_SIZE];
extern uint32_t input_buffer_index;

void keyboard_init(void);
void keyboard_irq_handler(void);           // обработчик прерывания клавиатуры
uint8_t keyboard_read_scancode(void);
int keyboard_scancode_to_key(uint8_t scancode);
int keyboard_is_initialized(void);
int keyboard_ctrl_pressed(void);

// Позволяет ДРУГИМ источникам ввода (сейчас — USB HID, см. usb_hid.c)
// сообщить сюда текущее состояние Ctrl из СВОЕГО отчёта, чтобы
// keyboard_ctrl_pressed() отражал реальное состояние клавиши независимо от
// того, через какое устройство она была нажата — QEMU обычно доставляет
// один и тот же физический keystroke сразу на PS/2 и на USB HID.
void keyboard_set_ctrl_state(int pressed);
