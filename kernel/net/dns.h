#pragma once

#include "lib/types.h"

// Минимальный блокирующий DNS-резолвер (один запрос A за раз, тот же
// стиль опроса, что icmp_ping_wait()/tcp_connect()) поверх udp.c. Нужен
// dlpg sync/upgrade, чтобы обращаться по имени хоста, а не только по IP.
//
// Сервер берётся из net_get_config()->dns_server (по умолчанию — DNS-
// форвардер QEMU SLIRP). Не следует CNAME и не кеширует — каждый вызов
// это отдельный UDP-обмен.

// Резолвит hostname в IPv4 (host-order). 0 = успех (*out_ip заполнен),
// -1 = таймаут/NXDOMAIN/ошибка сети/нет A-записи в ответе.
int dns_resolve(const char *hostname, uint32_t *out_ip);
