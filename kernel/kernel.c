#include <stdint.h>
#include <stdarg.h>

// --- Структуры данных ---
typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
} BootInfo;

// --- Встроенный шрифт 8x8 пикселей ---
static unsigned char full_font_data[][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* (space) */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00}, /* ! */
    {0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* " */
    {0x24, 0x7E, 0x24, 0x7E, 0x24, 0x00, 0x00, 0x00}, /* # */
    {0x24, 0x3E, 0x6C, 0x3E, 0x0C, 0x78, 0x24, 0x00}, /* $ */
    {0x00, 0x66, 0x6C, 0x18, 0x30, 0x36, 0x66, 0x00}, /* % */
    {0x3C, 0x42, 0x5A, 0x5A, 0x5A, 0x24, 0x00, 0x00}, /* & */
    {0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ' */
    {0x0C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x0C, 0x00}, /* ( */
    {0x30, 0x18, 0x18, 0x18, 0x18, 0x18, 0x30, 0x00}, /* ) */
    {0x00, 0x00, 0x24, 0x7E, 0x24, 0x00, 0x00, 0x00}, /* * */
    {0x00, 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00}, /* + */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, /* , */
    {0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, /* . */
    {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00}, /* / */
    {0x3C, 0x66, 0x6E, 0x76, 0x6E, 0x66, 0x3C, 0x00}, /* 0 */
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* 1 */
    {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00}, /* 2 */
    {0x3C, 0x06, 0x06, 0x1C, 0x06, 0x06, 0x3C, 0x00}, /* 3 */
    {0x0C, 0x1C, 0x2C, 0x4C, 0x7E, 0x0C, 0x0C, 0x00}, /* 4 */
    {0x7E, 0x60, 0x60, 0x7C, 0x06, 0x06, 0x3C, 0x00}, /* 5 */
    {0x3C, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00}, /* 6 */
    {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x60, 0x00}, /* 7 */
    {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}, /* 8 */
    {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x3C, 0x00}, /* 9 */
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00}, /* : */
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30}, /* ; */
    {0x00, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x00, 0x00}, /* < */
    {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}, /* = */
    {0x00, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x00, 0x00}, /* > */
    {0x3C, 0x66, 0x66, 0x0C, 0x18, 0x00, 0x18, 0x00}, /* ? */
    {0x3C, 0x42, 0x7E, 0x7E, 0x7E, 0x42, 0x3C, 0x00}, /* @ */
    {0x18, 0x24, 0x24, 0x24, 0x3C, 0x24, 0x24, 0x00}, /* A */
    {0x3C, 0x24, 0x24, 0x3C, 0x24, 0x24, 0x3C, 0x00}, /* B */
    {0x3C, 0x42, 0x40, 0x40, 0x40, 0x42, 0x3C, 0x00}, /* C */
    {0x3C, 0x24, 0x24, 0x24, 0x24, 0x24, 0x3C, 0x00}, /* D */
    {0x3C, 0x40, 0x40, 0x3C, 0x40, 0x40, 0x3C, 0x00}, /* E */
    {0x3C, 0x40, 0x40, 0x3C, 0x40, 0x40, 0x40, 0x00}, /* F */
    {0x3C, 0x42, 0x40, 0x4e, 0x42, 0x42, 0x3C, 0x00}, /* G */
    {0x24, 0x24, 0x24, 0x3C, 0x24, 0x24, 0x24, 0x00}, /* H */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* I */
    {0x0C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x7C, 0x00}, /* J */
    {0x24, 0x28, 0x30, 0x18, 0x30, 0x28, 0x24, 0x00}, /* K */
    {0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x3C, 0x00}, /* L */
    {0x42, 0x66, 0x7E, 0x7E, 0x66, 0x42, 0x42, 0x00}, /* M */
    {0x42, 0x62, 0x72, 0x52, 0x4A, 0x46, 0x42, 0x00}, /* N */
    {0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00}, /* O */
    {0x3C, 0x24, 0x24, 0x3C, 0x20, 0x20, 0x20, 0x00}, /* P */
    {0x3C, 0x42, 0x42, 0x42, 0x4A, 0x46, 0x3E, 0x00}, /* Q */
    {0x3C, 0x24, 0x24, 0x3C, 0x28, 0x24, 0x24, 0x00}, /* R */
    {0x3C, 0x40, 0x40, 0x3C, 0x02, 0x02, 0x3C, 0x00}, /* S */
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* T */
    {0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00}, /* U */
    {0x42, 0x42, 0x42, 0x42, 0x42, 0x24, 0x18, 0x00}, /* V */
    {0x42, 0x42, 0x42, 0x66, 0x7E, 0x7E, 0x24, 0x00}, /* W */
    {0x42, 0x24, 0x18, 0x18, 0x18, 0x24, 0x42, 0x00}, /* X */
    {0x42, 0x24, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* Y */
    {0x7E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x7E, 0x00}, /* Z */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* [ */
    {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00}, /* \ */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* ] */
    {0x00, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ^ */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E}, /* _ */
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ` */
    {0x00, 0x00, 0x3C, 0x42, 0x3E, 0x02, 0x7C, 0x00}, /* a */
    {0x40, 0x40, 0x3C, 0x42, 0x42, 0x42, 0x3C, 0x00}, /* b */
    {0x00, 0x00, 0x3C, 0x42, 0x40, 0x42, 0x3C, 0x00}, /* c */
    {0x02, 0x02, 0x3C, 0x42, 0x42, 0x42, 0x3C, 0x00}, /* d */
    {0x00, 0x00, 0x3C, 0x42, 0x3C, 0x40, 0x3C, 0x00}, /* e */
    {0x1C, 0x20, 0x20, 0x7C, 0x20, 0x20, 0x20, 0x00}, /* f */
    {0x00, 0x00, 0x3C, 0x42, 0x42, 0x3E, 0x02, 0x7C}, /* g */
    {0x40, 0x40, 0x3C, 0x42, 0x42, 0x42, 0x42, 0x00}, /* h */
    {0x00, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00, 0x00}, /* i */
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x30}, /* j */
    {0x40, 0x40, 0x42, 0x44, 0x48, 0x44, 0x42, 0x00}, /* k */
    {0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00}, /* l */
    {0x00, 0x00, 0x42, 0x66, 0x66, 0x5A, 0x42, 0x00}, /* m */
    {0x00, 0x00, 0x3C, 0x42, 0x42, 0x42, 0x42, 0x00}, /* n */
    {0x00, 0x00, 0x3C, 0x42, 0x42, 0x42, 0x3C, 0x00}, /* o */
    {0x00, 0x00, 0x3C, 0x42, 0x42, 0x3C, 0x40, 0x40}, /* p */
    {0x00, 0x00, 0x3C, 0x42, 0x42, 0x3C, 0x02, 0x02}, /* q */
    {0x00, 0x00, 0x3C, 0x42, 0x40, 0x40, 0x40, 0x00}, /* r */
    {0x00, 0x00, 0x3C, 0x40, 0x3C, 0x02, 0x3C, 0x00}, /* s */
    {0x00, 0x20, 0x20, 0x7C, 0x20, 0x20, 0x00, 0x00}, /* t */
    {0x00, 0x00, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00}, /* u */
    {0x00, 0x00, 0x42, 0x42, 0x42, 0x24, 0x18, 0x00}, /* v */
    {0x00, 0x00, 0x42, 0x42, 0x5A, 0x5A, 0x24, 0x00}, /* w */
    {0x00, 0x00, 0x42, 0x24, 0x18, 0x24, 0x42, 0x00}, /* x */
    {0x00, 0x00, 0x42, 0x42, 0x42, 0x3E, 0x02, 0x7C}, /* y */
    {0x00, 0x00, 0x7C, 0x04, 0x08, 0x10, 0x7C, 0x00}, /* z */
    {0x10, 0x18, 0x14, 0x10, 0x14, 0x18, 0x10, 0x00}, /* { */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* | */
    {0x08, 0x0C, 0x14, 0x08, 0x14, 0x0C, 0x08, 0x00}, /* } */
    {0x00, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ~ */
};

// --- Глобальные переменные состояния консоли ---
static uint32_t current_x = 0;
static uint32_t current_y = 0;
static uint32_t screen_width_chars;
static uint32_t screen_height_chars;
static uint32_t* framebuffer;
static uint32_t current_color = 0xFFFFFF;
static uint32_t current_bg_color = 0x000000;
static uint32_t pixels_per_scan_line;

// --- Структуры и переменные для клавиатуры ---
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_COMMAND_PORT 0x64

// Буфер ввода командной строки
#define INPUT_BUFFER_SIZE 256
static char input_buffer[INPUT_BUFFER_SIZE];
static uint32_t input_buffer_index = 0;

// Таблица скан-кодов (Set 1) - только основные клавиши
static const char scancode_to_char[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Shift-версия (грубая реализация)
static const char scancode_to_char_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static int shift_pressed = 0;
static int caps_lock = 0;

// --- Прототипы функций ---
void initialize_console(BootInfo* bi);
void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void put_char_graphic(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);
void put_char(char c);
void print_string(const char* str);
void printf(const char* format, ...);
void clear_screen(void);
void scroll_screen(void);
void utoa(uint64_t value, char* buffer, int base);
void keyboard_init(void);
void keyboard_handler(void);
uint8_t keyboard_read_scancode(void);
char keyboard_scancode_to_char(uint8_t scancode);
void process_keypress(char c);
void execute_command(void);
void show_prompt(void);
int strcmp(const char* s1, const char* s2);
char to_lower(char c);  // Новая функция для приведения к нижнему регистру
int strcmp_case_insensitive(const char* s1, const char* s2);  // Новая функция для сравнения без учета регистра

// --- Точка входа ядра ---
__attribute__((section(".text.prologue")))
void _start(BootInfo* bi) {
    initialize_console(bi);
    clear_screen();
    keyboard_init();
    
    current_y = 1;
    
    current_color = 0xAAAAAA;
    printf("LufiraOS Kernel v1.0 Boot Sequence:\n");
    printf("-----------------------------------\n\n");
    
    current_color = 0xFFFFFF;
    printf("Detected Resolution: %d x %d\n", bi->HorizontalResolution, bi->VerticalResolution);
    printf("Framebuffer Address: 0x%x\n", bi->FrameBufferBase);
    printf("Characters Grid: %d x %d\n\n", screen_width_chars, screen_height_chars);
    
    current_color = 0x55FF55;
    printf("Keyboard initialized: OK\n");
    printf("System status: OK.\n");
    
    current_color = 0xFFFFFF;
    show_prompt();
    
    while (1) {
        // Проверяем клавиатуру
        keyboard_handler();
        __asm__ volatile ("pause"); // Замена hlt для экономии энергии
    }
}

// --- Реализация функций консоли ---
void initialize_console(BootInfo* bi) {
    framebuffer = (uint32_t*)bi->FrameBufferBase;
    pixels_per_scan_line = bi->PixelsPerScanLine;
    screen_width_chars = bi->HorizontalResolution / 9;
    screen_height_chars = bi->VerticalResolution / 9;
    current_x = 0;
    current_y = 0;
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= screen_width_chars * 8 || y >= screen_height_chars * 8) return;
    framebuffer[y * pixels_per_scan_line + x] = color;
}

void put_char_graphic(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if (c < 32 || c > 127) c = '?';
    
    unsigned char* glyph = full_font_data[c - 32];
    const uint32_t char_width = 8;
    const uint32_t char_height = 8;
    const uint32_t padding_x = 1;
    const uint32_t padding_y = 1;
    
    for (uint32_t cy = 0; cy < char_height + padding_y; cy++) {
        for (uint32_t cx = 0; cx < char_width + padding_x; cx++) {
            uint32_t target_x = x * (char_width + padding_x) + cx;
            uint32_t target_y = y * (char_height + padding_y) + cy;
            
            if (cx < char_width && cy < char_height) {
                if ((glyph[cy] >> (7 - cx)) & 1) {
                    put_pixel(target_x, target_y, fg_color);
                } else {
                    put_pixel(target_x, target_y, bg_color);
                }
            } else {
                put_pixel(target_x, target_y, bg_color);
            }
        }
    }
}

void put_char(char c) {
    if (c == '\n') {
        current_x = 0;
        current_y++;
    } else if (c == '\b') { // Backspace
        if (current_x > 0) {
            current_x--;
            put_char_graphic(' ', current_x, current_y, current_color, current_bg_color);
        }
    } else {
        put_char_graphic(c, current_x, current_y, current_color, current_bg_color);
        current_x++;
    }
    
    if (current_x >= screen_width_chars) {
        current_x = 0;
        current_y++;
    }
    
    if (current_y >= screen_height_chars) {
        scroll_screen();
        current_y = screen_height_chars - 1;
        current_x = 0;
    }
}

void scroll_screen(void) {
    // Упрощенная прокрутка: очищаем экран
    clear_screen();
    current_x = 0;
    current_y = screen_height_chars - 5; // Оставляем место для приглашения
}

void clear_screen(void) {
    for (uint32_t y = 0; y < screen_height_chars * 8; y++) {
        for (uint32_t x = 0; x < screen_width_chars * 8; x++) {
            put_pixel(x, y, current_bg_color);
        }
    }
    current_x = 0;
    current_y = 0;
}

void print_string(const char* str) {
    while (*str) {
        put_char(*str++);
    }
}

// --- Реализация printf ---
void utoa(uint64_t value, char* buffer, int base) {
    char* original_buffer = buffer;
    uint64_t temp = value;
    int digits = 0;
    
    do {
        digits++;
        temp /= base;
    } while (temp > 0);
    
    buffer += digits;
    *buffer = '\0';
    
    do {
        *(--buffer) = "0123456789abcdef"[value % base];
        value /= base;
    } while (value > 0);
}

void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    char buffer[20];
    
    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == 's') {
                char* str = va_arg(args, char*);
                print_string(str);
            } else if (*format == 'd' || *format == 'u') {
                uint64_t val = va_arg(args, uint64_t);
                utoa(val, buffer, 10);
                print_string(buffer);
            } else if (*format == 'x' || *format == 'p' || *format == 'l') {
                uint64_t val = va_arg(args, uint64_t);
                utoa(val, buffer, 16);
                print_string("0x");
                print_string(buffer);
            } else if (*format == '%') {
                put_char('%');
            }
        } else {
            put_char(*format);
        }
        format++;
    }
    va_end(args);
}

// --- Реализация поддержки клавиатуры ---
void keyboard_init(void) {
    // Сброс контроллера клавиатуры
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFF), "Nd"(KEYBOARD_COMMAND_PORT));
    
    // Ожидание сброса
    for (volatile int i = 0; i < 100000; i++);
    
    // Включение прерываний клавиатуры (если будем использовать прерывания)
    // __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xAE), "Nd"(KEYBOARD_COMMAND_PORT));
    
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
    int use_shift = shift_pressed ^ caps_lock; // XOR: если shift или caps нажат
    
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

// Простая реализация strcmp
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// Функция приведения символа к нижнему регистру
char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

// Функция сравнения строк без учета регистра
int strcmp_case_insensitive(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = to_lower(*s1);
        char c2 = to_lower(*s2);
        
        if (c1 != c2) {
            return c1 - c2;
        }
        
        s1++;
        s2++;
    }
    
    // Если одна строка закончилась, а другая нет
    return to_lower(*s1) - to_lower(*s2);
}

void execute_command(void) {
    input_buffer[input_buffer_index] = '\0';
    
    if (input_buffer_index == 0) return;
    
    // Используем сравнение без учета регистра
    if (strcmp_case_insensitive(input_buffer, "help") == 0) {
        printf("\nAvailable commands:\n");
        printf("  help    - Show this help\n");
        printf("  clear   - Clear screen\n");
        printf("  reboot  - Reboot system\n");
        printf("  version - Show kernel version\n");
        printf("  echo    - Echo text back\n");
    } else if (strcmp_case_insensitive(input_buffer, "clear") == 0) {
        clear_screen();
        show_prompt();
    } else if (strcmp_case_insensitive(input_buffer, "reboot") == 0) {
        printf("\nRebooting...\n");
        // Перезагрузка через 8042 контроллер
        __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    } else if (strcmp_case_insensitive(input_buffer, "version") == 0) {
        printf("\nLufiraOS Kernel v1.0\n");
        printf("Built: %s %s\n", __DATE__, __TIME__);
    } else if (strcmp_case_insensitive(input_buffer, "echo") == 0) {
        printf("\nUsage: echo <text>\n");
    } else if (strcmp_case_insensitive(input_buffer, "echo test") == 0) {
        printf("\nTest successful!\n");
    } else {
        // Проверим, не начинается ли команда с "echo "
        int echo_prefix = 1;
        for (int i = 0; i < 5; i++) {
            if (to_lower(input_buffer[i]) != "echo "[i]) {
                echo_prefix = 0;
                break;
            }
        }
        
        if (echo_prefix && input_buffer[4] == ' ') {
            // Это команда echo с аргументами
            printf("\n");
            // Пропускаем "echo " и выводим остальное
            for (int i = 5; i < input_buffer_index; i++) {
                put_char(input_buffer[i]);
            }
            put_char('\n');
        } else {
            printf("\nUnknown command: %s\n", input_buffer);
            printf("Type 'help' for available commands.\n");
        }
    }
    
    // Очищаем буфер
    input_buffer_index = 0;
}

void show_prompt(void) {
    current_color = 0xFFFFFF;
    printf("\n> ");
    
    // Сбрасываем состояние ввода
    input_buffer_index = 0;
    for (int i = 0; i < INPUT_BUFFER_SIZE; i++) {
        input_buffer[i] = 0;
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