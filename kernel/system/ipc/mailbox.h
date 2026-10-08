#pragma once

// mailbox.c — v0.8 (GUI+WM), этап 3: generic IPC-примитив, НЕ привязанный к
// GUI. До этого момента у двух НЕродственных процессов не было способа ни
// поделиться памятью (shm.c — только fork-time MAP_SHARED), ни обменяться
// сообщением (vfs_pipe() — только через наследование fd при fork()). Нужен
// был именно для переноса WM из ядра в userspace-процесс (см. kernel/system/
// ipc/wm_protocol.h) — но сам этот файл ничего не знает про окна: один
// почтовый ящик на процесс, адресация по pid, фиксированный размер
// сообщения, копирование через ядро (та же идея, что и у pipe_t в vfs.c,
// только датаграммами и для ЛЮБЫХ, не только родственных, процессов).
//
// Блокировка/пробуждение — тот же приём, что и у pipe_t.read_waiter/
// console_input_waiter (один слот на ящик, этого достаточно: процесс в
// этой ОС однопоточный, одновременно заблокироваться в своём же recv()
// дважды он не может). Единственное отличие от pipe/console: mailbox_send()
// вызывается не только из обычного syscall-контекста (другой процесс), но
// и ПРЯМО ИЗ IRQ-обработчиков клавиатуры/мыши (input.c, см. wm_protocol.h) —
// поэтому мутация кольцевого буфера защищена cli/sti (mailbox_lock()/
// unlock(), тот же приём, что и console_write_lock() в vfs.c, по той же
// причине: без этого таймерный/клавиатурный IRQ мог бы вклиниться прямо
// посреди чужого push/pop и испортить head/tail/count).

#include "lib/types.h"

#define MAX_IPC_MSG_PAYLOAD   96
#define MAX_IPC_MAILBOX_SLOTS 32

struct process; // избегаем include-цикла с process.h — нужен только как указатель

typedef struct {
    uint32_t sender_pid;
    uint32_t len;
    uint8_t payload[MAX_IPC_MSG_PAYLOAD];
} ipc_msg_t;

typedef struct {
    ipc_msg_t slots[MAX_IPC_MAILBOX_SLOTS];
    int head, tail, count;
    struct process *waiter; // заблокирован в mailbox_recv(), ждёт непустого ящика
} ipc_mailbox_t;

void mailbox_init(ipc_mailbox_t *mb);

// Копирует сообщение в почтовый ящик процесса dest_pid. sender_pid==0 —
// сентинел "от ядра" (input.c, см. wm_protocol.h) — реальные pid никогда не
// достигают 0 (idle-процесс). Возвращает 0 при успехе, -1 если dest_pid не
// найден или len > MAX_IPC_MSG_PAYLOAD, -2 если ящик адресата полон
// (SYS_IPC_SEND транслирует это в -EAGAIN — на практике не должно
// происходить: глубина очереди 32 заведомо больше числа одновременных
// клиентов WM).
int mailbox_send(uint32_t dest_pid, uint32_t sender_pid, const void *data, uint32_t len);

// Забирает следующее сообщение СВОЕГО (current_process) почтового ящика
// целиком (ipc_msg_t, тот же layout, что и userspace-зеркало
// lufira_ipc_msg_t, libc/include/lufira/syscall.h) в out. timeout_ms < 0 —
// ждать НЕОГРАНИЧЕННО (настоящая блокировка PROCESS_BLOCKED с явным
// пробуждением из mailbox_send(), нулевая стоимость CPU, пока ничего не
// приходит); 0 — вернуть -1 немедленно, если ящик пуст; >0 — ждать не
// больше стольки мс (тот же приём опроса с шагом в один тик, что и у
// SYS_POLL, syscall.h, — нужно самим проснуться по истечении таймаута,
// не только по чужому mailbox_send()). Возвращает 0 при успехе, иначе -1.
int mailbox_recv(void *out, int timeout_ms);
