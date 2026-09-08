#include "input.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/console/console.h"
#include "shell/shell.h"

static int mouse_x = 0;
static int mouse_y = 0;
static uint8_t mouse_buttons = 0;

// Перенесено из keyboard.c без изменений — единственное отличие в том, что
// теперь этот путь общий для PS/2 и (позже) USB HID клавиатуры.
void input_keyboard_event(int key) {
    if (key == 0) return;

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
