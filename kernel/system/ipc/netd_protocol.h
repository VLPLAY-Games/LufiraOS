#pragma once

// netd_protocol.h — формат control-plane сообщений сетевого демона поверх
// общего mailbox.h, тот же принцип, что wm_protocol.h/soundd_protocol.h.
// Используется ядром (syscall.c — релеит SYS_NET_FETCH/SYS_PING/
// SYS_NET_CONFIG_* через service_call()) и userspace-процессом netd
// (lufira-packages/daemons/netd/) — обе стороны обязаны видеть байт-в-байт
// одинаковый layout (зеркало в libc/include/lufira/netd_protocol.h).
//
// NETD_OP_FETCH не несёт ни URL, ни тело HTTP-ответа само — 96-байтный
// payload mailbox'а (MAX_IPC_MSG_PAYLOAD) слишком тесен для обоих (реальные
// URL пакетного индекса уже длиннее любого разумного усечения — см. историю
// бага: "https://raw.githubusercontent.com/.../index.json" — 89 байт, не
// влезал в прежнее поле str[68], фетч тихо уходил по обрезанному пути и
// получал 404). Вместо этого ядро заранее выделяет ДВЕ shm-области (shm.c,
// см. sys_net_fetch()): url_shm_id — ровно одна страница с самой строкой
// URL (USER_STRING_MAX == PAGE_SIZE == 4096, влезает всегда без усечения),
// body_shm_id — под cap байт тела ответа (0, если cap==0). netd сам
// SYS_SHM_ATTACH обе, читает URL и пишет ответ, SYS_MUNMAP перед ответом —
// ядро читает готовые данные через phys_to_virt() (ему для этого не нужно
// ни ATTACH, ни чужая страница таблиц).

#include "lib/types.h"

#define NETD_OP_FETCH      1 // url_shm_id, body_shm_id, cap
#define NETD_OP_PING       2 // a[0]=dst_ip, a[1]=id, a[2]=seq, a[3]=timeout_ms
#define NETD_OP_CONFIG_GET 3 // без аргументов
#define NETD_OP_CONFIG_SET 4 // a[0]=ip, a[1]=netmask, a[2]=gateway

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    uint32_t url_shm_id;  // только FETCH — одна страница, сама строка URL (NUL-терминирована)
    uint32_t body_shm_id; // только FETCH — область под тело ответа, 0 если cap==0
    uint32_t cap;         // только FETCH — размер body_shm_id в байтах (out_cap)
    int32_t  a[4];
} netd_request_t;

typedef struct __attribute__((packed)) {
    int32_t  result; // FETCH: длина тела/отрицательный HTTP_FETCH_E*; PING: RTT мс/ошибка; CONFIG_*: 0
    int32_t  a[2];   // FETCH: a[0]=HTTP статус; CONFIG_GET: a[0]=link_up
    uint8_t  mac[6]; // CONFIG_GET
    uint32_t our_ip, netmask, gateway, dns_server; // CONFIG_GET
} netd_reply_t;
