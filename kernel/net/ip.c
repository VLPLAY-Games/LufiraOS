#include "ip.h"
#include "net.h"
#include "eth.h"
#include "arp.h"
#include "icmp.h"
#include "tcp.h"
#include "udp.h"
#include "lib/string.h"
#include "system/devmode/devmode.h"

#define IP_FLAG_MF_MASK    0x2000u // network-order-decoded flags_frag
#define IP_FRAG_OFFSET_MASK 0x1FFFu

static uint16_t ip_id_counter = 1;

int ip_send(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t len) {
    if (len > (ETH_MTU_PAYLOAD - IP_HEADER_LEN)) return -1;

    const net_config_t *cfg = net_get_config();

    uint8_t next_hop_mac[6];
    if (dst_ip == 0xFFFFFFFFu) {
        // Ограниченный broadcast (RFC 919 §7) — только локальный сегмент,
        // не маршрутизируется. ARP для него не работает и не нужен — кадр
        // шлётся на Ethernet-broadcast напрямую. Потребитель — dhcp.c
        // (клиент ещё не знает ни своего IP, ни адреса сервера).
        memset(next_hop_mac, 0xFF, 6);
    } else {
        uint32_t next_hop = ((dst_ip ^ cfg->our_ip) & cfg->netmask) == 0 ? dst_ip : cfg->gateway;
        if (arp_resolve(next_hop, next_hop_mac) != 0) {
            DLOG("[IP] ARP resolve failed for next-hop\n");
            return -1;
        }
    }

    uint8_t packet[IP_HEADER_LEN + ETH_MTU_PAYLOAD];
    ip_header_t *hdr = (ip_header_t *)packet;
    hdr->version_ihl = (4u << 4) | 5u;
    hdr->tos = 0;
    hdr->total_length = htons((uint16_t)(IP_HEADER_LEN + len));
    hdr->id = htons(ip_id_counter++);
    hdr->flags_frag = 0;
    hdr->ttl = 64;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src = htonl(cfg->our_ip);
    hdr->dst = htonl(dst_ip);
    hdr->checksum = htons(net_checksum(hdr, IP_HEADER_LEN));

    memcpy(packet + IP_HEADER_LEN, payload, len);

    return eth_send(next_hop_mac, ETHERTYPE_IP, packet, (uint16_t)(IP_HEADER_LEN + len));
}

void ip_receive(const uint8_t *frame, uint16_t len) {
    if (len < IP_HEADER_LEN) return;

    const ip_header_t *hdr = (const ip_header_t *)frame;
    if ((hdr->version_ihl >> 4) != 4) return;
    if ((hdr->version_ihl & 0x0F) != 5) return; // IHL>5 = опции, не поддерживаем

    if (net_checksum(hdr, IP_HEADER_LEN) != 0) {
        DLOG("[IP] bad checksum\n");
        return;
    }

    uint16_t total_length = ntohs(hdr->total_length);
    if (total_length < IP_HEADER_LEN || total_length > len) return;

    uint16_t flags_frag = ntohs(hdr->flags_frag);
    if ((flags_frag & IP_FLAG_MF_MASK) || (flags_frag & IP_FRAG_OFFSET_MASK) != 0) {
        DLOG("[IP] fragmented packet dropped (unsupported)\n");
        return;
    }

    // Принимаем адресованное нам и ограниченный broadcast (255.255.255.255)
    // — нужен dhcp.c: до DHCPACK своего IP ещё нет, сервер отвечает на
    // broadcast (см. DHCP_FLAG_BROADCAST в dhcp.c). Остальным протоколам
    // безопасно: порт/контекст дальше сами проверяют icmp_receive()/
    // udp_receive(), широковещательный ICMP echo обрабатывается как
    // обычный (RFC 1122).
    uint32_t dst_ip = ntohl(hdr->dst);
    if (dst_ip != net_get_config()->our_ip && dst_ip != 0xFFFFFFFFu) return;

    uint32_t src_ip = ntohl(hdr->src);
    const uint8_t *payload = frame + IP_HEADER_LEN;
    uint16_t payload_len = (uint16_t)(total_length - IP_HEADER_LEN);

    if (hdr->protocol == IP_PROTO_ICMP) {
        icmp_receive(src_ip, payload, payload_len);
    } else if (hdr->protocol == IP_PROTO_TCP) {
        tcp_receive(src_ip, payload, payload_len);
    } else if (hdr->protocol == IP_PROTO_UDP) {
        // DNS-резолвер (dns.c) — единственный потребитель UDP в этом стеке.
        udp_receive(src_ip, payload, payload_len);
    }
    // остальные протоколы молча отбрасываем
}
