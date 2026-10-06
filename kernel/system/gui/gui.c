// gui.c — v0.8 (GUI + WM), первый срез. Архитектура и границы — см. gui.h.

#include "gui.h"
#include "drivers/console/console.h"
#include "drivers/console/graphics2d.h"
#include "drivers/input/input.h"
#include "drivers/keyboard/keyboard.h"
#include "system/mm/heap.h"
#include "lib/stddef.h"
#include "lib/string.h"

typedef struct {
    int head, tail, count;
    gui_event_t events[32];
} gui_event_queue_t;

typedef struct {
    int in_use;
    uint32_t owner_pid;
    int x, y;              // верхний левый угол ВСЕГО окна (рамка+титлбар)
    uint32_t w, h;          // размер КЛИЕНТСКОЙ области (без рамки/титлбара)
    char title[GUI_TITLE_MAX];
    uint32_t *pixels;       // kmalloc'd, w*h, уже convert_color()'нутые пиксели
    gui_event_queue_t events;
} gui_window_t;

static gui_window_t g_windows[GUI_MAX_WINDOWS];
static int g_order[GUI_MAX_WINDOWS]; // z-order: g_order[0]=задний..g_order[count-1]=передний (индексы в g_windows)
static int g_window_count = 0;
static int g_focused = -1; // индекс в g_windows, -1 = нет фокуса

static int g_gui_dirty = 1; // перерисовать при следующем gui_tick() (НЕ то же, что console_mark_dirty())
static int g_prev_buttons = 0;

// Состояние перетаскивания титлбара мышью.
static int g_dragging = -1;    // индекс в g_windows, -1 = не тащим
static int g_drag_off_x = 0, g_drag_off_y = 0; // смещение от угла окна до точки клика

// GUI_CHAR_WIDTH/GUI_CHAR_HEIGHT (8x8) не экспортированы из console.c — они
// совпадают с размером глифа, который сами же put_char_graphic_px()/
// font_draw_glyph_to_buffer() используют жёстко прошитым внутри (см.
// их же реализации в console.c), так что 8 здесь не произвольное число.
#define GUI_CHAR_WIDTH   8
#define GUI_CHAR_HEIGHT  8

#define DESKTOP_BG       0x2b2b3a
#define TITLEBAR_ACTIVE  0x3a6ea5
#define TITLEBAR_INACTIVE 0x4a4a55
#define BORDER_COLOR     0x1a1a22
#define TITLE_TEXT_COLOR 0xffffff
#define CLOSE_BTN_COLOR  0xcc4444

static void queue_push(gui_window_t *win, int type, int x, int y, int key_or_button) {
    gui_event_queue_t *q = &win->events;
    if (q->count >= 32) return; // переполнение - молча роняем, как и console_input_push()
    q->events[q->tail] = (gui_event_t){ .type = type, .x = x, .y = y, .key_or_button = key_or_button };
    q->tail = (q->tail + 1) % 32;
    q->count++;
}

void gui_init(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        g_windows[i].in_use = 0;
        g_order[i] = -1;
    }
    g_window_count = 0;
    g_focused = -1;
}

int gui_mode_active(void) {
    return g_window_count > 0;
}

// Полная ширина/высота окна НА ЭКРАНЕ (рамка+титлбар+клиентская область) —
// нужно и для отрисовки, и для hit-тестирования мыши.
static uint32_t outer_w(gui_window_t *w) { return w->w + 2 * GUI_BORDER; }
static uint32_t outer_h(gui_window_t *w) { return w->h + GUI_TITLEBAR_HEIGHT + GUI_BORDER; }

static int find_window_at(int px, int py, int *out_idx) {
    // Сверху вниz по z-order (последний в g_order - самый верхний).
    for (int i = g_window_count - 1; i >= 0; i--) {
        int idx = g_order[i];
        gui_window_t *w = &g_windows[idx];
        if (!w->in_use) continue;
        if (px >= w->x && px < w->x + (int)outer_w(w) &&
            py >= w->y && py < w->y + (int)outer_h(w)) {
            *out_idx = idx;
            return 1;
        }
    }
    return 0;
}

static void raise_to_top(int idx) {
    int pos = -1;
    for (int i = 0; i < g_window_count; i++) if (g_order[i] == idx) { pos = i; break; }
    if (pos < 0 || pos == g_window_count - 1) return;
    for (int i = pos; i < g_window_count - 1; i++) g_order[i] = g_order[i + 1];
    g_order[g_window_count - 1] = idx;
}

static void set_focus(int idx) {
    if (g_focused == idx) return;
    g_focused = idx;
    g_gui_dirty = 1;
}

// Обрабатывает ОДИН переход состояния кнопок мыши (down/up) за тик —
// click-to-focus + raise, перетаскивание титлбара, клик на [X] (шлёт
// GUI_EVENT_CLOSE владельцу, сама ничего не разрушает — см. gui.h), клик
// в клиентской области (шлёт GUI_EVENT_MOUSE_DOWN/UP с координатами
// относительно клиентского угла).
static void handle_mouse_transition(int mx, int my, int buttons) {
    int going_down = (buttons & 1) && !(g_prev_buttons & 1);
    int going_up = !(buttons & 1) && (g_prev_buttons & 1);

    if (going_down) {
        int idx;
        if (find_window_at(mx, my, &idx)) {
            gui_window_t *w = &g_windows[idx];
            raise_to_top(idx);
            set_focus(idx);
            g_gui_dirty = 1;

            int close_x = w->x + (int)outer_w(w) - GUI_BORDER - GUI_CLOSE_BTN_SIZE - 2;
            int close_y = w->y + (GUI_TITLEBAR_HEIGHT - GUI_CLOSE_BTN_SIZE) / 2;
            if (mx >= close_x && mx < close_x + GUI_CLOSE_BTN_SIZE &&
                my >= close_y && my < close_y + GUI_CLOSE_BTN_SIZE) {
                queue_push(w, GUI_EVENT_CLOSE, 0, 0, 0);
            } else if (my < w->y + GUI_TITLEBAR_HEIGHT) {
                // клик по титлбару (не по [X]) - начинаем перетаскивание
                g_dragging = idx;
                g_drag_off_x = mx - w->x;
                g_drag_off_y = my - w->y;
            } else {
                // клик в клиентской области
                int rel_x = mx - (w->x + GUI_BORDER);
                int rel_y = my - (w->y + GUI_TITLEBAR_HEIGHT);
                queue_push(w, GUI_EVENT_MOUSE_DOWN, rel_x, rel_y, buttons);
            }
        } else {
            set_focus(-1);
        }
    }

    if (going_up) {
        g_dragging = -1;
        if (g_focused >= 0 && g_windows[g_focused].in_use) {
            gui_window_t *w = &g_windows[g_focused];
            int rel_x = mx - (w->x + GUI_BORDER);
            int rel_y = my - (w->y + GUI_TITLEBAR_HEIGHT);
            queue_push(w, GUI_EVENT_MOUSE_UP, rel_x, rel_y, buttons);
        }
    }

    if (g_dragging >= 0 && (buttons & 1)) {
        gui_window_t *w = &g_windows[g_dragging];
        int nx = mx - g_drag_off_x;
        int ny = my - g_drag_off_y;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if ((uint32_t)nx + outer_w(w) > screen_width_pixels) nx = (int)(screen_width_pixels - outer_w(w));
        if ((uint32_t)ny + outer_h(w) > screen_height_pixels) ny = (int)(screen_height_pixels - outer_h(w));
        if (nx != w->x || ny != w->y) {
            w->x = nx;
            w->y = ny;
            g_gui_dirty = 1;
        }
    }
}

// Имя не draw_cursor() - уже есть console.c's draw_cursor(void) (рисует
// текстовый курсор-прямоугольник), это другая функция с другой сигнатурой.
static void gui_draw_cursor(int x, int y) {
    // Простая стрелка-треугольник без растровых ресурсов (см. "без
    // иконок" в шапке gui.h) - диагональная линия + короткая "пятка",
    // этого достаточно, чтобы видеть, куда указывает курсор.
    uint32_t white = convert_color(0xffffff);
    for (int i = 0; i < 12; i++) {
        gfx_draw_line(x, y, x + i, y + i, white);
    }
    gfx_draw_line(x, y, x, y + 12, white);
}

void gui_tick(void) {
    if (g_window_count == 0) return;

    int mx = input_mouse_get_x();
    int my = input_mouse_get_y();
    int buttons = input_mouse_get_buttons();
    if (buttons != g_prev_buttons || g_dragging >= 0) {
        handle_mouse_transition(mx, my, buttons);
    }
    g_prev_buttons = buttons;

    // НАЙДЕННЫЙ БАГ (живое тестирование: курсор не следовал за мышью) —
    // g_gui_dirty раньше выставлялся ТОЛЬКО переходами кнопок
    // (handle_mouse_transition()/окна), а просто движение мыши БЕЗ
    // клика никогда не помечало кадр "грязным" — gui_draw_cursor() ниже
    // хоть и читает АКТУАЛЬНУЮ mx/my при каждой реальной перерисовке, но
    // сама перерисовка из-за раннего выхода просто не происходила вовсе,
    // пока курсор двигали без зажатой кнопки.
    static int prev_mx = -1, prev_my = -1;
    if (mx != prev_mx || my != prev_my) {
        g_gui_dirty = 1;
        prev_mx = mx;
        prev_my = my;
    }

    if (!g_gui_dirty) return;
    g_gui_dirty = 0;

    // graphics2d.h: gfx_*/put_pixel ожидают УЖЕ сконвертированный цвет
    // (BGR/RGB в зависимости от PixelFormat, см. convert_color() в
    // console.c) - конвертируем константы ОДИН раз за кадр, не на
    // каждый вызов/пиксель (тот же приём, что и у остального кода,
    // см. graphics2d.h).
    uint32_t desktop_bg = convert_color(DESKTOP_BG);
    uint32_t border_c = convert_color(BORDER_COLOR);
    uint32_t titlebar_active = convert_color(TITLEBAR_ACTIVE);
    uint32_t titlebar_inactive = convert_color(TITLEBAR_INACTIVE);
    uint32_t title_text = convert_color(TITLE_TEXT_COLOR);
    uint32_t close_btn = convert_color(CLOSE_BTN_COLOR);
    uint32_t white = convert_color(0xffffff);

    gfx_fill_rect(0, 0, screen_width_pixels, screen_height_pixels, desktop_bg);

    for (int i = 0; i < g_window_count; i++) {
        gui_window_t *w = &g_windows[g_order[i]];
        if (!w->in_use) continue;

        int is_focused = (g_order[i] == g_focused);
        uint32_t ow = outer_w(w), oh = outer_h(w);
        uint32_t titlebar_c = is_focused ? titlebar_active : titlebar_inactive;

        gfx_fill_rect(w->x, w->y, ow, oh, border_c);
        gfx_fill_rect(w->x + GUI_BORDER, w->y, w->w, GUI_TITLEBAR_HEIGHT, titlebar_c);

        int tx = w->x + GUI_BORDER + 4;
        int ty = w->y + (GUI_TITLEBAR_HEIGHT - GUI_CHAR_HEIGHT) / 2;
        for (int c = 0; w->title[c] && tx + GUI_CHAR_WIDTH < w->x + (int)ow - GUI_CLOSE_BTN_SIZE - 6; c++) {
            put_char_graphic_px(w->title[c], tx, ty, title_text, titlebar_c);
            tx += GUI_CHAR_WIDTH;
        }

        int close_x = w->x + (int)ow - GUI_BORDER - GUI_CLOSE_BTN_SIZE - 2;
        int close_y = w->y + (GUI_TITLEBAR_HEIGHT - GUI_CLOSE_BTN_SIZE) / 2;
        gfx_fill_rect(close_x, close_y, GUI_CLOSE_BTN_SIZE, GUI_CLOSE_BTN_SIZE, close_btn);
        put_char_graphic_px('X', close_x + 4, close_y + 4, white, close_btn);

        gfx_blit(w->x + GUI_BORDER, w->y + GUI_TITLEBAR_HEIGHT, w->pixels, w->w, w->h, w->w);
    }

    gui_draw_cursor(mx, my);
    console_mark_dirty();
}

int gui_handle_key(int key) {
    if (!gui_mode_active()) return 0;
    if (g_focused < 0 || !g_windows[g_focused].in_use) return 1; // GUI активен, но фокуса нет - просто глотаем
    queue_push(&g_windows[g_focused], GUI_EVENT_KEY, 0, 0, key);
    return 1;
}

static int find_free_slot(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) if (!g_windows[i].in_use) return i;
    return -1;
}

int gui_window_create(uint32_t owner_pid, int x, int y, uint32_t w, uint32_t h, const char *title) {
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return -1;
    if (g_window_count >= GUI_MAX_WINDOWS) return -1;

    int idx = find_free_slot();
    if (idx < 0) return -1;

    uint32_t *pixels = (uint32_t*)kmalloc((size_t)w * h * sizeof(uint32_t));
    if (!pixels) return -1;
    memset(pixels, 0, (size_t)w * h * sizeof(uint32_t));

    gui_window_t *win = &g_windows[idx];
    win->in_use = 1;
    win->owner_pid = owner_pid;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->pixels = pixels;
    win->events.head = win->events.tail = win->events.count = 0;

    int n = 0;
    if (title) while (title[n] && n < GUI_TITLE_MAX - 1) { win->title[n] = title[n]; n++; }
    win->title[n] = '\0';

    g_order[g_window_count++] = idx;
    set_focus(idx);
    g_gui_dirty = 1;
    return idx;
}

// Проверка владения - тот же паттерн, что уже process_set_foreground()
// (process.c, v0.8-мост пункт 7): свой PID не проверяем по UID/root,
// просто сверяем с тем, кто создал окно - чужой процесс не может
// рисовать/закрыть/перетащить программно чужое окно по id.
static gui_window_t *get_owned(uint32_t owner_pid, int window_id) {
    if (window_id < 0 || window_id >= GUI_MAX_WINDOWS) return NULL;
    gui_window_t *w = &g_windows[window_id];
    if (!w->in_use || w->owner_pid != owner_pid) return NULL;
    return w;
}

int gui_window_destroy(uint32_t owner_pid, int window_id) {
    gui_window_t *w = get_owned(owner_pid, window_id);
    if (!w) return -1;

    kfree(w->pixels);
    w->pixels = NULL;
    w->in_use = 0;

    for (int i = 0; i < g_window_count; i++) {
        if (g_order[i] == window_id) {
            for (int j = i; j < g_window_count - 1; j++) g_order[j] = g_order[j + 1];
            g_window_count--;
            break;
        }
    }
    if (g_focused == window_id) {
        g_focused = (g_window_count > 0) ? g_order[g_window_count - 1] : -1;
    }
    if (g_dragging == window_id) g_dragging = -1;

    if (g_window_count == 0) {
        // Последнее окно закрыто - gui_tick() с этого момента даже не
        // зайдёт в отрисовку (g_window_count==0, ранний выход), так что
        // "грязный" back buffer с рабочим столом/окнами так и останется
        // на экране навсегда, если не перерисовать текстовую консоль
        // прямо сейчас.
        console_redraw_from_history();
    } else {
        g_gui_dirty = 1;
    }
    return 0;
}

int gui_window_fill(uint32_t owner_pid, int window_id, uint32_t color) {
    gui_window_t *w = get_owned(owner_pid, window_id);
    if (!w) return -1;
    uint32_t c = convert_color(color);
    for (uint32_t i = 0; i < w->w * w->h; i++) w->pixels[i] = c;
    g_gui_dirty = 1;
    return 0;
}

int gui_window_draw_rect(uint32_t owner_pid, int window_id, int x, int y, uint32_t w, uint32_t h, uint32_t color) {
    gui_window_t *win = get_owned(owner_pid, window_id);
    if (!win) return -1;
    uint32_t c = convert_color(color);

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + (int)w; if (x1 > (int)win->w) x1 = (int)win->w;
    int y1 = y + (int)h; if (y1 > (int)win->h) y1 = (int)win->h;

    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            win->pixels[py * win->w + px] = c;

    g_gui_dirty = 1;
    return 0;
}

int gui_window_draw_text(uint32_t owner_pid, int window_id, int x, int y, const char *text, uint32_t color) {
    gui_window_t *win = get_owned(owner_pid, window_id);
    if (!win || !text) return -1;
    uint32_t c = convert_color(color);

    // font_draw_glyph_to_buffer() (console.c, v0.8 GUI+WM) — тот же
    // битмап-шрифт 8x8, что и put_char_graphic()/put_char_graphic_px(),
    // но пишет в ПРОИЗВОЛЬНЫЙ вызывающий буфер (приватный буфер окна),
    // не в экран/back buffer напрямую.
    for (int i = 0; text[i]; i++) {
        font_draw_glyph_to_buffer(win->pixels, win->w, win->h,
                                  x + i * GUI_CHAR_WIDTH, y, (unsigned char)text[i], c);
    }
    g_gui_dirty = 1;
    return 0;
}

int gui_window_move(uint32_t owner_pid, int window_id, int x, int y) {
    gui_window_t *w = get_owned(owner_pid, window_id);
    if (!w) return -1;
    w->x = x;
    w->y = y;
    g_gui_dirty = 1;
    return 0;
}

int gui_window_poll_event(uint32_t owner_pid, int window_id, gui_event_t *out) {
    gui_window_t *w = get_owned(owner_pid, window_id);
    if (!w || !out) return 0;
    gui_event_queue_t *q = &w->events;
    if (q->count == 0) return 0;
    *out = q->events[q->head];
    q->head = (q->head + 1) % 32;
    q->count--;
    return 1;
}

void gui_destroy_windows_owned_by(uint32_t pid) {
    // Снимок id'ов за один проход - gui_window_destroy() сама правит
    // g_order/g_window_count по ходу, повторный проход по "живому"
    // массиву без снимка сдвигал бы индексы под ногами.
    int to_destroy[GUI_MAX_WINDOWS];
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (g_windows[i].in_use && g_windows[i].owner_pid == pid) to_destroy[n++] = i;
    }
    for (int i = 0; i < n; i++) gui_window_destroy(pid, to_destroy[i]);
}
