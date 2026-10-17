#pragma once

#include "lib/types.h"

// Минимальный блокирующий DNS-резолвер (один запрос A за раз, стиль
// опроса как у icmp_ping_wait()/tcp_connect()) поверх udp.c.
//
// Сервер берётся из net_get_config()->dns_server (по умолчанию —
// форвардер QEMU SLIRP). Не следует CNAME и не кеширует.

// Резолвит hostname в IPv4 (host-order). 0 = успех (*out_ip заполнен),
// -1 = таймаут/NXDOMAIN/ошибка сети/нет A-записи в ответе.
int dns_resolve(const char *hostname, uint32_t *out_ip);
