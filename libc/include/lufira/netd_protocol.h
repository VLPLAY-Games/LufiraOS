#pragma once

// netd_protocol.h — зеркало kernel/system/ipc/netd_protocol.h, байт-в-байт.
// Используется только самим netd-процессом — обычные программы общаются
// через sys_net_fetch()/sys_ping()/sys_net_config_*() (syscall.h), которые
// прячут этот протокол внутри ядра.

#include <stdint.h>

#define NETD_OP_FETCH      1
#define NETD_OP_PING       2
#define NETD_OP_CONFIG_GET 3
#define NETD_OP_CONFIG_SET 4

struct __attribute__((packed)) netd_request {
    uint32_t opcode;
    uint32_t url_shm_id;
    uint32_t body_shm_id;
    uint32_t cap;
    int32_t  a[4];
};

struct __attribute__((packed)) netd_reply {
    int32_t  result;
    int32_t  a[2];
    uint8_t  mac[6];
    uint32_t our_ip, netmask, gateway, dns_server;
};
