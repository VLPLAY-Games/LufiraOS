#pragma once

// wm_protocol.h — зеркало kernel/system/ipc/wm_protocol.h, байт-в-байт.
// Используется ТОЛЬКО самим WM-процессом (lufira-packages/src/apps/wm.c) —
// обычные GUI-приложения общаются через sys_win_*() (syscall.h), которые
// прячут этот протокол под собой внутри ядра. См. подробный разбор
// архитектуры в kernel-side версии этого файла.

#include <stdint.h>

#define WM_SENDER_KERNEL 0

#define WM_INPUT_KEY   100
#define WM_INPUT_MOUSE 101
#define WM_NOTIFY_PROCESS_EXIT 102 // a[0] = pid завершившегося процесса — см. kernel-side версию этого файла

#define WM_OP_WIN_CREATE     1
#define WM_OP_WIN_DESTROY    2
#define WM_OP_WIN_FILL       3
#define WM_OP_WIN_DRAW_RECT  4
#define WM_OP_WIN_DRAW_TEXT  5
#define WM_OP_WIN_POLL_EVENT 6
#define WM_OP_WIN_MOVE       7

struct __attribute__((packed)) wm_request {
    uint32_t opcode;
    int32_t a[6];
    char str[64];
};

struct __attribute__((packed)) wm_reply {
    int32_t result;
    int32_t ev_type, ev_x, ev_y, ev_key;
};
