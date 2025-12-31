#include <stdint.h>
#include <stdarg.h>
#include "console.h"

// Глобальные переменные состояния консоли
uint32_t current_x = 0;
uint32_t current_y = 0;
uint32_t screen_width_chars;
uint32_t screen_height_chars;
uint32_t* framebuffer;
uint32_t current_color = 0xFFFFFF;
uint32_t current_bg_color = 0x000000;
uint32_t pixels_per_scan_line;
uint32_t screen_width_pixels;
uint32_t screen_height_pixels;
uint32_t pixel_format = 0;

// Размеры символа 8x8
#define CHAR_WIDTH 8
#define CHAR_HEIGHT 8
#define CHAR_PADDING_X 1
#define CHAR_PADDING_Y 1

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

// Преобразование цвета из RGB в BGR если нужно
uint32_t convert_color(uint32_t color) {
    if (pixel_format == 0) {
        return color;
    } else {
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        return (b << 16) | (g << 8) | r;
    }
}

void initialize_console(BootInfo* bi) {
    framebuffer = (uint32_t*)bi->FrameBufferBase;
    pixels_per_scan_line = bi->PixelsPerScanLine;
    screen_width_pixels = bi->HorizontalResolution;
    screen_height_pixels = bi->VerticalResolution;
    pixel_format = bi->PixelFormat;
    
    // Преобразуем стандартные цвета
    current_color = convert_color(0xFFFFFF);
    current_bg_color = convert_color(0x000000);
    
    // Вычисляем количество символов, которые поместятся на экране
    // Каждый символ: 8 пикселей + 1 пиксель отступ = 9 пикселей
    screen_width_chars = screen_width_pixels / (CHAR_WIDTH + CHAR_PADDING_X);
    screen_height_chars = screen_height_pixels / (CHAR_HEIGHT + CHAR_PADDING_Y);
    
    // Гарантируем хотя бы минимальный размер
    if (screen_width_chars < 10) screen_width_chars = 10;
    if (screen_height_chars < 10) screen_height_chars = 10;
    
    current_x = 0;
    current_y = 0;
    
    // Очищаем весь экран при инициализации
    clear_entire_screen();
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= screen_width_pixels || y >= screen_height_pixels) return;
    framebuffer[y * pixels_per_scan_line + x] = color;
}

// Отрисовка символа 8x8
void put_char_graphic(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if (c < 32 || c > 127) c = '?';
    
    unsigned char* glyph = full_font_data[c - 32];
    
    // Переводим позиции символа в пиксели
    uint32_t base_x = x * (CHAR_WIDTH + CHAR_PADDING_X);
    uint32_t base_y = y * (CHAR_HEIGHT + CHAR_PADDING_Y);
    
    // Ограничиваем область отрисовки размерами экрана
    if (base_x >= screen_width_pixels || base_y >= screen_height_pixels) return;
    
    // Преобразуем цвета если нужно
    uint32_t converted_fg = fg_color;
    uint32_t converted_bg = bg_color;
    
    for (uint32_t cy = 0; cy < CHAR_HEIGHT + CHAR_PADDING_Y; cy++) {
        uint32_t target_y = base_y + cy;
        if (target_y >= screen_height_pixels) break;
        
        for (uint32_t cx = 0; cx < CHAR_WIDTH + CHAR_PADDING_X; cx++) {
            uint32_t target_x = base_x + cx;
            if (target_x >= screen_width_pixels) break;
            
            if (cx < CHAR_WIDTH && cy < CHAR_HEIGHT) {
                // Внутри символа: проверяем бит шрифта
                if ((glyph[cy] >> (7 - cx)) & 1) {
                    put_pixel(target_x, target_y, converted_fg);
                } else {
                    put_pixel(target_x, target_y, converted_bg);
                }
            } else {
                // Отступы заливаем цветом фона
                put_pixel(target_x, target_y, converted_bg);
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
    uint32_t char_height_pixels = CHAR_HEIGHT + CHAR_PADDING_Y;
    
    // Сдвигаем содержимое экрана вверх на высоту одного символа
    for (uint32_t y = char_height_pixels; y < screen_height_pixels; y++) {
        for (uint32_t x = 0; x < screen_width_pixels; x++) {
            uint32_t src_pixel = framebuffer[y * pixels_per_scan_line + x];
            framebuffer[(y - char_height_pixels) * pixels_per_scan_line + x] = src_pixel;
        }
    }
    
    // Очищаем последнюю строку
    uint32_t last_line_start = screen_height_pixels - char_height_pixels;
    for (uint32_t y = last_line_start; y < screen_height_pixels; y++) {
        for (uint32_t x = 0; x < screen_width_pixels; x++) {
            put_pixel(x, y, current_bg_color);
        }
    }
    
    current_x = 0;
    current_y = screen_height_chars - 1;
}

// Очищает только область консоли (для символов)
void clear_screen(void) {
    uint32_t console_width = screen_width_chars * (CHAR_WIDTH + CHAR_PADDING_X);
    uint32_t console_height = screen_height_chars * (CHAR_HEIGHT + CHAR_PADDING_Y);
    
    // Ограничиваем размер консоли размером экрана
    if (console_width > screen_width_pixels) console_width = screen_width_pixels;
    if (console_height > screen_height_pixels) console_height = screen_height_pixels;
    
    for (uint32_t y = 0; y < console_height; y++) {
        for (uint32_t x = 0; x < console_width; x++) {
            put_pixel(x, y, current_bg_color);
        }
    }
    current_x = 0;
    current_y = 0;
}

// Очищает весь экран полностью
void clear_entire_screen(void) {
    for (uint32_t y = 0; y < screen_height_pixels; y++) {
        for (uint32_t x = 0; x < screen_width_pixels; x++) {
            framebuffer[y * pixels_per_scan_line + x] = current_bg_color;
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

void itoa(int64_t value, char* buffer, int base) {
    if (value < 0 && base == 10) {
        *buffer++ = '-';
        value = -value;
    }
    utoa((uint64_t)value, buffer, base);
}

void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    char buffer[32];
    
    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == 's') {
                char* str = va_arg(args, char*);
                print_string(str);
            } else if (*format == 'd') {
                int64_t val = va_arg(args, int64_t);
                itoa(val, buffer, 10);
                print_string(buffer);
            } else if (*format == 'u') {
                uint64_t val = va_arg(args, uint64_t);
                utoa(val, buffer, 10);
                print_string(buffer);
            } else if (*format == 'x' || *format == 'p' || *format == 'l') {
                uint64_t val = va_arg(args, uint64_t);
                utoa(val, buffer, 16);
                if (*format == 'p' || *format == 'l') {
                    print_string("0x");
                }
                print_string(buffer);
            } else if (*format == '%') {
                put_char('%');
            } else if (*format == 'c') {
                char c = (char)va_arg(args, int);
                put_char(c);
            }
        } else {
            put_char(*format);
        }
        format++;
    }
    va_end(args);
}

// Функция для отображения системной информации
void display_system_info(BootInfo* bi) {
    // Заголовок
    current_color = convert_color(0x00AAFF); // Голубой
    printf("\n");
    printf("================================================\n");
    printf("             LufiraOS Kernel v1.0              \n");
    printf("================================================\n\n");
    
    // Системная информация
    current_color = convert_color(0x55FF55); // Зеленый
    printf("SYSTEM INFORMATION:\n");
    printf("-------------------\n");
    
    current_color = convert_color(0xFFFFFF); // Белый
    printf("  Architecture:     x86_64\n");
    printf("  Build Date:       %s\n", __DATE__);
    printf("  Build Time:       %s\n", __TIME__);
    
    // Информация о памяти
    current_color = convert_color(0x55FF55);
    printf("\nMEMORY INFORMATION:\n");
    printf("-------------------\n");
    
    current_color = convert_color(0xFFFFFF);
    printf("  Total Memory:     %u MB\n", (uint32_t)(bi->TotalMemory / (1024 * 1024)));
    printf("  Framebuffer:      0x%lx\n", bi->FrameBufferBase);
    printf("  FB Size:          %u KB\n", (uint32_t)(bi->FrameBufferSize / 1024));
    
    // Информация о дисплее
    current_color = convert_color(0x55FF55);
    printf("\nDISPLAY INFORMATION:\n");
    printf("--------------------\n");
    
    current_color = convert_color(0xFFFFFF);
    printf("  Resolution:       %d x %d\n", bi->HorizontalResolution, bi->VerticalResolution);
    printf("  Pixel Format:     %s\n", (bi->PixelFormat == 0) ? "RGB" : "BGR");
    printf("  Pixels/Line:      %d\n", bi->PixelsPerScanLine);
    printf("  Console Grid:     %d x %d chars\n", screen_width_chars, screen_height_chars);
    
    // Информация о загрузчике
    current_color = convert_color(0x55FF55);
    printf("\nBOOT INFORMATION:\n");
    printf("-----------------\n");
    
    current_color = convert_color(0xFFFFFF);
    printf("  Memory Map Size:  %u bytes\n", (uint32_t)bi->MemoryMapSize);
    printf("  Descriptor Size:  %u bytes\n", bi->MemoryMapDescriptorSize);
    
    // Состояние системы
    current_color = convert_color(0x55FF55);
    printf("\nSYSTEM STATUS:\n");
    printf("--------------\n");
    
    current_color = convert_color(0xFFFFFF);
    printf("  Console:          READY\n");
    printf("  Keyboard:         INITIALIZING...\n");
    printf("  Memory Manager:   NOT INITIALIZED\n");
    printf("  Interrupts:       DISABLED\n");
    printf("  Task Manager:     NOT INITIALIZED\n");
}