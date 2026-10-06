// gui_widgets.c — v0.8 (GUI+WM), второй срез. См. архитектурный разбор в
// lufira/gui_widgets.h.

#include <lufira/gui_widgets.h>
#include <string.h>

#define BTN_BG       0x3a6ea5
#define BTN_FG       0xffffff
#define BTN_BORDER   0x1a1a22
#define TB_BG        0x15151d
#define TB_BORDER_FOCUSED   0x3a6ea5
#define TB_BORDER_UNFOCUSED 0x4a4a55
#define TB_FG        0xffffff
#define TB_CURSOR    0xffffff

void gui_button_init(gui_button_t *b, int x, int y, int w, int h, const char *label) {
    b->x = x; b->y = y; b->w = w; b->h = h;
    b->bg = BTN_BG;
    b->fg = BTN_FG;
    int n = 0;
    if (label) while (label[n] && n < (int)sizeof(b->label) - 1) { b->label[n] = label[n]; n++; }
    b->label[n] = '\0';
}

void gui_button_draw(int win, const gui_button_t *b) {
    sys_win_draw_rect(win, b->x, b->y, (unsigned)b->w, (unsigned)b->h, BTN_BORDER);
    sys_win_draw_rect(win, b->x + 1, b->y + 1, (unsigned)(b->w - 2), (unsigned)(b->h - 2), b->bg);

    int label_len = (int)strlen(b->label);
    int text_w = label_len * 8; // битмап-шрифт 8x8 - см. gui.c/console.c
    int tx = b->x + (b->w - text_w) / 2;
    int ty = b->y + (b->h - 8) / 2;
    if (tx < b->x) tx = b->x;
    sys_win_draw_text(win, tx, ty, b->label, b->fg);
}

int gui_button_contains(const gui_button_t *b, int x, int y) {
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

void gui_textbox_init(gui_textbox_t *t, int x, int y, int w, int h) {
    t->x = x; t->y = y; t->w = w; t->h = h;
    t->text[0] = '\0';
    t->len = 0;
    t->focused = 0;
}

void gui_textbox_draw(int win, const gui_textbox_t *t) {
    uint32_t border = t->focused ? TB_BORDER_FOCUSED : TB_BORDER_UNFOCUSED;
    sys_win_draw_rect(win, t->x, t->y, (unsigned)t->w, (unsigned)t->h, border);
    sys_win_draw_rect(win, t->x + 1, t->y + 1, (unsigned)(t->w - 2), (unsigned)(t->h - 2), TB_BG);

    int tx = t->x + 4;
    int ty = t->y + (t->h - 8) / 2;
    sys_win_draw_text(win, tx, ty, t->text, TB_FG);

    if (t->focused) {
        int cursor_x = tx + t->len * 8;
        sys_win_draw_rect(win, cursor_x, ty, 1, 8, TB_CURSOR);
    }
}

int gui_textbox_contains(const gui_textbox_t *t, int x, int y) {
    return x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h;
}

int gui_textbox_handle_key(gui_textbox_t *t, int key) {
    if (key == '\b') {
        if (t->len == 0) return 0;
        t->len--;
        t->text[t->len] = '\0';
        return 1;
    }
    if (key < 32 || key > 126) return 0; // '\n'/'\t'/спец-коды - не печатные, игнорируем
    if (t->len >= GUI_TEXTBOX_MAX) return 0;

    // Сколько символов реально влезает по ширине поля (минус отступы
    // слева/справа и место под курсор) - не даём тексту убежать за край.
    int max_visible = (t->w - 8 - 8) / 8;
    if (max_visible < 1) max_visible = 1;
    if (t->len >= max_visible) return 0;

    t->text[t->len++] = (char)key;
    t->text[t->len] = '\0';
    return 1;
}
