#pragma once

#include "lib/types.h"

/*
 * Минимальный DHCPv4-клиент (RFC 2131/2132): DISCOVER -> OFFER -> REQUEST ->
 * ACK, широковещательно через udp.c/ip.c. Один блокирующий вызов с
 * бюджетом, без фонового renew/rebind и без DHCPDECLINE/DHCPRELEASE —
 * тот же уровень упрощения, что у dns_resolve()/arp_resolve(). При успехе
 * применяет IP/маску/шлюз/DNS через net_set_config(); при неудаче
 * net_config_t остаётся прежним.
 */

// 0 = аренда получена и применена, -1 = не удалось (нет карты, таймаут,
// NAK). Безопасно звать даже без активной карты.
int dhcp_start(uint32_t timeout_ms);
