#include "graphics2d.h"
#include "console.h"
#include "lib/string.h"

static inline int32_t min_i32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t max_i32(int32_t a, int32_t b) { return a > b ? a : b; }

void gfx_fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    int32_t x0 = max_i32(x, 0);
    int32_t y0 = max_i32(y, 0);
    int32_t x1 = min_i32(x + (int32_t)w, (int32_t)screen_width_pixels);
    int32_t y1 = min_i32(y + (int32_t)h, (int32_t)screen_height_pixels);

    for (int32_t py = y0; py < y1; py++) {
        // Пишем строку целиком напрямую в framebuffer (не через put_pixel
        // по одному пикселю) — та же экономия, ради которой put_char_graphic()
        // в console.c тоже не вызывает put_pixel() лишний раз без нужды.
        uint32_t *row = framebuffer + (uint32_t)py * pixels_per_scan_line + (uint32_t)x0;
        for (int32_t px = x0; px < x1; px++) *row++ = color;
    }
    // В обход put_pixel() — он и выставляет "грязный" флаг для двойной
    // буферизации (console.c), сами ставим здесь. Точный прямоугольник
    // (не весь экран) — см. подробный комментарий у console_mark_dirty_rect().
    if (y1 > y0 && x1 > x0) console_mark_dirty_rect(x0, y0, x1 - x0, y1 - y0);
}

void gfx_draw_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (w == 0 || h == 0) return;
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + (int32_t)h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + (int32_t)w - 1, y, 1, h, color);
}

void gfx_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    int32_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t dy = (y1 > y0) ? (y0 - y1) : (y1 - y0); // отрицательный |dy|
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;

    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < (int32_t)screen_width_pixels && y0 < (int32_t)screen_height_pixels)
            put_pixel((uint32_t)x0, (uint32_t)y0, color);

        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_blit(int32_t dst_x, int32_t dst_y, const uint32_t *src,
               uint32_t src_w, uint32_t src_h, uint32_t src_stride) {
    int32_t x0 = max_i32(dst_x, 0);
    int32_t y0 = max_i32(dst_y, 0);
    int32_t x1 = min_i32(dst_x + (int32_t)src_w, (int32_t)screen_width_pixels);
    int32_t y1 = min_i32(dst_y + (int32_t)src_h, (int32_t)screen_height_pixels);
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t copy_w = (uint32_t)(x1 - x0);
    // Сколько строк/столбцов src обрезал клиппинг слева/сверху — ровно
    // настолько же сдвигаем откуда читаем в src.
    uint32_t src_skip_x = (uint32_t)(x0 - dst_x);
    uint32_t src_skip_y = (uint32_t)(y0 - dst_y);

    for (int32_t py = y0; py < y1; py++) {
        uint32_t src_row_index = (uint32_t)(py - y0) + src_skip_y;
        const uint32_t *src_row = src + src_row_index * src_stride + src_skip_x;
        uint32_t *dst_row = framebuffer + (uint32_t)py * pixels_per_scan_line + (uint32_t)x0;
        memcpy(dst_row, src_row, copy_w * sizeof(uint32_t));
    }
    // Точный прямоугольник, не весь экран — см. подробный комментарий у
    // console_mark_dirty_rect() (console.h/.c): именно этот вызов (через
    // SYS_FB_PRESENT) WM гонит на КАЖДОЕ движение мыши, раньше это
    // раздувало "грязную" область до целого кадра на каждый тик.
    console_mark_dirty_rect(x0, y0, (int)copy_w, (int)(y1 - y0));
}
