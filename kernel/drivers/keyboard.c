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
static int ctrl_pressed = 0;
static int alt_pressed = 0;
static int extended_scancode = 0;  // Флаг для расширенных скан-кодов

// Таблица скан-кодов (Set 1) - с улучшенной обработкой
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
    
    // Включение прерываний клавиатуры
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xAE), "Nd"(KEYBOARD_COMMAND_PORT));
    
    // Сброс состояния модификаторов
    shift_pressed = 0;
    caps_lock = 0;
    ctrl_pressed = 0;
    alt_pressed = 0;
    extended_scancode = 0;
    
    // Очистка буфера клавиатуры
    uint8_t temp;
    uint8_t status;
    
    while (1) {
        __asm__ volatile ("inb %1, %0" : "=a"(status) : "Nd"(KEYBOARD_STATUS_PORT));
        if (!(status & 1)) break;
        __asm__ volatile ("inb %1, %0" : "=a"(temp) : "Nd"(KEYBOARD_DATA_PORT));
    }
    
    // Отправка команды сброса клавиатуры
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xF6), "Nd"(KEYBOARD_DATA_PORT));
    
    // Небольшая задержка
    for (volatile int i = 0; i < 50000; i++);
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
    // Обработка префикса расширенного скан-кода
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return 0;
    }
    
    // Проверяем, является ли это кодом отпускания клавиши
    if (scancode & 0x80) {
        uint8_t keycode = scancode & 0x7F;
        
        // Сбрасываем флаг расширенного кода
        if (extended_scancode) {
            extended_scancode = 0;
            return 0;
        }
        
        // Обработка отпускания модификаторов
        switch (keycode) {
            case 0x2A: // Левый Shift
            case 0x36: // Правый Shift
                shift_pressed = 0;
                break;
            case 0x1D: // Левый Ctrl
                ctrl_pressed = 0;
                break;
            case 0x38: // Левый Alt
                alt_pressed = 0;
                break;
        }
        return 0;
    }
    
    // Обработка нажатия модификаторов (только если не был префикс расширения)
    if (!extended_scancode) {
        switch (scancode) {
            case 0x2A: // Левый Shift
            case 0x36: // Правый Shift
                shift_pressed = 1;
                return 0;
            case 0x3A: // Caps Lock
                caps_lock = !caps_lock;
                return 0;
            case 0x1D: // Левый Ctrl
                ctrl_pressed = 1;
                return 0;
            case 0x38: // Левый Alt
                alt_pressed = 1;
                return 0;
        }
    } else {
        // Обработка расширенных кодов (правые Ctrl/Alt и др.)
        switch (scancode) {
            case 0x1D: // Правый Ctrl (E0 1D)
                ctrl_pressed = 1;
                extended_scancode = 0;
                return 0;
            case 0x38: // Правый Alt (E0 38)
                alt_pressed = 1;
                extended_scancode = 0;
                return 0;
            default:
                extended_scancode = 0;
                return 0;
        }
    }
    
    // Преобразование скан-кода в символ
    char result = 0;
    
    if (scancode < 128) {
        // Определяем, использовать ли shift версию
        int use_upper = shift_pressed;
        
        // Caps Lock влияет только на буквы
        if ((scancode >= 0x10 && scancode <= 0x19) ||  // q..p
            (scancode >= 0x1E && scancode <= 0x26) ||  // a..l
            (scancode >= 0x2C && scancode <= 0x32)) {  // z..m
            use_upper = use_upper ^ caps_lock; // XOR: инвертируем если caps_lock включен
        }
        
        if (use_upper) {
            result = scancode_to_char_shift[scancode];
        } else {
            result = scancode_to_char[scancode];
        }
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