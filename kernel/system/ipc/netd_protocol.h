#pragma once

// netd_protocol.h — формат control-plane сообщений сетевого демона поверх
// общего mailbox.h, тот же принцип, что wm_protocol.h/soundd_protocol.h.
// Используется ядром (syscall.c — релеит SYS_NET_FETCH/SYS_PING/
// SYS_NET_CONFIG_* через service_call()) и userspace-процессом netd
// (lufira-packages/daemons/netd/) — обе стороны обязаны видеть байт-в-байт
// одинаковый layout (зеркало в libc/include/lufira/netd_protocol.h).
//
// NETD_OP_FETCH не несёт тело HTTP-ответа само — оно не умещается в
// 96-байтный payload mailbox'а. Вместо этого ядро заранее выделяет shm-
// область (shm.c, см. sys_net_fetch()) под cap байт, shm_id передаётся
// здесь; netd сам SYS_SHM_ATTACH её, пишет ответ прямо туда, SYS_MUNMAP
// перед ответом — ядро читает уже готовые данные через phys_to_virt()
// (ему для этого не нужно ни ATTACH, ни чужая страница таблиц).

#include "lib/types.h"

#define NETD_OP_FETCH      1 // shm_id, cap; str=URL (усечён, см. комментарий поля)
#define NETD_OP_PING       2 // a[0]=dst_ip, a[1]=id, a[2]=seq, a[3]=timeout_ms
#define NETD_OP_CONFIG_GET 3 // без аргументов
#define NETD_OP_CONFIG_SET 4 // a[0]=ip, a[1]=netmask, a[2]=gateway

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    uint32_t shm_id; // только FETCH
    uint32_t cap;    // только FETCH — размер области/out_cap в байтах
    int32_t  a[4];
    // FETCH: URL. 96-байтный payload mailbox'а не резиновый (см.
    // MAX_IPC_MSG_PAYLOAD, mailbox.h) — длинные URL усекаются тем же
    // приёмом, что уже применяется к заголовкам окон (SYS_WIN_CREATE,
    // wm_protocol.h): на практике URL пакетного индекса/релиза короче.
    char str[68];
} netd_request_t;

typedef struct __attribute__((packed)) {
    int32_t  result; // FETCH: длина тела/отрицательный HTTP_FETCH_E*; PING: RTT мс/ошибка; CONFIG_*: 0
    int32_t  a[2];   // FETCH: a[0]=HTTP статус; CONFIG_GET: a[0]=link_up
    uint8_t  mac[6]; // CONFIG_GET
    uint32_t our_ip, netmask, gateway, dns_server; // CONFIG_GET
} netd_reply_t;
