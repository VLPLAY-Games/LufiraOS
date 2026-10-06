#pragma once

// gui_widgets.h — v0.8 (GUI+WM), второй срез: минимальный набор виджетов
// поверх SYS_WIN_* (syscall.h) — кнопка и однострочное текстовое поле.
// Живёт в libc.so (та же разделяемая библиотека, что string/malloc/
// printf/stdlib — см. dynlink.c, LufiraOS/kernel/system/elf/) — ВТОРУЮ
// отдельную .so заводить не стали: dynlink.c сегодня принципиально
// рассчитан на ОДНУ библиотеку (см. его же комментарий у DT_NEEDED), и
// под этот срез заводить вторую не было нужды — виджеты это же просто
// функции поверх уже существующих sys_win_*().
//
// Виджеты НЕ хранят номер окна (win) внутри себя — тот же объект можно
// нарисовать в любое окно, координаты всегда КЛИЕНТСКИЕ (как и у самих
// sys_win_draw_*()). Код приложения сам решает, когда перерисовывать
// (обычно: после каждого обработанного события, которое вернуло "что-то
// изменилось").

#include <lufira/syscall.h>

typedef struct {
    int x, y, w, h;
    char label[32];
    uint32_t bg, fg;
} gui_button_t;

void gui_button_init(gui_button_t *b, int x, int y, int w, int h, const char *label);
void gui_button_draw(int win, const gui_button_t *b);
// 1, если (x,y) — клиентские координаты клика — попадают в кнопку.
int gui_button_contains(const gui_button_t *b, int x, int y);

#define GUI_TEXTBOX_MAX 127

typedef struct {
    int x, y, w, h;
    char text[GUI_TEXTBOX_MAX + 1];
    int len;
    int focused; // влияет только на цвет рамки/наличие курсора при отрисовке
} gui_textbox_t;

void gui_textbox_init(gui_textbox_t *t, int x, int y, int w, int h);
void gui_textbox_draw(int win, const gui_textbox_t *t);
int gui_textbox_contains(const gui_textbox_t *t, int x, int y);
// key — как в lufira_gui_event.key_or_button (сырой код: печатные ASCII
// 32-126, '\b' — стереть последний символ, прочее молча игнорируется).
// Возвращает 1, если текст действительно изменился (надо перерисовать).
int gui_textbox_handle_key(gui_textbox_t *t, int key);
