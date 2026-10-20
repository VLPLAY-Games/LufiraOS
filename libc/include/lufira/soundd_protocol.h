#pragma once

// soundd_protocol.h — зеркало kernel/system/ipc/soundd_protocol.h,
// байт-в-байт. Используется только самим soundd-процессом — обычные
// программы общаются через sys_sound_*() (syscall.h), которые прячут этот
// протокол внутри ядра.

#include <stdint.h>

#define SOUNDD_OP_INFO       1
#define SOUNDD_OP_SET_VOLUME 2
#define SOUNDD_OP_STOP       3

struct __attribute__((packed)) soundd_request {
    uint32_t opcode;
    int32_t a[2];
};

struct __attribute__((packed)) soundd_reply {
    int32_t result;
    int32_t a[2];
    char str[16];
};
