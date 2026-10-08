#pragma once

#include "lib/types.h"

// Минимальный DNS-резолвер (один запрос типа A за раз, блокирующий
// опрос — тот же стиль, что icmp_ping_wait()/tcp_connect(), см. их
// комментарии) поверх udp.c. Нужен пакетному менеджеру (dlpg sync/
// upgrade, http_client.c), чтобы ходить по имени хоста
// (raw.githubusercontent.com), а не только по IP, как раньше умел
// только wget (см. shell/commands/net.c).
//
// Резолвер берётся из net_get_config()->dns_server (по умолчанию —
// встроенный DNS-форвардер QEMU SLIRP). НЕ следует CNAME отдельным
// запросом и НЕ кеширует ответы — каждый вызов это свежий полный
// UDP-обмен; для редких (одно обращение на весь dlpg sync/upgrade)
// этого достаточно.

// Резолвит hostname в IPv4 (host-order). 0 = успех (*out_ip заполнен),
// -1 = таймаут/NXDOMAIN/ошибка сети/нет A-записи в ответе.
int dns_resolve(const char *hostname, uint32_t *out_ip);
