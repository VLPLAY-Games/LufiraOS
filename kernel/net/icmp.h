#pragma once

#include "lib/types.h"

#define ICMP_TYPE_ECHO_REPLY   0u
#define ICMP_TYPE_ECHO_REQUEST 8u

// Вызывается из ip_receive() на ICMP-пакеты: отвечает на Echo Request'ы,
// а Echo Reply сверяет с текущим "в полёте" пингом (см. icmp_ping_send()) —
// один пинг единовременно, как и у синхронного foreground-command ping.
void icmp_receive(uint32_t src_ip, const void *payload, uint16_t len);

// Отправляет Echo Request и взводит ожидание ответа на этот id/seq.
// 0 = отправлено, -1 = ошибка ip_send() (в т.ч. ARP-таймаут на маршруте).
int icmp_ping_send(uint32_t dst_ip, uint16_t id, uint16_t seq);

// Блокирующе (сама опрашивает карту + pit_wait_ms) ждёт Echo Reply на
// последний icmp_ping_send(), не дольше timeout_ms. 0 = получен, RTT (мс,
// точность 10мс — разрешение PIT) в *out_rtt_ms; -1 = таймаут.
int icmp_ping_wait(uint32_t timeout_ms, uint32_t *out_rtt_ms);
