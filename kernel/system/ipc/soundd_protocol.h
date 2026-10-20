#pragma once

// soundd_protocol.h — формат control-plane сообщений звукового демона
// поверх общего mailbox.h, тот же принцип, что wm_protocol.h. Используется
// ядром (syscall.c — релеит SYS_SOUND_INFO/SET_VOLUME/STOP через
// service_call()) и userspace-процессом soundd (lufira-packages/daemons/
// soundd.c) — обе стороны обязаны видеть байт-в-байт одинаковый layout
// (зеркало в libc/include/lufira/soundd_protocol.h).
//
// PCM-данные (SYS_SOUND_PLAY_PCM/SYS_SOUND_CONSUME) НЕ ходят через этот
// протокол — они не умещаются в 96-байтный payload mailbox'а, см. кольцо
// sound_ring_write()/sound_ring_consume() в syscall.c.

#include "lib/types.h"

#define SOUNDD_OP_INFO       1 // -> result: available (0/1); a[0]=volume, a[1]=sample_rate; str=имя драйвера
#define SOUNDD_OP_SET_VOLUME 2 // a[0]=volume (0-100)
#define SOUNDD_OP_STOP       3 // без аргументов — останавливает аппаратный поток

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    int32_t a[2];
} soundd_request_t;

typedef struct __attribute__((packed)) {
    int32_t result;
    int32_t a[2];
    char str[16];
} soundd_reply_t;
