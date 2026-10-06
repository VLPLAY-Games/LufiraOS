#pragma once

// gui_widgets.h — v0.8 (GUI+WM): набор виджетов поверх SYS_WIN_*
// (syscall.h) — кнопка, однострочное текстовое поле (второй срез),
// чекбокс и список (этап 3, "больше виджетов").
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

// Квадратная рамка WxH пикселей (см. GUI_CHECKBOX_SIZE в gui_widgets.c) +
// надпись справа — thin wrapper в духе gui_button_t выше.
#define GUI_CHECKBOX_LABEL_MAX 32

typedef struct {
    int x, y;
    char label[GUI_CHECKBOX_LABEL_MAX];
    int checked;
} gui_checkbox_t;

void gui_checkbox_init(gui_checkbox_t *c, int x, int y, const char *label);
void gui_checkbox_draw(int win, const gui_checkbox_t *c);
// Клик попал в квадрат ИЛИ в текст рядом (вся строка кликабельна, как в
// большинстве тулкитов — так проще целиться мышью).
int gui_checkbox_contains(const gui_checkbox_t *c, int x, int y);
// Переключает checked, возвращает новое значение. Вызывающий сам решает,
// звать ли это (обычно — после gui_checkbox_contains() на MOUSE_DOWN).
int gui_checkbox_toggle(gui_checkbox_t *c);

// Список строк с выделением по клику. БЕЗ прокрутки в этой версии —
// показывает первые (h / GUI_LISTBOX_ROW_H) элементов из item_count;
// элементов, не поместившихся по высоте, просто не видно (нет события
// колеса мыши в этой GUI, прокручивать пока нечем — см. gui.c/wm.c).
#define GUI_LISTBOX_MAX_ITEMS 64
#define GUI_LISTBOX_ITEM_MAX  48
#define GUI_LISTBOX_ROW_H     16

typedef struct {
    int x, y, w, h;
    char items[GUI_LISTBOX_MAX_ITEMS][GUI_LISTBOX_ITEM_MAX];
    int item_count;
    int selected; // -1 - нет выделения
} gui_listbox_t;

void gui_listbox_init(gui_listbox_t *l, int x, int y, int w, int h);
void gui_listbox_clear(gui_listbox_t *l);
// Возвращает индекс добавленного элемента, -1 если список уже полон
// (GUI_LISTBOX_MAX_ITEMS). Текст обрезается до GUI_LISTBOX_ITEM_MAX-1.
int gui_listbox_add_item(gui_listbox_t *l, const char *text);
void gui_listbox_draw(int win, const gui_listbox_t *l);
// (x,y) — клиентские координаты клика. Если попали в видимый элемент —
// выделяет его и возвращает индекс; иначе возвращает -1 и selected не
// трогает (клик мимо списка не снимает выделение — как и у gui_textbox_t,
// виджет сам не решает это за приложение).
int gui_listbox_click(gui_listbox_t *l, int x, int y);
