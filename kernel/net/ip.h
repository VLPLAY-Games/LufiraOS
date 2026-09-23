#pragma once

#include "lib/types.h"

#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP  6u

#define IP_HEADER_LEN 20

typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;   // 4<<4 | 5 (без опций)
    uint8_t  tos;
    uint16_t total_length;  // network order, включая сам заголовок
    uint16_t id;            // network order
    uint16_t flags_frag;    // network order
    uint8_t  ttl;
    uint8_t  protocol;      // IP_PROTO_*
    uint16_t checksum;      // network order
    uint32_t src;           // network order
    uint32_t dst;           // network order
} ip_header_t;

// Определяет next-hop (тот же subnet -> dst_ip напрямую, иначе шлюз),
// резолвит его MAC через arp_resolve(), собирает заголовок и отправляет
// через eth_send(). 0 = успех, -1 = ошибка (в т.ч. ARP-таймаут).
int ip_send(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t len);

// Вызывается из eth_receive() на IP-кадры. Отбрасывает: неверную
// версию/IHL>5 (без опций)/битый чексум/фрагментированные пакеты (нет
// реассемблинга — см. план, TCP уже сегментирует данные сам) и всё
// адресованное не нам. Иначе диспетчеризует по protocol в
// icmp_receive()/tcp_receive().
void ip_receive(const uint8_t *frame, uint16_t len);
