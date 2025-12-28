/* keyboard.c - драйвер клавиатуры для LufiraOS */

#include "keyboard.h"
#include "shell.h"     /* для terminal_putchar/writestring */
#include <stdint.h>

/* Таблица скан-кодов для обычных клавиш (без Shift) */
static const char scan_code_table[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

/* Таблица скан-кодов для клавиш с Shift */
static const char scan_code_table_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,
    0, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

/* Состояние клавиатуры */
static keyboard_state_t kbd_state = {0};

/* Флаг для обработки расширенных скан-кодов (E0 префикс) */
static int extended_scancode = 0;

/* Проверить, нажата ли клавиша (Output buffer full) */
static inline uint8_t keyboard_is_key_pressed(void) {
    uint8_t status;
    __asm__ volatile("inb %1, %0" : "=a"(status) : "dN"(KEYBOARD_STATUS_PORT));
    return status & 0x01;
}

/* Прочитать скан-код */
static inline uint8_t keyboard_read_scancode(void) {
    uint8_t scancode;
    __asm__ volatile("inb %1, %0" : "=a"(scancode) : "dN"(KEYBOARD_DATA_PORT));
    return scancode;
}

/* Инициализация клавиатуры */
void keyboard_init(void) {
    kbd_state.shift_pressed = 0;
    kbd_state.ctrl_pressed = 0;
    kbd_state.alt_pressed = 0;
    kbd_state.caps_lock = 0;
    extended_scancode = 0;
}

/* Проверить, доступен ли символ */
int keyboard_available(void) {
    return keyboard_is_key_pressed() ? 1 : 0;
}

/* Получить один "символ" (или специальный код).
 * Возвращает int (0 - нет данных/игнорировать, >0 - код).
 */
int keyboard_getchar(void) {
    /* Ждём байт со скан-кодом */
    while (!keyboard_is_key_pressed()) {
        __asm__ volatile("pause");
    }

    uint8_t scancode = keyboard_read_scancode();

    /* Обработка префикса E0 для расширенных скан-кодов */
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return 0;
    }

    /* Если это отпускание клавиши (break code) */
    if (scancode & 0x80) {
        uint8_t key = scancode & 0x7F;

        if (extended_scancode) {
            /* Сброс флага расширенного кода (на отпускании E0-пара) */
            extended_scancode = 0;
            return 0;
        }

        /* Обработка отпускания modifier'ов */
        if (key == KEY_LSHIFT || key == KEY_RSHIFT) {
            kbd_state.shift_pressed = 0;
        } else if (key == KEY_LCTRL) {
            kbd_state.ctrl_pressed = 0;
        } else if (key == KEY_LALT) {
            kbd_state.alt_pressed = 0;
        }
        return 0;
    }

    /* Если это расширенный (E0) make-code */
    if (extended_scancode) {
        extended_scancode = 0;
        switch (scancode) {
            case KEY_UP_SC:    return KBD_ARROW_UP;
            case KEY_DOWN_SC:  return KBD_ARROW_DOWN;
            case KEY_LEFT_SC:  return KBD_ARROW_LEFT;
            case KEY_RIGHT_SC: return KBD_ARROW_RIGHT;
            default:
                return 0;
        }
    }

    /* Обработка нажатия модификаторов и специальных клавиш */
    if (scancode == KEY_LSHIFT || scancode == KEY_RSHIFT) {
        kbd_state.shift_pressed = 1;
        return 0;
    } else if (scancode == KEY_LCTRL) {
        kbd_state.ctrl_pressed = 1;
        return 0;
    } else if (scancode == KEY_LALT) {
        kbd_state.alt_pressed = 1;
        return 0;
    } else if (scancode == KEY_CAPS) {
        kbd_state.caps_lock = !kbd_state.caps_lock;
        return 0;
    } else if (scancode == KEY_BACKSPACE) {
        return '\b';
    } else if (scancode == KEY_ENTER) {
        return '\n';
    } else if (scancode == KEY_ESC) {
        return 27;
    }

    /* Преобразование скан-кода в символ */
    if (scancode < sizeof(scan_code_table)) {
        char result = 0;
        if (kbd_state.shift_pressed) {
            result = scan_code_table_shift[scancode];
        } else {
            result = scan_code_table[scancode];
        }

        /* Учет Caps Lock */
        if (kbd_state.caps_lock && result >= 'a' && result <= 'z') {
            if (!kbd_state.shift_pressed) {
                result = result - 'a' + 'A';
            } else {
                result = result - 'A' + 'a';
            }
        }

        return (int)result;
    }

    return 0;
}

/* Получить строку с клавиатуры */
void keyboard_getline(char* buffer, uint32_t max_length) {
    uint32_t index = 0;
    int c;

    while (1) {
        c = keyboard_getchar();

        if (c == 0) continue; /* игнорируем промежуточные/пустые значения */

        if (c == '\n') {
            buffer[index] = '\0';
            terminal_putchar('\n');
            break;
        } else if (c == '\b') {
            if (index > 0) {
                index--;
                terminal_putchar('\b');
            }
        } else if (c > 0 && c < 0x80 && index < max_length - 1) {
            buffer[index++] = (char)c;
            terminal_putchar((char)c);
        }
    }
}
