#include "usb_hid.h"
#include "drivers/input/input.h"
#include "drivers/keyboard/keyboard.h"

// Usage ID стрелок в таблице "Keyboard/Keypad" HID (стандартные для любой
// boot-протокольной клавиатуры).
#define HID_KEY_RIGHT_ARROW 0x4F
#define HID_KEY_LEFT_ARROW  0x50
#define HID_KEY_DOWN_ARROW  0x51
#define HID_KEY_UP_ARROW    0x52

#define HID_MOD_LEFT_SHIFT  (1 << 1)
#define HID_MOD_RIGHT_SHIFT (1 << 5)

// Usage ID 0x04..0x38 — буквы, цифры, базовые знаки препинания и
// управляющие клавиши: тот же набор символов, что и в PS/2-таблицах
// keyboard.c, просто переиндексированный под HID usage-коды вместо
// scan-кодов.
static const int hid_keycode_ascii[0x39] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x29] = 27,   [0x2A] = '\b', [0x2B] = '\t', [0x2C] = ' ',
    [0x2D] = '-',  [0x2E] = '=',  [0x2F] = '[',  [0x30] = ']',  [0x31] = '\\',
    [0x33] = ';',  [0x34] = '\'', [0x35] = '`',  [0x36] = ',',  [0x37] = '.',
    [0x38] = '/',
};

static const int hid_keycode_ascii_shift[0x39] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y',
    [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$', [0x22] = '%',
    [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',
    [0x28] = '\n', [0x29] = 27,  [0x2A] = '\b', [0x2B] = '\t', [0x2C] = ' ',
    [0x2D] = '_',  [0x2E] = '+', [0x2F] = '{',  [0x30] = '}',  [0x31] = '|',
    [0x33] = ':',  [0x34] = '"', [0x35] = '~',  [0x36] = '<',  [0x37] = '>',
    [0x38] = '?',
};

// Usage-коды из ПРЕДЫДУЩЕГО отчёта — нужны только для того, чтобы отличить
// "клавиша только что нажата" от "клавиша всё ещё удерживается" (иначе при
// удержании событие сыпалось бы на каждый опрос, ~каждые 10мс).
static uint8_t last_keys[6];

static int hid_usage_was_pressed(uint8_t usage) {
    for (int i = 0; i < 6; i++) {
        if (last_keys[i] == usage) return 1;
    }
    return 0;
}

static int hid_decode_usage(uint8_t usage, int shift) {
    switch (usage) {
        case HID_KEY_LEFT_ARROW:  return KEY_LEFT_ARROW;
        case HID_KEY_RIGHT_ARROW: return KEY_RIGHT_ARROW;
        case HID_KEY_UP_ARROW:    return KEY_UP_ARROW;
        case HID_KEY_DOWN_ARROW:  return KEY_DOWN_ARROW;
    }
    if (usage < 0x39) {
        return shift ? hid_keycode_ascii_shift[usage] : hid_keycode_ascii[usage];
    }
    return 0; // F-клавиши, Caps/Num/Scroll Lock и прочее boot-протоколом не разбираем
}

void usb_hid_keyboard_report(const uint8_t report[8]) {
    uint8_t modifier = report[0];
    const uint8_t *keys = &report[2];
    int shift = (modifier & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;

    for (int i = 0; i < 6; i++) {
        uint8_t usage = keys[i];
        if (usage <= 1) continue; // 0 = нет клавиши, 1 = rollover error
        if (hid_usage_was_pressed(usage)) continue; // уже было нажато, не новое событие

        int key = hid_decode_usage(usage, shift);
        if (key != 0) {
            input_keyboard_event(key);
        }
    }

    for (int i = 0; i < 6; i++) last_keys[i] = keys[i];
}

void usb_hid_mouse_report(const uint8_t *report, int len) {
    if (len < 3) return; // короче минимального boot-отчёта (кнопки+dx+dy) — мусор

    uint8_t buttons = report[0] & 0x07;
    int dx = (int8_t)report[1];
    int dy = (int8_t)report[2];

    input_mouse_event(dx, dy, buttons);
}
