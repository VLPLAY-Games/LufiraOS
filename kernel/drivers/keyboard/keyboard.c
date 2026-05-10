#include "lib/types.h"
#include "keyboard.h"
#include "shell/shell.h"
#include "drivers/console/console.h"

// Порт клавиатуры
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_COMMAND_PORT 0x64

// Вспомогательные функции портов (нужны только здесь)
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Глобальные переменные
char input_buffer[INPUT_BUFFER_SIZE];
uint32_t input_buffer_index = 0;

static int shift_pressed = 0;
static int caps_lock = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;
static int extended_scancode = 0;
static int keyboard_initialized = 0;

// Таблицы скан-кодов (без изменений)
static const char scancode_to_char[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char scancode_to_char_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void keyboard_init(void) {
    // Сброс контроллера клавиатуры
    outb(KEYBOARD_COMMAND_PORT, 0xFF);
    for (volatile int i = 0; i < 100000; i++);

    // Включение прерываний клавиатуры
    outb(KEYBOARD_COMMAND_PORT, 0xAE);
    
    // Проверка наличия клавиатуры
    outb(KEYBOARD_COMMAND_PORT, 0xAA);  // self-test
    for (volatile int i = 0; i < 100000; i++);
    uint8_t test_result = inb(KEYBOARD_DATA_PORT);
    
    if (test_result != 0x55) {
        keyboard_initialized = 0;
        return;
    }

    // Сброс состояния модификаторов
    shift_pressed = 0;
    caps_lock = 0;
    ctrl_pressed = 0;
    alt_pressed = 0;
    extended_scancode = 0;

    // Очистка буфера клавиатуры
    uint8_t status;
    while (1) {
        status = inb(KEYBOARD_STATUS_PORT);
        if (!(status & 1)) break;
        (void)inb(KEYBOARD_DATA_PORT);
    }

    // Отправка команды сброса клавиатуры
    outb(KEYBOARD_COMMAND_PORT, 0xD4);  // write to keyboard
    for (volatile int i = 0; i < 10000; i++);
    outb(KEYBOARD_DATA_PORT, 0xF6);    // reset command
    for (volatile int i = 0; i < 50000; i++);
    
    // Ждём ACK
    uint8_t ack = 0;
    int timeout = 1000000;
    while (--timeout) {
        if (inb(KEYBOARD_STATUS_PORT) & 1) {
            ack = inb(KEYBOARD_DATA_PORT);
            break;
        }
    }
    
    if (ack == 0xFA) {
        keyboard_initialized = 1;
    } else {
        keyboard_initialized = 0;
    }
}


// Чтение скан-кода из порта (без ожидания)
uint8_t keyboard_read_scancode(void) {
    return inb(KEYBOARD_DATA_PORT);
}

int keyboard_scancode_to_key(uint8_t scancode) {
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return 0;
    }

    if (scancode & 0x80) {
        uint8_t keycode = scancode & 0x7F;
        if (extended_scancode) {
            extended_scancode = 0;
            return 0;
        }
        switch (keycode) {
            case 0x2A: case 0x36: shift_pressed = 0; break;
            case 0x1D: ctrl_pressed = 0; break;
            case 0x38: alt_pressed = 0; break;
        }
        return 0;
    }

    if (!extended_scancode) {
        switch (scancode) {
            case 0x2A: case 0x36: shift_pressed = 1; return 0;
            case 0x3A: caps_lock = !caps_lock; return 0;
            case 0x1D: ctrl_pressed = 1; return 0;
            case 0x38: alt_pressed = 1; return 0;
        }
    } else {
        switch (scancode) {
            case 0x4B: extended_scancode = 0; return KEY_LEFT_ARROW;
            case 0x4D: extended_scancode = 0; return KEY_RIGHT_ARROW;
            case 0x48: extended_scancode = 0; return KEY_UP_ARROW;
            case 0x50: extended_scancode = 0; return KEY_DOWN_ARROW;
            case 0x1D: ctrl_pressed = 1; extended_scancode = 0; return 0;
            case 0x38: alt_pressed = 1; extended_scancode = 0; return 0;
            default: extended_scancode = 0; return 0;
        }
    }

    int result = 0;
    if (scancode < 128) {
        int use_upper = shift_pressed;
        if ((scancode >= 0x10 && scancode <= 0x19) ||
            (scancode >= 0x1E && scancode <= 0x26) ||
            (scancode >= 0x2C && scancode <= 0x32)) {
            use_upper = use_upper ^ caps_lock;
        }
        result = use_upper ? scancode_to_char_shift[scancode]
                          : scancode_to_char[scancode];
    }
    return result;
}

void process_keypress(int key) {
    if (key == 0) return;

    switch (key) {
        case KEY_LEFT_ARROW:
            shell_handle_left_arrow();
            return;
        case KEY_RIGHT_ARROW:
            shell_handle_right_arrow();
            return;
        case KEY_UP_ARROW:
            shell_handle_up_arrow();
            return;
        case KEY_DOWN_ARROW:
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

// ============ НОВЫЙ ОБРАБОТЧИК ПРЕРЫВАНИЯ IRQ1 ============
void keyboard_irq_handler(void) {
    uint8_t status = inb(KEYBOARD_STATUS_PORT);
    if (!(status & 1))
        return;

    uint8_t scancode = keyboard_read_scancode();
    int key = keyboard_scancode_to_key(scancode);
    process_keypress(key);
}

int keyboard_is_initialized(void) {
    return keyboard_initialized;
}
