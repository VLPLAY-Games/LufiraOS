#pragma once

// wm_protocol.h — формат сообщений ОКОННОГО сервера поверх общего
// mailbox.h. Используется и ядром (syscall.c — релеит клиентские SYS_WIN_*
// в WM-процесс; input.c — проталкивание сырых событий ввода), и userspace
// WM-процессом (lufira-packages/src/apps/wm.c) — обе стороны обязаны видеть
// байт-в-байт одинаковый layout (зеркало в libc/include/lufira/syscall.h
// называется так же).
//
// SYS_WIN_* внутри syscall.c — тонкие RPC-обёртки: упаковать wm_request_t,
// mailbox_send() зарегистрированному WM pid (process_get_wm_pid()),
// заблокированно дождаться wm_reply_t тем же mailbox_recv(). Каждый
// клиентский процесс отправляет РОВНО один запрос и сразу блокируется на
// ответ, поэтому корреляция запрос/ответ по id не нужна: один mailbox,
// один запрос в полёте.

#include "lib/types.h"

// sender_pid во входящем ipc_msg_t (mailbox.h) == 0 — сообщение не от
// клиента, а от самого ядра (прямой вызов mailbox_send() из input.c,
// см. ниже) — настоящие pid никогда не достигают 0 (idle-процесс).
#define WM_SENDER_KERNEL 0

// Опкоды "от ядра" (sender_pid==0) — сырой ввод, WM САМ решает фокус/
// маршрутизацию (раньше это делал gui_handle_key()/input_mouse_get_*()
// внутри ядра — теперь вся эта логика переехала в userspace).
#define WM_INPUT_KEY   100 // a[0] = код клавиши (как раньше передавался в gui_handle_key()), a[1] = 1 нажата / 0 отпущена
#define WM_INPUT_MOUSE 101 // a[0]=x, a[1]=y (абсолютные, накопленные), a[2]=биты кнопок

// Процесс не обязан сам прибрать свои окна перед смертью (краш/kill) —
// иначе они висели бы на экране вечно, принадлежа несуществующему PID.
// Ядро шлёт WM это уведомление (a[0] = pid завершившегося процесса), и WM
// сам ищет и закрывает все окна с этим owner_pid (см. wm.c).
#define WM_NOTIFY_PROCESS_EXIT 102 // a[0] = pid завершившегося процесса

// Опкоды RPC-запросов от клиентов (sender_pid == их pid) — зеркалят
// SYS_WIN_* 1:1, см. syscall.h.
#define WM_OP_WIN_CREATE     1 // a[0..3]=x,y,w,h; str=title
#define WM_OP_WIN_DESTROY    2 // a[0]=window_id
#define WM_OP_WIN_FILL       3 // a[0]=window_id, a[1]=color
#define WM_OP_WIN_DRAW_RECT  4 // a[0]=window_id, a[1..4]=x,y,w,h, a[5]=color
#define WM_OP_WIN_DRAW_TEXT  5 // a[0]=window_id, a[1..2]=x,y, a[3]=color; str=text
#define WM_OP_WIN_POLL_EVENT 6 // a[0]=window_id
#define WM_OP_WIN_MOVE       7 // a[0]=window_id, a[1..2]=x,y

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    int32_t a[6];
    char str[64];
} wm_request_t;

// result: у WM_OP_WIN_CREATE — id окна (>=0) либо -1; у DESTROY/FILL/
// DRAW_RECT/DRAW_TEXT/MOVE — 0/-1; у POLL_EVENT — 0 (нет события) либо 1
// (событие есть, ev_* заполнены — те же значения, что раньше были в
// gui_event_t.{type,x,y,key_or_button}, GUI_EVENT_* из удалённого gui.h;
// теперь ядро этих констант вовсе не знает, просто прозрачно передаёт
// int'ы между WM и lufira_gui_event_t клиента).
typedef struct __attribute__((packed)) {
    int32_t result;
    int32_t ev_type, ev_x, ev_y, ev_key;
} wm_reply_t;
