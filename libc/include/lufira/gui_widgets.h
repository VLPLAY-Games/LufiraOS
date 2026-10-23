#pragma once

// gui_widgets.h — набор виджетов поверх SYS_WIN_* (syscall.h): кнопка,
// текстовое поле, чекбокс, список. Живёт в libc.so (dynlink.c рассчитан
// ровно на одну разделяемую библиотеку) — виджеты это просто функции
// поверх уже существующих sys_win_*().
//
// Виджеты НЕ хранят номер окна внутри себя — тот же объект можно
// нарисовать в любое окно, координаты всегда КЛИЕНТСКИЕ. Приложение само
// решает, когда перерисовывать (обычно — после события, изменившего
// состояние).
//
// dirty (0.9.5, partial-dirty — см. WM_OP_WIN_MARK_DIRTY): каждый виджет
// сам отслеживает, менялось ли его содержимое с последней отрисовки —
// *_init() взводит его (нужен первый draw), мутирующие функции библиотеки
// (toggle/handle_key/scroll/add_item/clear/click-с-изменением-выделения)
// взводят его заново при реальном изменении, а *_draw() гасит его в конце.
// *_draw() САМ ничего не пропускает и рисует всегда безусловно, как и
// раньше — так уже сложившийся у приложений паттерн "залить окно и
// перерисовать всё заново" (calc.c и т.п., см. redraw()) не ломается: fill()
// стирает пиксели виджета независимо от того, менялось ли его СОДЕРЖИМОЕ,
// так что безусловный skip внутри *_draw() был бы невидимой дырой в фоне.
// dirty — ПОДСКАЗКА ВЫЗЫВАЮЩЕМУ: новый код (миграция на 1.0, когда фон не
// заливается целиком каждый раз) может сам решать, звать ли *_draw()
// вообще, глядя на dirty, и завершать явный sys_win_mark_dirty() только
// вокруг того, что реально изменилось — старым приложениям, которые это
// поле не читают, оно совершенно не мешает (чистая добавка, без смены
// поведения).

#include <lufira/syscall.h>

typedef struct {
    int x, y, w, h;
    char label[32];
    uint32_t bg, fg;
    int dirty; // см. комментарий вверху файла
} gui_button_t;

void gui_button_init(gui_button_t *b, int x, int y, int w, int h, const char *label);
// Не const — гасит b->dirty после отрисовки (см. комментарий вверху файла).
void gui_button_draw(int win, gui_button_t *b);
// 1, если (x,y) — клиентские координаты клика — попадают в кнопку.
int gui_button_contains(const gui_button_t *b, int x, int y);

#define GUI_TEXTBOX_MAX 127

typedef struct {
    int x, y, w, h;
    char text[GUI_TEXTBOX_MAX + 1];
    int len;
    int focused; // влияет только на цвет рамки/наличие курсора при отрисовке
    int dirty; // см. комментарий вверху файла
} gui_textbox_t;

void gui_textbox_init(gui_textbox_t *t, int x, int y, int w, int h);
// Не const — гасит t->dirty после отрисовки (см. комментарий вверху файла).
void gui_textbox_draw(int win, gui_textbox_t *t);
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
    int dirty; // см. комментарий вверху файла
} gui_checkbox_t;

void gui_checkbox_init(gui_checkbox_t *c, int x, int y, const char *label);
// Не const — гасит c->dirty после отрисовки (см. комментарий вверху файла).
void gui_checkbox_draw(int win, gui_checkbox_t *c);
// Клик попал в квадрат ИЛИ в текст рядом (вся строка кликабельна, как в
// большинстве тулкитов — так проще целиться мышью).
int gui_checkbox_contains(const gui_checkbox_t *c, int x, int y);
// Переключает checked, возвращает новое значение. Вызывающий сам решает,
// звать ли это (обычно — после gui_checkbox_contains() на MOUSE_DOWN).
int gui_checkbox_toggle(gui_checkbox_t *c);

// Список строк с выделением по клику и вертикальной прокруткой —
// scroll_offset — индекс первого видимого элемента. Колеса мыши в этой GUI
// нет, поэтому прокрутку водит вызывающий через gui_listbox_scroll()
// (обычно от пары кнопок "▲"/"▼" рядом со списком). Справа — тонкая
// полоса-индикатор позиции (не перетаскиваемая).
#define GUI_LISTBOX_MAX_ITEMS 64
#define GUI_LISTBOX_ITEM_MAX  48
#define GUI_LISTBOX_ROW_H     16
#define GUI_LISTBOX_SCROLLBAR_W 6

typedef struct {
    int x, y, w, h;
    char items[GUI_LISTBOX_MAX_ITEMS][GUI_LISTBOX_ITEM_MAX];
    int item_count;
    int selected;      // -1 - нет выделения (абсолютный индекс, не видимая строка)
    int scroll_offset; // индекс первого показанного элемента
    int dirty; // см. комментарий вверху файла
} gui_listbox_t;

void gui_listbox_init(gui_listbox_t *l, int x, int y, int w, int h);
void gui_listbox_clear(gui_listbox_t *l);
// Возвращает индекс добавленного элемента, -1 если список уже полон
// (GUI_LISTBOX_MAX_ITEMS). Текст обрезается до GUI_LISTBOX_ITEM_MAX-1.
int gui_listbox_add_item(gui_listbox_t *l, const char *text);
// Не const — гасит l->dirty после отрисовки (см. комментарий вверху файла).
void gui_listbox_draw(int win, gui_listbox_t *l);
// (x,y) — клиентские координаты клика. Если попали в видимый элемент —
// выделяет его и возвращает АБСОЛЮТНЫЙ индекс; иначе возвращает -1 и
// selected не трогает (клик мимо списка не снимает выделение — как и у
// gui_textbox_t, виджет сам не решает это за приложение).
int gui_listbox_click(gui_listbox_t *l, int x, int y);
// Сдвигает scroll_offset на delta строк (отрицательное — вверх), зажимая
// в [0, max(0, item_count - видимых_строк)]. Возвращает 1, если позиция
// реально изменилась (вызывающему решать, перерисовывать ли) — 0 иначе
// (уже у края).
int gui_listbox_scroll(gui_listbox_t *l, int delta);
