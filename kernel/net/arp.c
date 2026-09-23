#include "arp.h"
#include "net.h"
#include "eth.h"
#include "drivers/net/rtl8139.h"
#include "system/timer/pit.h"
#include "lib/string.h"

#define ARP_CACHE_SIZE      16
#define ARP_HTYPE_ETHERNET  1u
#define ARP_PTYPE_IPV4      0x0800u
#define ARP_OP_REQUEST      1u
#define ARP_OP_REPLY        2u

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} arp_packet_t;

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int arp_next_slot = 0; // при переполнении кэша — round-robin замена

static const uint8_t ARP_BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static int arp_cache_lookup(uint32_t ip, uint8_t out_mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(out_mac, arp_cache[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

static void arp_cache_insert(uint32_t ip, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    int slot = arp_next_slot;
    arp_next_slot = (arp_next_slot + 1) % ARP_CACHE_SIZE;
    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].valid = 1;
}

static void ip_to_bytes(uint32_t ip, uint8_t out[4]) {
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >> 8);
    out[3] = (uint8_t)ip;
}

static void arp_send_request(uint32_t target_ip) {
    arp_packet_t pkt;
    pkt.htype = htons(ARP_HTYPE_ETHERNET);
    pkt.ptype = htons(ARP_PTYPE_IPV4);
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.oper = htons(ARP_OP_REQUEST);
    memcpy(pkt.sha, rtl8139_get_mac(), 6);
    ip_to_bytes(net_get_config()->our_ip, pkt.spa);
    memset(pkt.tha, 0, 6);
    ip_to_bytes(target_ip, pkt.tpa);

    eth_send(ARP_BCAST_MAC, ETHERTYPE_ARP, &pkt, sizeof(pkt));
}

int arp_resolve(uint32_t ip, uint8_t out_mac[6]) {
    if (!rtl8139_found()) return -1;
    if (arp_cache_lookup(ip, out_mac) == 0) return 0;

    arp_send_request(ip);

    // pit_wait_ms(1) всегда округляет вверх до целого тика PIT (10мс, см.
    // pit_wait_ms()) — считать в РЕАЛЬНЫХ тиках через pit_get_ticks(), а не
    // в "миллисекундах" через число итераций, иначе таймаут оказывается в
    // 10 раз длиннее задуманного (этот самый units-баг уже один раз
    // ловили при отладке USB Mass Storage — см. план).
    uint64_t deadline = pit_get_ticks() + 200; // ~2000мс
    while (pit_get_ticks() < deadline) {
        rtl8139_poll();
        if (arp_cache_lookup(ip, out_mac) == 0) return 0;
        pit_wait_ms(1);
    }
    return -1;
}

void arp_receive(const uint8_t *payload, uint16_t len) {
    if (len < sizeof(arp_packet_t)) return;
    const arp_packet_t *pkt = (const arp_packet_t *)payload;

    if (ntohs(pkt->htype) != ARP_HTYPE_ETHERNET || ntohs(pkt->ptype) != ARP_PTYPE_IPV4) return;

    uint32_t sender_ip = ((uint32_t)pkt->spa[0] << 24) | ((uint32_t)pkt->spa[1] << 16) |
                          ((uint32_t)pkt->spa[2] << 8) | pkt->spa[3];
    arp_cache_insert(sender_ip, pkt->sha);

    if (ntohs(pkt->oper) != ARP_OP_REQUEST) return;

    uint32_t target_ip = ((uint32_t)pkt->tpa[0] << 24) | ((uint32_t)pkt->tpa[1] << 16) |
                          ((uint32_t)pkt->tpa[2] << 8) | pkt->tpa[3];
    if (target_ip != net_get_config()->our_ip) return;

    arp_packet_t reply;
    reply.htype = htons(ARP_HTYPE_ETHERNET);
    reply.ptype = htons(ARP_PTYPE_IPV4);
    reply.hlen = 6;
    reply.plen = 4;
    reply.oper = htons(ARP_OP_REPLY);
    memcpy(reply.sha, rtl8139_get_mac(), 6);
    memcpy(reply.spa, pkt->tpa, 4);
    memcpy(reply.tha, pkt->sha, 6);
    memcpy(reply.tpa, pkt->spa, 4);
    eth_send(pkt->sha, ETHERTYPE_ARP, &reply, sizeof(reply));
}
