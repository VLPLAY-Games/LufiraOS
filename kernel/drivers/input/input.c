#include "input.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/console/console.h"
#include "shell/shell.h"
#include "system/timer/pit.h"

static int mouse_x = 0;
static int mouse_y = 0;
static uint8_t mouse_buttons = 0;

// QEMU доставляет один и тот же keystroke сразу на PS/2 (IRQ1) и на USB HID
// (опрашивается из timer_irq_handler(), с задержкой до ~10мс) — без
// фильтрации один Enter превращался в два вызова shell_handle_enter()
// подряд, а второй мог стартовать прямо изнутри таймерного прерывания и
// портить память ядра (pmm_alloc_page() не рассчитан на реентерабельный
// вызов). Игнорируем повтор того же key, если он пришёл слишком быстро.
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

        case KEY_CTRL_C:
            if (console_is_scrolled())
                console_scroll_to_bottom();

            shell_handle_ctrl_c();
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
