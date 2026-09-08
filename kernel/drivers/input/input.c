#include "input.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/console/console.h"
#include "shell/shell.h"
#include "system/timer/pit.h"

static int mouse_x = 0;
static int mouse_y = 0;
static uint8_t mouse_buttons = 0;

// QEMU обычно доставляет один и тот же физический keystroke сразу на ОБА
// зарегистрированных устройства ввода — PS/2 (немедленно, по IRQ1) и USB
// HID-клавиатуру (опрашивается раз в тик из timer_irq_handler(), то есть
// с задержкой до ~10мс). Без фильтрации один Enter превращался в ДВА
// вызова shell_handle_enter() подряд — а поскольку второй мог стартовать
// прямо ИЗНУТРИ таймерного прерывания, вложенного в код, который сам ещё
// не успел безопасно завершиться (pmm_alloc_page() и другие функции этого
// ядра не рассчитаны на реентерабельный вызов), второй "run" реально
// портил память ядра (два разных вызова pmm_alloc_page() успевали
// получить одну и ту же "свободную" физическую страницу). Игнорируем
// повтор ТОГО ЖЕ key, если он пришёл слишком быстро — реальный
// человеческий повторный набор той же клавиши всегда медленнее.
#define KEY_DEBOUNCE_TICKS 3

static int last_key = 0;
static uint64_t last_key_tick = 0;

// Перенесено из keyboard.c без изменений — единственное отличие в том, что
// теперь этот путь общий для PS/2 и (позже) USB HID клавиатуры.
void input_keyboard_event(int key) {
    if (key == 0) return;

    uint64_t now = pit_get_ticks();
    if (key == last_key && (now - last_key_tick) < KEY_DEBOUNCE_TICKS) {
        return;
    }
    last_key = key;
    last_key_tick = now;

    switch (key) {
        case KEY_LEFT_ARROW:
            if (console_is_scrolled())
                console_scroll_to_bottom();

            shell_handle_left_arrow();
            return;

        case KEY_RIGHT_ARROW:
            if (console_is_scrolled())
                console_scroll_to_bottom();

            shell_handle_right_arrow();
            return;

        case KEY_UP_ARROW:
            if (keyboard_ctrl_pressed()) {
                console_scroll_up();
                return;
            }

            if (console_is_scrolled())
                console_scroll_to_bottom();

            shell_handle_up_arrow();
            return;

        case KEY_DOWN_ARROW:
            if (keyboard_ctrl_pressed()) {
                console_scroll_down();
                return;
            }

            if (console_is_scrolled())
                console_scroll_to_bottom();

            shell_handle_down_arrow();
            return;
        case '\t':  // Tab!
            shell_handle_tab();
            return;
    }

    if (key == '\n') {
        shell_handle_enter();
        return;
    }
    if (key == '\b') {
        shell_handle_backspace();
        return;
    }

    shell_handle_char(key);
}

void input_mouse_event(int dx, int dy, uint8_t buttons) {
    mouse_x += dx;
    mouse_y += dy;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;

    mouse_buttons = buttons;
}

int input_mouse_get_x(void) {
    return mouse_x;
}

int input_mouse_get_y(void) {
    return mouse_y;
}

uint8_t input_mouse_get_buttons(void) {
    return mouse_buttons;
}
