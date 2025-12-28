/* keyboard.h - драйвер клавиатуры для LufiraOS */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* Scan Code Set 1 (make codes) */
#define KEY_ESC        0x01
#define KEY_BACKSPACE  0x0E
#define KEY_ENTER      0x1C
#define KEY_LCTRL      0x1D
#define KEY_LSHIFT     0x2A
#define KEY_RSHIFT     0x36
#define KEY_LALT       0x38
#define KEY_CAPS       0x3A
#define KEY_F1         0x3B
#define KEY_F2         0x3C
#define KEY_F3         0x3D
#define KEY_F4         0x3E
#define KEY_F5         0x3F
#define KEY_F6         0x40
#define KEY_F7         0x41
#define KEY_F8         0x42
#define KEY_F9         0x43
#define KEY_F10        0x44
#define KEY_F11        0x57
#define KEY_F12        0x58

/* scancode values for extended keys (after 0xE0) */
#define KEY_UP_SC      0x48
#define KEY_DOWN_SC    0x50
#define KEY_LEFT_SC    0x4B
#define KEY_RIGHT_SC   0x4D

/* special codes returned by keyboard_getchar() for arrows */
#define KBD_ARROW_UP     0x80
#define KBD_ARROW_DOWN   0x81
#define KBD_ARROW_LEFT   0x82
#define KBD_ARROW_RIGHT  0x83

/* Порт клавиатуры */
#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/* Состояния клавиш */
typedef struct {
    uint8_t shift_pressed;
    uint8_t ctrl_pressed;
    uint8_t alt_pressed;
    uint8_t caps_lock;
} keyboard_state_t;

/* Инициализация клавиатуры */
void keyboard_init(void);

/* Проверить, доступен ли символ */
int keyboard_available(void);

/* Получить символ с клавиатуры
 * Возвращаемое значение:
 *   0         - ничего полезного (игнорировать)
 *   1..127    - обычный ASCII-символ
 *   0x80..0x83 - специальные коды стрелок (см. KBD_ARROW_*)
 */
int keyboard_getchar(void);

/* Получить строку с клавиатуры */
void keyboard_getline(char* buffer, uint32_t max_length);

#endif /* KEYBOARD_H */
