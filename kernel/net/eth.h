#pragma once

#include "lib/types.h"

#define ETHERTYPE_IP  0x0800u
#define ETHERTYPE_ARP 0x0806u

#define ETH_HEADER_LEN   14
#define ETH_MTU_PAYLOAD  1500

typedef struct __attribute__((packed)) {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype; // network order
} eth_header_t;

// Собирает заголовок (src = MAC карты, см. rtl8139_get_mac()) и отправляет
// кадр целиком через rtl8139_send(). 0 = успех, -1 = ошибка/карта не
// найдена/payload больше ETH_MTU_PAYLOAD.
int eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len);

// Вызывается из rtl8139_poll() на каждый принятый кадр — разбирает
// ethertype и передаёт дальше в arp_receive()/ip_receive(); остальное
// молча отбрасывается.
void eth_receive(const uint8_t *frame, uint16_t len);
