#include "lib/types.h"
#include "lib/stdarg.h"
#include "lib/stddef.h"
#include "console.h"
#include "lib/colors.h"

// Глобальные переменные состояния консоли
uint32_t current_x = 0;
uint32_t current_y = 0;
uint32_t screen_width_chars = 0;
uint32_t screen_height_chars = 0;
uint32_t* framebuffer = NULL;
uint32_t current_color = 0xFFFFFF;
uint32_t current_bg_color = 0x000000;
uint32_t pixels_per_scan_line = 0;
uint32_t screen_width_pixels = 0;
uint32_t screen_height_pixels = 0;
uint32_t pixel_format = 0;

// Переменные для мигающего курсора
int cursor_visible = 1;
int cursor_enabled = 1;
uint32_t cursor_blink_counter = 0;
uint32_t cursor_blink_rate = 10;

// Текущая цветовая пара
ColorPair current_colors = {
    .fg_color = 0xFFFFFF,
    .bg_color = 0x000000,
    .fg_index = COLOR_WHITE,
    .bg_index = COLOR_BLACK
};

// Размеры символа 8x8
#define CHAR_WIDTH 8
#define CHAR_HEIGHT 8
#define CHAR_PADDING_X 1
#define CHAR_PADDING_Y 4

// 16-цветная палитра VGA (в формате 0xRRGGBB)
const uint32_t color_palette_16[] = {
    0x000000, // BLACK
    0x0000AA, // BLUE
    0x00AA00, // GREEN
    0x00AAAA, // CYAN
    0xAA0000, // RED
    0xAA00AA, // MAGENTA
    0xAA5500, // BROWN
    0xAAAAAA, // LIGHT_GRAY
    0x555555, // DARK_GRAY
    0x5555FF, // LIGHT_BLUE
    0x55FF55, // LIGHT_GREEN
    0x55FFFF, // LIGHT_CYAN
    0xFF5555, // LIGHT_RED
    0xFF55FF, // LIGHT_MAGENTA
    0xFFFF55, // YELLOW
    0xFFFFFF  // WHITE
};

// Имена цветов
const char* color_names_16[] = {
    "BLACK", "BLUE", "GREEN", "CYAN", "RED", "MAGENTA", "BROWN", "LIGHT_GRAY",
    "DARK_GRAY", "LIGHT_BLUE", "LIGHT_GREEN", "LIGHT_CYAN", "LIGHT_RED", 
    "LIGHT_MAGENTA", "YELLOW", "WHITE"
};

// 256-цветная палитра (инициализируется динамически)
uint32_t color_palette_256[256] = {0};

// --- Самодельный улучшенный шрифт 8x8 пикселей ---
static unsigned char full_font_data[][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* (space) */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18}, /* ! */
    {0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, /* " */
    {0x24, 0x7E, 0x24, 0x24, 0x7E, 0x24, 0x24, 0x00}, /* # */
    {0x18, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x18, 0x00}, /* $ */
    {0x62, 0x66, 0x0C, 0x18, 0x30, 0x66, 0x46, 0x00}, /* % */
    {0x3C, 0x66, 0x3C, 0x66, 0x66, 0x66, 0x3E, 0x00}, /* & */
    {0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ' */
    {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00}, /* ( */
    {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00}, /* ) */
    {0x18, 0x5A, 0x3C, 0xFF, 0x3C, 0x5A, 0x18, 0x00}, /* * */
    {0x18, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x00}, /* + */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, /* , */
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, /* . */
    {0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00}, /* / */
    {0xFF, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xFF, 0x00}, /* 0 */
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* 1 */
    {0x7E, 0x03, 0x03, 0x7E, 0xC0, 0xC0, 0xFF, 0x00}, /* 2 */
    {0x7E, 0x03, 0x03, 0x3E, 0x03, 0x03, 0x7E, 0x00}, /* 3 */
    {0xC3, 0xC3, 0xC3, 0xFF, 0x03, 0x03, 0x03, 0x00}, /* 4 */
    {0xFF, 0xC0, 0xC0, 0xFE, 0x03, 0x03, 0xFE, 0x00}, /* 5 */
    {0x7E, 0xC0, 0xC0, 0xFE, 0xC3, 0xC3, 0x7E, 0x00}, /* 6 */
    {0xFF, 0x03, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x00}, /* 7 */
    {0x7E, 0xC3, 0xC3, 0x7E, 0xC3, 0xC3, 0x7E, 0x00}, /* 8 */
    {0x7E, 0xC3, 0xC3, 0x7F, 0x03, 0x03, 0x7E, 0x00}, /* 9 */
    {0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00}, /* : */
    {0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00}, /* ; */
    {0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00}, /* < */
    {0x00, 0x7E, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00}, /* = */
    {0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x00}, /* > */
    {0x7E, 0xC3, 0x06, 0x1C, 0x18, 0x00, 0x18, 0x00}, /* ? */
    {0x7E, 0x81, 0xBD, 0xA5, 0xBD, 0x81, 0x7E, 0x00}, /* @ */
    {0x7E, 0xC3, 0xC3, 0xFF, 0xC3, 0xC3, 0xC3, 0x00}, /* A */
    {0xFE, 0xC3, 0xC3, 0xFE, 0xC3, 0xC3, 0xFE, 0x00}, /* B */
    {0x7F, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0x7F, 0x00}, /* C */
    {0xFC, 0xC6, 0xC3, 0xC3, 0xC3, 0xC6, 0xFC, 0x00}, /* D */
    {0xFF, 0xC0, 0xC0, 0xFC, 0xC0, 0xC0, 0xFF, 0x00}, /* E */
    {0xFF, 0xC0, 0xC0, 0xFC, 0xC0, 0xC0, 0xC0, 0x00}, /* F */
    {0x7F, 0xC0, 0xC0, 0xCF, 0xC3, 0xC3, 0x7F, 0x00}, /* G */
    {0xC3, 0xC3, 0xC3, 0xFF, 0xC3, 0xC3, 0xC3, 0x00}, /* H */
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* I */
    {0x1F, 0x0C, 0x0C, 0x0C, 0x0C, 0xCC, 0x78, 0x00}, /* J */
    {0xC3, 0xC6, 0xCC, 0xF8, 0xCC, 0xC6, 0xC3, 0x00}, /* K */
    {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFF, 0x00}, /* L */
    {0xC3, 0xE7, 0xFF, 0xDB, 0xC3, 0xC3, 0xC3, 0x00}, /* M */
    {0xC3, 0xE3, 0xF3, 0xDB, 0xCF, 0xC7, 0xC3, 0x00}, /* N */
    {0x7E, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0x7E, 0x00}, /* O */
    {0xFE, 0xC3, 0xC3, 0xFE, 0xC0, 0xC0, 0xC0, 0x00}, /* P */
    {0x7E, 0xC3, 0xC3, 0xC3, 0xC3, 0x66, 0x3C, 0x1B}, /* Q */
    {0xFE, 0xC3, 0xC3, 0xFE, 0xD8, 0xCC, 0xC6, 0x00}, /* R */
    {0x7E, 0xC0, 0xC0, 0x7E, 0x03, 0x03, 0x7E, 0x00}, /* S */
    {0xFF, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* T */
    {0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0x7E, 0x00}, /* U */
    {0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0x66, 0x3C, 0x00}, /* V */
    {0xC3, 0xC3, 0xC3, 0xDB, 0xFF, 0xE7, 0xC3, 0x00}, /* W */
    {0xC3, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0xC3, 0x00}, /* X */
    {0xC3, 0xC3, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00}, /* Y */
    {0xFF, 0x03, 0x06, 0x1C, 0x30, 0x60, 0xFF, 0x00}, /* Z */
    {0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00}, /* [ */
    {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01}, /* \ */
    {0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00}, /* ] */
    {0x18, 0x3C, 0x66, 0xC3, 0x00, 0x00, 0x00, 0x00}, /* ^ */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, /* _ */
    {0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ` */
    {0x00, 0x7E, 0x03, 0x7F, 0xC3, 0xC3, 0x7F, 0x00}, /* a */
    {0xC0, 0xC0, 0xFE, 0xC3, 0xC3, 0xC3, 0xFE, 0x00}, /* b */
    {0x00, 0x7E, 0xC0, 0xC0, 0xC0, 0xC0, 0x7E, 0x00}, /* c */
    {0x03, 0x03, 0x7F, 0xC3, 0xC3, 0xC3, 0x7F, 0x00}, /* d */
    {0x00, 0x7E, 0xC3, 0xFF, 0xC0, 0xC0, 0x7E, 0x00}, /* e */
    {0x1C, 0x30, 0xFC, 0x30, 0x30, 0x30, 0x30, 0x00}, /* f */
    {0x00, 0x7F, 0xC3, 0xC3, 0x7F, 0x03, 0x03, 0x7E}, /* g */
    {0xC0, 0xC0, 0xFE, 0xC3, 0xC3, 0xC3, 0xC3, 0x00}, /* h */
    {0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* i */
    {0x06, 0x00, 0x06, 0x06, 0x06, 0x06, 0x66, 0x3C}, /* j */
    {0xC0, 0xC0, 0xC6, 0xCC, 0xF8, 0xCC, 0xC6, 0x00}, /* k */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* l */
    {0x00, 0xFE, 0xDB, 0xDB, 0xDB, 0xC3, 0xC3, 0x00}, /* m */
    {0x00, 0xFE, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0x00}, /* n */
    {0x00, 0x7E, 0xC3, 0xC3, 0xC3, 0xC3, 0x7E, 0x00}, /* o */
    {0x00, 0xFE, 0xC3, 0xC3, 0xFE, 0xC0, 0xC0, 0xC0}, /* p */
    {0x00, 0x7F, 0xC3, 0xC3, 0x7F, 0x03, 0x03, 0x03}, /* q */
    {0x00, 0xFE, 0xC3, 0xC0, 0xC0, 0xC0, 0xC0, 0x00}, /* r */
    {0x00, 0x7E, 0xC0, 0x7E, 0x03, 0x03, 0x7E, 0x00}, /* s */
    {0x18, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x00}, /* t */
    {0x00, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0x7F, 0x00}, /* u */
    {0x00, 0xC3, 0xC3, 0xC3, 0xC3, 0x66, 0x3C, 0x00}, /* v */
    {0x00, 0xC3, 0xDB, 0xDB, 0xDB, 0xFF, 0x66, 0x00}, /* w */
    {0x00, 0xC3, 0x66, 0x3C, 0x3C, 0x66, 0xC3, 0x00}, /* x */
    {0x00, 0xC3, 0xC3, 0xC3, 0x7F, 0x03, 0x03, 0x7E}, /* y */
    {0x00, 0xFF, 0x06, 0x1C, 0x30, 0x60, 0xFF, 0x00}, /* z */
    {0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00}, /* { */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}, /* | */
    {0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0x00}, /* } */
    {0x00, 0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ~ */
};

// ==================== CONSOLE SCROLLBACK ====================

#define CONSOLE_HISTORY_LINES 1000
#define CONSOLE_MAX_COLUMNS   256

static char console_history[CONSOLE_HISTORY_LINES][CONSOLE_MAX_COLUMNS];

static uint32_t console_history_count = 1;
static uint32_t console_history_start = 0;
static uint32_t console_history_scroll = 0;

static int console_rendering_history = 0;
static uint32_t history_current_column = 0;


// Прототипы
static void history_clear_line(uint32_t index);
static void history_new_line(void);
static void history_set_visible_char(uint32_t x, uint32_t y, char c);
static void history_render(void);


static int interrupts_enabled(void) {
    uint64_t rflags;
    asm volatile ("pushfq; pop %0" : "=r"(rflags));
    return (int)((rflags >> 9) & 1ULL);
}

// Преобразование цвета из RGB в BGR если нужно
uint32_t convert_color(uint32_t color) {
    // if (pixel_format == 0) {
    //     return color; // RGB формат
    // } else {
    //     // BGR формат: преобразуем RGB в BGR
    //     uint8_t r = (color >> 16) & 0xFF;
    //     uint8_t g = (color >> 8) & 0xFF;
    //     uint8_t b = color & 0xFF;
    //     return (b << 16) | (g << 8) | r;
    // }
    return color;
}

// Инициализация 256-цветной палитры
void init_256_color_palette(void) {
    // 0-15: 16 стандартных цветов VGA
    for (int i = 0; i < 16; i++) {
        color_palette_256[i] = color_palette_16[i];
    }
    
    // 16-231: 6x6x6 RGB куб
    int index = 16;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                uint8_t red = r * 51;   // 0, 51, 102, 153, 204, 255
                uint8_t green = g * 51;
                uint8_t blue = b * 51;
                uint32_t rgb = (red << 16) | (green << 8) | blue;
                color_palette_256[index++] = rgb;
            }
        }
    }
    
    // 232-255: Оттенки серого
    for (int i = 0; i < 24; i++) {
        uint8_t gray = 8 + i * 10;
        if (gray > 238) gray = 238;
        uint32_t rgb = (gray << 16) | (gray << 8) | gray;
        color_palette_256[232 + i] = rgb;
    }
}

// Получение цвета из палитры с преобразованием
uint32_t get_color_from_palette(int index) {
    if (index < 0 || index >= 256) {
        return convert_color(0xFFFFFF); // Белый по умолчанию
    }
    return convert_color(color_palette_256[index]);
}

void initialize_console(BootInfo* bi) {
    framebuffer = (uint32_t*)bi->FrameBufferBase;
    pixels_per_scan_line = bi->PixelsPerScanLine;
    screen_width_pixels = bi->HorizontalResolution;
    screen_height_pixels = bi->VerticalResolution;
    pixel_format = bi->PixelFormat;
    
    // Инициализируем 256-цветную палитру
    init_256_color_palette();
    
    // Преобразуем стандартные цвета
    current_colors.fg_color = convert_color(0xFFFFFF);
    current_colors.bg_color = convert_color(0x000000);
    current_color = current_colors.fg_color;
    current_bg_color = current_colors.bg_color;
    
    // Вычисляем количество символов, которые поместятся на экране
    screen_width_chars = screen_width_pixels / (CHAR_WIDTH + CHAR_PADDING_X);
    screen_height_chars = screen_height_pixels / (CHAR_HEIGHT + CHAR_PADDING_Y);

    // Инициализация scrollback
    console_history_count = 1;
    console_history_start = 0;
    console_history_scroll = 0;
    history_current_column = 0;

    for (uint32_t i = 0; i < CONSOLE_HISTORY_LINES; i++) {
        history_clear_line(i);
    }
    
    // Гарантируем хотя бы минимальный размер
    if (screen_width_chars < 10) screen_width_chars = 10;
    if (screen_height_chars < 10) screen_height_chars = 10;
    
    current_x = 0;
    current_y = 0;
    
    // Инициализация курсора
    cursor_visible = 1;
    cursor_enabled = 1;
    cursor_blink_counter = 0;
    
    // Очищаем весь экран при инициализации
    clear_entire_screen();
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= screen_width_pixels || y >= screen_height_pixels) return;
    framebuffer[y * pixels_per_scan_line + x] = color;
}

// Получение имени цвета
const char* get_color_name(ConsoleColor color) {
    if (color >= 0 && color <= 15) {
        return color_names_16[color];
    }
    return "RGB";
}

// Отрисовка символа 8x8
void put_char_graphic(int c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if (!console_rendering_history) {
        if (!console_is_scrolled()) {
            history_set_visible_char(x, y, (char)c);
        }
    }
    if (c < 32 || c > 127) c = '?';
    
    unsigned char* glyph = full_font_data[c - 32];
    
    // Переводим позиции символа в пиксели
    uint32_t base_x = x * (CHAR_WIDTH + CHAR_PADDING_X);
    uint32_t base_y = y * (CHAR_HEIGHT + CHAR_PADDING_Y);
    
    // Ограничиваем область отрисовки размерами экрана
    if (base_x >= screen_width_pixels || base_y >= screen_height_pixels) return;
    
    for (uint32_t cy = 0; cy < CHAR_HEIGHT + CHAR_PADDING_Y; cy++) {
        uint32_t target_y = base_y + cy;
        if (target_y >= screen_height_pixels) break;
        
        for (uint32_t cx = 0; cx < CHAR_WIDTH + CHAR_PADDING_X; cx++) {
            uint32_t target_x = base_x + cx;
            if (target_x >= screen_width_pixels) break;
            
            if (cx < CHAR_WIDTH && cy < CHAR_HEIGHT) {
                // Внутри символа: проверяем бит шрифта
                if ((glyph[cy] >> (7 - cx)) & 1) {
                    put_pixel(target_x, target_y, fg_color);
                } else {
                    put_pixel(target_x, target_y, bg_color);
                }
            } else {
                // Отступы заливаем цветом фона
                put_pixel(target_x, target_y, bg_color);
            }
        }
    }
}

void put_char(char c) {
    // Если пользователь смотрит старый вывод,
    // любой обычный вывод возвращает нас вниз.
    if (console_is_scrolled()) {
        console_scroll_to_bottom();
    }

    if (cursor_enabled && cursor_visible) {
        erase_cursor();
    }

    if (c == '\n') {

        current_x = 0;
        current_y++;

        history_new_line();

    } else if (c == '\r') {

        current_x = 0;
        history_current_column = 0;

    } else if (c == '\b') {

        if (current_x > 0) {
            current_x--;

            put_char_graphic(
                ' ',
                current_x,
                current_y,
                current_color,
                current_bg_color
            );

            if (history_current_column > 0)
                history_current_column--;
        }

    } else if (c == '\t') {

        for (int i = 0; i < 4; i++) {

            put_char_graphic(
                ' ',
                current_x,
                current_y,
                current_color,
                current_bg_color
            );

            current_x++;
            history_current_column++;

            if (current_x >= screen_width_chars) {

                current_x = 0;
                current_y++;

                history_new_line();
            }
        }

    } else {

        put_char_graphic(
            c,
            current_x,
            current_y,
            current_color,
            current_bg_color
        );

        current_x++;
        history_current_column++;

        if (current_x >= screen_width_chars) {

            current_x = 0;
            current_y++;

            history_new_line();
        }
    }

    if (current_y >= screen_height_chars) {
        scroll_screen();
    }

    if (cursor_enabled && cursor_visible) {
        draw_cursor();
    }
}

void scroll_screen(void) {
    // При обычном переполнении всегда остаёмся
    // внизу scrollback.
    console_history_scroll = 0;

    current_x = 0;

    if (screen_height_chars > 0)
        current_y = screen_height_chars - 1;
    else
        current_y = 0;

    history_render();

    cursor_visible = 0;

    if (cursor_enabled)
        draw_cursor();
}

// Очищает только область консоли (для символов)
void clear_screen(void) {
    // Стираем курсор если он видим
    if (cursor_enabled && cursor_visible) {
        erase_cursor();
    }
    
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
    
    // Восстанавливаем курсор
    if (cursor_enabled && cursor_visible) {
        draw_cursor();
    }
}

// Очищает весь экран полностью
void clear_entire_screen(void) {
    // Стираем курсор если он видим
    if (cursor_enabled && cursor_visible) {
        erase_cursor();
    }
    
    for (uint32_t y = 0; y < screen_height_pixels; y++) {
        for (uint32_t x = 0; x < screen_width_pixels; x++) {
            framebuffer[y * pixels_per_scan_line + x] = current_bg_color;
        }
    }
    current_x = 0;
    current_y = 0;
    
    // Восстанавливаем курсор
    if (cursor_enabled && cursor_visible) {
        draw_cursor();
    }
}

void print_string(const char* str) {
    while (*str) {
        put_char(*str++);
    }
}

void utoa(uint64_t value, char* buffer, int base) {
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
            
            // Обработка %%
            if (*format == '%') {
                put_char('%');
            }
            // Строка
            else if (*format == 's') {
                char* str = va_arg(args, char*);
                print_string(str);
            }
            // Символ
            else if (*format == 'c') {
                char c = (char)va_arg(args, int);
                put_char(c);
            }
            // Десятичное знаковое
            else if (*format == 'd') {
                int64_t val = va_arg(args, int64_t);
                itoa(val, buffer, 10);
                print_string(buffer);
            }
            // Десятичное беззнаковое
            else if (*format == 'u') {
                uint64_t val = va_arg(args, uint64_t);
                utoa(val, buffer, 10);
                print_string(buffer);
            }
            // Указатель (всегда с 0x)
            else if (*format == 'p') {
                uint64_t val = va_arg(args, uint64_t);
                print_string("0x");
                utoa(val, buffer, 16);
                print_string(buffer);
            }
            // Шестнадцатеричное или long/long long
            else if (*format == 'x' || *format == 'l') {
                // Если просто %x — печатаем без 0x
                if (*format == 'x') {
                    uint64_t val = va_arg(args, uint64_t);
                    utoa(val, buffer, 16);
                    print_string(buffer);
                }
                // Если %l... — пропускаем все 'l' и смотрим что дальше
                else {
                    format++; // пропускаем первый 'l'
                    if (*format == 'l') {
                        format++; // пропускаем второй 'l' (%llx)
                    }
                    
                    if (*format == 'x') {
                        uint64_t val = va_arg(args, uint64_t);
                        utoa(val, buffer, 16);
                        print_string(buffer);
                    }
                    else if (*format == 'u') {
                        uint64_t val = va_arg(args, uint64_t);
                        utoa(val, buffer, 10);
                        print_string(buffer);
                    }
                    else if (*format == 'd') {
                        int64_t val = va_arg(args, int64_t);
                        itoa(val, buffer, 10);
                        print_string(buffer);
                    }
                    else if (*format == 's') {
                        char* str = va_arg(args, char*);
                        print_string(str);
                    }
                    else {
                        // Неизвестный формат — пропускаем
                        format--;
                    }
                }
            }
            // Неизвестный спецификатор — игнорируем
            else {
                // Ничего не делаем, просто пропускаем
            }
        } else {
            put_char(*format);
        }
        format++;
    }
    va_end(args);
}

// ==================== ФУНКЦИИ ДЛЯ РАБОТЫ С ЦВЕТАМИ ====================

// Установка цветов по индексам (0-15)
void set_color_by_index(ConsoleColor fg, ConsoleColor bg) {
    if (fg >= 0 && fg <= 255) {
        current_colors.fg_index = fg;
        current_colors.fg_color = get_color_from_palette(fg);
        current_color = current_colors.fg_color;
    }
    if (bg >= 0 && bg <= 255) {
        current_colors.bg_index = bg;
        current_colors.bg_color = get_color_from_palette(bg);
        current_bg_color = current_colors.bg_color;
    }
}

// Установка цветов по RGB значениям
void set_color_by_rgb(uint32_t fg_rgb, uint32_t bg_rgb) {
    current_colors.fg_color = convert_color(fg_rgb);
    current_colors.bg_color = convert_color(bg_rgb);
    current_colors.fg_index = COLOR_RGB;
    current_colors.bg_index = COLOR_RGB;
    current_color = current_colors.fg_color;
    current_bg_color = current_colors.bg_color;
}

// Установка цвета текста по индексу
void set_foreground_color(ConsoleColor color) {
    if (color >= 0 && color <= 255) {
        current_colors.fg_index = color;
        current_colors.fg_color = get_color_from_palette(color);
        current_color = current_colors.fg_color;
    }
}

// Установка цвета фона по индексу
void set_background_color(ConsoleColor color) {
    if (color >= 0 && color <= 255) {
        current_colors.bg_index = color;
        current_colors.bg_color = get_color_from_palette(color);
        current_bg_color = current_colors.bg_color;
    }
}

// Установка цвета текста по RGB
void set_foreground_rgb(uint32_t rgb) {
    current_colors.fg_color = convert_color(rgb);
    current_colors.fg_index = COLOR_RGB;
    current_color = current_colors.fg_color;
}

// Установка цвета фона по RGB
void set_background_rgb(uint32_t rgb) {
    current_colors.bg_color = convert_color(rgb);
    current_colors.bg_index = COLOR_RGB;
    current_bg_color = current_colors.bg_color;
}

// Сброс цветов к значениям по умолчанию
void reset_colors(void) {
    set_color_by_index(COLOR_WHITE, COLOR_BLACK);
}

// Преобразование RGB в ближайший цвет из 16-цветной палитры
ConsoleColor find_closest_color(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    
    // Простая эвристика для определения цвета
    if (r == g && g == b) {
        // Оттенки серого
        if (r < 64) return COLOR_BLACK;
        if (r < 128) return COLOR_DARK_GRAY;
        if (r < 192) return COLOR_LIGHT_GRAY;
        return COLOR_WHITE;
    }
    
    // Определяем доминирующий цвет
    if (r > g && r > b) {
        if (r > 200) return COLOR_LIGHT_RED;
        return COLOR_RED;
    } else if (g > r && g > b) {
        if (g > 200) return COLOR_LIGHT_GREEN;
        return COLOR_GREEN;
    } else if (b > r && b > g) {
        if (b > 200) return COLOR_LIGHT_BLUE;
        return COLOR_BLUE;
    } else if (r == g && r > b) {
        if (r > 200) return COLOR_YELLOW;
        return COLOR_BROWN;
    } else if (r == b && r > g) {
        return COLOR_MAGENTA;
    } else if (g == b && g > r) {
        return COLOR_CYAN;
    }
    
    return COLOR_WHITE;
}

// Вывод таблицы 16-цветной палитры
void print_color_table_16(void) {
    printf("\n16-Color Palette:\n");
    printf("----------------\n");
    
    // Сохраняем текущие цвета
    ColorPair saved_colors = current_colors;
    uint32_t saved_color = current_color;
    uint32_t saved_bg_color = current_bg_color;
    
    for (int i = 0; i < 16; i++) {
        // Выводим индекс и название белым цветом
        set_foreground_color(COLOR_WHITE);
        
        // Выводим индекс
        if (i < 10) {
            printf("  %d: ", i);
        } else {
            printf(" %d: ", i);
        }
        
        // Выводим имя цвета
        printf("%s", color_names_16[i]);
        
        // Дополняем пробелами для выравнивания
        int name_len = 0;
        const char* p = color_names_16[i];
        while (*p++) name_len++;
        
        for (int j = name_len; j < 13; j++) {
            put_char(' ');
        }
        
        // Устанавливаем цвет текста для решеток
        if (i == COLOR_BLACK) {
            // Для черного цвета используем темно-серый, чтобы было видно на черном фоне
            set_foreground_color(COLOR_DARK_GRAY);
        } else {
            set_foreground_color((ConsoleColor)i);
        }
        
        // Выводим четыре решетки
        printf(" [####]\n");
    }
    
    // Восстанавливаем цвета
    current_colors = saved_colors;
    current_color = saved_color;
    current_bg_color = saved_bg_color;
}

// Функция для отображения системной информации
void display_system_info(BootInfo* bi) {
    // Сохраняем текущие цвета
    ColorPair saved_colors = current_colors;
    uint32_t saved_color = current_color;
    uint32_t saved_bg_color = current_bg_color;
    
    // Заголовок
    current_color = convert_color(0x00AAFF); // Голубой
    printf("\n");
    printf("================================================\n");
    printf("             LufiraOS Kernel v0.2              \n");
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
    printf("  Color Support:    256 colors\n");
    
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
    printf("  Console:          READY (256 colors)\n");
    printf("  Keyboard:         READY\n");
    printf("  Memory Manager:   NOT INITIALIZED\n");
    printf("  Interrupts:       %s\n", interrupts_enabled() ? "ENABLED" : "DISABLED");
    printf("  Task Manager:     NOT INITIALIZED\n");
    
    // Восстанавливаем цвета
    current_colors = saved_colors;
    current_color = saved_color;
    current_bg_color = saved_bg_color;
}

// ==================== ФУНКЦИИ КУРСОРА ====================

// Отрисовка курсора (подчеркивание в текущей позиции)
void draw_cursor(void) {
    if (!cursor_enabled) return;
    
    // Сохраняем текущий цвет
    uint32_t saved_color = current_color;
    
    // Устанавливаем цвет курсора (белый)
    current_color = convert_color(0xFFFFFF);
    
    // Вычисляем позицию курсора (нижняя часть текущего символа)
    uint32_t base_x = current_x * (CHAR_WIDTH + CHAR_PADDING_X);
    uint32_t base_y = current_y * (CHAR_HEIGHT + CHAR_PADDING_Y) + CHAR_HEIGHT;
    
    // Отрисовываем горизонтальную линию (подчеркивание)
    for (uint32_t x = 0; x < CHAR_WIDTH; x++) {
        uint32_t target_x = base_x + x;
        uint32_t target_y = base_y;
        
        if (target_x < screen_width_pixels && target_y < screen_height_pixels) {
            put_pixel(target_x, target_y, current_color);
        }
    }
    
    // Восстанавливаем цвет
    current_color = saved_color;
    
    cursor_visible = 1;
}

// Стирание курсора
void erase_cursor(void) {
    if (!cursor_enabled) return;
    
    // Вычисляем позицию курсора
    uint32_t base_x = current_x * (CHAR_WIDTH + CHAR_PADDING_X);
    uint32_t base_y = current_y * (CHAR_HEIGHT + CHAR_PADDING_Y) + CHAR_HEIGHT;
    
    // Стираем горизонтальную линию (заливаем цветом фона)
    for (uint32_t x = 0; x < CHAR_WIDTH; x++) {
        uint32_t target_x = base_x + x;
        uint32_t target_y = base_y;
        
        if (target_x < screen_width_pixels && target_y < screen_height_pixels) {
            put_pixel(target_x, target_y, current_bg_color);
        }
    }
    
    cursor_visible = 0;
}

// Обновление состояния курсора (мигание)
void update_cursor(void) {
    if (!cursor_enabled) return;
    
    cursor_blink_counter++;
    
    // Мигаем с заданной частотой
    if (cursor_blink_counter >= cursor_blink_rate) {
        cursor_blink_counter = 0;
        
        if (cursor_visible) {
            erase_cursor();
        } else {
            draw_cursor();
        }
    }
}

// Включение/выключение курсора
void enable_cursor(int enabled) {
    if (cursor_enabled && !enabled) {
        // Выключаем курсор - стираем его
        erase_cursor();
    }
    
    cursor_enabled = enabled;
    
    if (cursor_enabled && !cursor_visible) {
        // Включаем курсор - рисуем его
        draw_cursor();
    }
}

// Перемещение курсора влево
void move_cursor_left(void) {
    if (current_x > 0) {
        erase_cursor();
        current_x--;
        draw_cursor();
    }
}

// Перемещение курсора вправо
void move_cursor_right(void) {
    if (current_x < screen_width_chars - 1) {
        erase_cursor();
        current_x++;
        draw_cursor();
    }
}

// Установка позиции курсора
void set_cursor_position(uint32_t x, uint32_t y) {

    if (console_is_scrolled()) {
        console_scroll_to_bottom();
    }

    if (x >= screen_width_chars || y >= screen_height_chars)
        return;

    erase_cursor();

    current_x = x;
    current_y = y;

    draw_cursor();
}

static void history_clear_line(uint32_t index) {
    for (uint32_t i = 0; i < CONSOLE_MAX_COLUMNS; i++) {
        console_history[index][i] = ' ';
    }
}

static uint32_t history_get_index(uint32_t logical_line) {
    if (logical_line >= console_history_count)
        return 0;

    return (console_history_start + logical_line) % CONSOLE_HISTORY_LINES;
}

static void history_new_line(void) {
    uint32_t index;

    if (console_history_count < CONSOLE_HISTORY_LINES) {
        index = history_get_index(console_history_count);
        console_history_count++;
    } else {
        // Буфер заполнен — удаляем самую старую строку
        console_history_start =
            (console_history_start + 1) % CONSOLE_HISTORY_LINES;

        index = history_get_index(console_history_count - 1);
    }

    history_clear_line(index);
    history_current_column = 0;
}

static void history_set_visible_char(uint32_t x, uint32_t y, char c) {
    if (console_rendering_history)
        return;

    if (console_history_count == 0)
        return;

    if (screen_width_chars == 0)
        return;

    if (screen_width_chars > CONSOLE_MAX_COLUMNS)
        return;

    // Когда мы в обычном режиме, экран показывает последние строки.
    // Вычисляем логическую строку scrollback, соответствующую y.
    uint32_t visible_lines = screen_height_chars;

    uint32_t top_line = 0;

    if (console_history_count > visible_lines) {
        top_line = console_history_count - visible_lines;
    }

    if (console_history_scroll > 0) {
        if (top_line >= console_history_scroll)
            top_line -= console_history_scroll;
        else
            top_line = 0;
    }

    uint32_t logical_line = top_line + y;

    if (logical_line >= console_history_count)
        return;

    if (x >= CONSOLE_MAX_COLUMNS)
        return;

    uint32_t index = history_get_index(logical_line);
    console_history[index][x] = c;
}

static void history_render(void) {
    console_rendering_history = 1;

    // Полностью очищаем framebuffer
    for (uint32_t y = 0; y < screen_height_pixels; y++) {
        for (uint32_t x = 0; x < screen_width_pixels; x++) {
            framebuffer[y * pixels_per_scan_line + x] = current_bg_color;
        }
    }

    uint32_t max_scroll = 0;

    if (console_history_count > screen_height_chars) {
        max_scroll = console_history_count - screen_height_chars;
    }

    if (console_history_scroll > max_scroll)
        console_history_scroll = max_scroll;

    uint32_t top_line = 0;

    if (console_history_count > screen_height_chars) {
        top_line = console_history_count - screen_height_chars;
    }

    if (console_history_scroll > 0) {
        if (top_line >= console_history_scroll)
            top_line -= console_history_scroll;
        else
            top_line = 0;
    }

    for (uint32_t y = 0; y < screen_height_chars; y++) {
        uint32_t logical_line = top_line + y;

        if (logical_line >= console_history_count)
            break;

        uint32_t index = history_get_index(logical_line);

        for (uint32_t x = 0;
             x < screen_width_chars && x < CONSOLE_MAX_COLUMNS;
             x++) {

            char c = console_history[index][x];

            if (c == '\0')
                c = ' ';

            if (c != ' ') {
                put_char_graphic(
                    c,
                    x,
                    y,
                    current_color,
                    current_bg_color
                );
            }
        }
    }

    console_rendering_history = 0;
}

void console_scroll_up(void) {
    uint32_t max_scroll = 0;

    if (console_history_count > screen_height_chars) {
        max_scroll = console_history_count - screen_height_chars;
    }

    if (max_scroll == 0)
        return;

    if (console_history_scroll < max_scroll) {
        if (cursor_enabled && cursor_visible)
            erase_cursor();

        console_history_scroll++;

        cursor_visible = 0;
        history_render();
    }
}

void console_scroll_down(void) {
    if (console_history_scroll == 0)
        return;

    if (cursor_enabled && cursor_visible)
        erase_cursor();

    console_history_scroll--;

    history_render();

    if (console_history_scroll == 0) {
        cursor_visible = 0;
        draw_cursor();
    }
}

void console_scroll_to_bottom(void) {
    if (console_history_scroll == 0)
        return;

    if (cursor_enabled && cursor_visible)
        erase_cursor();

    console_history_scroll = 0;

    history_render();

    cursor_visible = 0;
    draw_cursor();
}

int console_is_scrolled(void) {
    return console_history_scroll != 0;
}