#pragma once

// gui.c — v0.8 (GUI + WM), первый срез. Явное архитектурное решение
// (принято по ходу, см. объяснение в чате): оконный менеджер живёт В
// ЯДРЕ, как и всё остальное в этой ОС (VFS/process/console — нигде нет
// отдельного userspace-демона с собственным протоколом IPC) — окна это
// кернел-резидентные структуры, доступные userspace только через
// syscall'ы (SYS_WIN_*, syscall.h). Содержимое окна (pixels) НЕ
// отображается в адресное пространство процесса вовсе — клиент рисует
// только через syscall'ы (SYS_WIN_FILL/DRAW_RECT/DRAW_TEXT), ядро само
// пишет в свой kmalloc'd буфер. Это проще, чем настоящая shared-memory
// модель клиент/сервер (которой тут пока неоткуда взяться — нет
// именованной межпроцессной shm, только fork-time MAP_SHARED, см.
// shm.c), и достаточно для первого среза.
//
// БЕЗ ИКОНОК (прямое указание пользователя: "их пока негде брать") —
// заголовок окна и кнопка закрытия рисуются существующим битмап-шрифтом
// (put_char_graphic(), console.c), не растровыми картинками.
//
// Композитинг переиспользует готовую double-buffering инфраструктуру
// v0.8-моста, пункта 2 (console.c: console_mark_dirty()/gfx_present()) —
// gui_tick_present() рисует рабочий стол + окна + курсор в тот же back
// buffer, что и текстовая консоль, и так же помечает его "грязным";
// реальный флаш на экран делает уже существующий console_tick_present()
// (вызывается следом из timer_irq_handler(), pit.c).

#include "lib/types.h"

#define GUI_MAX_WINDOWS      16
#define GUI_TITLE_MAX        32
#define GUI_TITLEBAR_HEIGHT  20
#define GUI_BORDER           2
#define GUI_CLOSE_BTN_SIZE   16

// События окна (SYS_WIN_POLL_EVENT) — та же "неблокирующая проверка,
// 0/1 возврат" форма, что уже у SYS_POLL (item 3 моста), не блокирующая
// пара read()/write().
#define GUI_EVENT_NONE       0
#define GUI_EVENT_KEY        1 // key_or_button = код клавиши (см. keyboard.h)
#define GUI_EVENT_MOUSE_DOWN 2 // x,y — координаты ОТНОСИТЕЛЬНО клиентской области; key_or_button = кнопка
#define GUI_EVENT_MOUSE_UP   3
#define GUI_EVENT_CLOSE      4 // пользователь нажал [X] — приложение само решает, выйти ли

typedef struct {
    int type;
    int x, y;
    int key_or_button;
} gui_event_t;

void gui_init(void);

// Каждый кадр (вызывается из timer_irq_handler(), pit.c) — обрабатывает
// переходы кнопок мыши (click-to-focus, drag титлбара), затем
// перерисовывает рабочий стол/окна/курсор в back buffer, если GUI-режим
// активен и что-то действительно изменилось (dirty-флаг самого gui.c —
// отдельный от console_mark_dirty(), см. gui.c).
void gui_tick(void);

int gui_mode_active(void);

// Перехват клавиатуры ДО обычного пути консоли (input.c,
// input_keyboard_event()) — возвращает 1, если клавиша "съедена" GUI
// (значит в обычный ring buffer/Ctrl+C её пускать не надо), иначе 0
// (GUI неактивен или нет сфокусированного окна — обычный путь как
// раньше).
int gui_handle_key(int key);

// SYS_WIN_* — реализации в syscall.c, тонкие обёртки поверх этих.
// owner_pid — current_process->pid вызывающего (кому принадлежит окно;
// нужно для автоматической очистки при выходе процесса, см.
// gui_destroy_windows_owned_by(), и чтобы чужой процесс не мог рисовать
// в твоё окно по id).
int gui_window_create(uint32_t owner_pid, int x, int y, uint32_t w, uint32_t h, const char *title);
int gui_window_destroy(uint32_t owner_pid, int window_id);
int gui_window_fill(uint32_t owner_pid, int window_id, uint32_t color);
int gui_window_draw_rect(uint32_t owner_pid, int window_id, int x, int y, uint32_t w, uint32_t h, uint32_t color);
int gui_window_draw_text(uint32_t owner_pid, int window_id, int x, int y, const char *text, uint32_t color);
int gui_window_move(uint32_t owner_pid, int window_id, int x, int y);
// Возвращает 1 и заполняет *out, если у окна есть событие в очереди, иначе 0.
int gui_window_poll_event(uint32_t owner_pid, int window_id, gui_event_t *out);

// Зовётся из process_exit()/зомби-уборки (process.c) — так же, как уже
// существующий сброс foreground_pid при выходе владельца: процесс не
// обязан сам прибрать свои окна перед смертью (крах/kill), иначе они
// висели бы на экране вечно, принадлежа уже не существующему PID.
void gui_destroy_windows_owned_by(uint32_t pid);
