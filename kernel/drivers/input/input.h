#pragma once

#include "lib/types.h"

/*
 * Общий входной слой: и PS/2, и USB HID клавиатура/мышь сообщают о
 * событиях сюда, а не напрямую в shell/консоль. Это единственная точка,
 * которая знает, что делать с нажатием клавиши или движением мыши,
 * независимо от того, с какой шины оно пришло.
 */

// Обрабатывает уже декодированную клавишу (ASCII-символ либо KEY_* из
// keyboard.h). key == 0 игнорируется (нет символа для этого скан-кода).
// Обёртка над input_keyboard_event_ex(key, 1) — "клавиша нажата".
void input_keyboard_event(int key);

// То же самое, но с явным состоянием клавиши: pressed=1 — нажата, pressed=0 —
// отпущена. Отпускания доходят только до WM/GUI-клиентов (нужно играм вроде
// doomgeneric-порта — держать "идём вперёд", пока WASD зажаты); обычный
// текстовый /dev/console их не видит, см. input.c.
void input_keyboard_event_ex(int key, int pressed);

// dx/dy — относительное смещение с последнего события (положительное dy
// означает движение вниз, как и положено экранным координатам); buttons —
// битовая маска нажатых кнопок (бит 0 = левая, 1 = правая, 2 = средняя).
void input_mouse_event(int dx, int dy, uint8_t buttons);

int input_mouse_get_x(void);
int input_mouse_get_y(void);
uint8_t input_mouse_get_buttons(void);

// Кольцевой буфер сырых байт клавиатуры для блокирующего console_read()
// (vfs.c) — путь параллельный прямому вызову shell_handle_*() из
// input_keyboard_event() ниже (тот не трогаем, пока его не заменит
// shell.elf). Байты — коды из keyboard.h (KEY_LEFT_ARROW=0x01 и т.д.),
// свой формат, не ANSI. KEY_CTRL_C сюда не попадает — станет SIGINT,
// а не байтом в потоке.
//
// Если буфер пуст, вызывающий процесс уходит в PROCESS_BLOCKED (schedule()),
// как и pipe_read() (vfs.c) — но, в отличие от pipe, console_input_push()
// ниже явно будит ожидающего обратно в PROCESS_READY при приходе байта.
int console_input_read(uint8_t *out, int max);

// Неблокирующая проверка для SYS_POLL: есть ли что почитать прямо сейчас,
// без ухода в PROCESS_BLOCKED, как делает console_input_read().
int console_input_has_data(void);
