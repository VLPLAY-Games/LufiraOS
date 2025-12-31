#include <stdint.h>
#include "keyboard.h"
#include "../shell/shell.h"
#include "../drivers/console.h"

// Порт клавиатуры
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_COMMAND_PORT 0x64

// Глобальные переменные
char input_buffer[INPUT_BUFFER_SIZE];
uint32_t input_buffer_index = 0;

static int shift_pressed = 0;
static int caps_lock = 0;

// Таблица скан-кодов (Set 1)
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
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void keyboard_init(void) {
    // Сброс контроллера клавиатуры
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFF), "Nd"(KEYBOARD_COMMAND_PORT));
    
    // Ожидание сброса
    for (volatile int i = 0; i < 100000; i++);
    
    // Очистка буфера клавиатуры
    uint8_t temp;
    uint8_t status;
    
    while (1) {
        __asm__ volatile ("inb %1, %0" : "=a"(status) : "Nd"(KEYBOARD_STATUS_PORT));
        if (!(status & 1)) break;
        __asm__ volatile ("inb %1, %0" : "=a"(temp) : "Nd"(KEYBOARD_DATA_PORT));
    }
}

uint8_t keyboard_read_scancode(void) {
    // Ждем, пока в буфере появится данные
    uint8_t status;
    do {
        __asm__ volatile ("inb %1, %0" : "=a"(status) : "Nd"(KEYBOARD_STATUS_PORT));
    } while (!(status & 1));
    
    // Читаем скан-код
    uint8_t scancode;
    __asm__ volatile ("inb %1, %0" : "=a"(scancode) : "Nd"(KEYBOARD_DATA_PORT));
    return scancode;
}

char keyboard_scancode_to_char(uint8_t scancode) {
    if (scancode & 0x80) {
        // Код отпускания клавиши
        uint8_t keycode = scancode & 0x7F;
        
        // Обработка отпускания модификаторов
        if (keycode == 0x2A || keycode == 0x36) { // Левый или правый Shift
            shift_pressed = 0;
        }
        return 0;
    }
    
    // Обработка нажатия модификаторов
    if (scancode == 0x2A || scancode == 0x36) { // Левый или правый Shift
        shift_pressed = 1;
        return 0;
    } else if (scancode == 0x3A) { // Caps Lock
        caps_lock = !caps_lock;
        return 0;
    }
    
    // Преобразование скан-кода в символ
    char result;
    int use_shift = shift_pressed ^ caps_lock;
    
    if (use_shift && scancode < 128) {
        result = scancode_to_char_shift[scancode];
    } else if (scancode < 128) {
        result = scancode_to_char[scancode];
    } else {
        result = 0;
    }
    
    return result;
}

void process_keypress(char c) {
    if (c == 0) return;
    
    if (c == '\n') { // Enter
        put_char('\n');
        execute_command();
        show_prompt();
        return;
    }
    
    if (c == '\b') { // Backspace
        if (input_buffer_index > 0) {
            input_buffer_index--;
            put_char('\b');
        }
        return;
    }
    
    // Обычный символ
    if (input_buffer_index < INPUT_BUFFER_SIZE - 1) {
        input_buffer[input_buffer_index++] = c;
        put_char(c);
    }
}

void keyboard_handler(void) {
    uint8_t status;
    __asm__ volatile ("inb %1, %0" : "=a"(status) : "Nd"(KEYBOARD_STATUS_PORT));
    
    if (status & 1) { // Есть данные в буфере
        uint8_t scancode = keyboard_read_scancode();
        char c = keyboard_scancode_to_char(scancode);
        process_keypress(c);
    }
}