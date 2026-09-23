#include "eth.h"
#include "net.h"
#include "arp.h"
#include "ip.h"
#include "drivers/net/rtl8139.h"
#include "lib/string.h"

int eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len) {
    if (!rtl8139_found()) return -1;
    if (len > ETH_MTU_PAYLOAD) return -1;

    uint8_t frame[ETH_HEADER_LEN + ETH_MTU_PAYLOAD];
    eth_header_t *hdr = (eth_header_t *)frame;
    memcpy(hdr->dst, dst_mac, 6);
    memcpy(hdr->src, rtl8139_get_mac(), 6);
    hdr->ethertype = htons(ethertype);
    memcpy(frame + ETH_HEADER_LEN, payload, len);

    return rtl8139_send(frame, (uint16_t)(ETH_HEADER_LEN + len));
}

void eth_receive(const uint8_t *frame, uint16_t len) {
    if (len < ETH_HEADER_LEN) return;

    const eth_header_t *hdr = (const eth_header_t *)frame;
    uint16_t ethertype = ntohs(hdr->ethertype);
    const uint8_t *payload = frame + ETH_HEADER_LEN;
    uint16_t payload_len = (uint16_t)(len - ETH_HEADER_LEN);

    if (ethertype == ETHERTYPE_ARP) {
        arp_receive(payload, payload_len);
    } else if (ethertype == ETHERTYPE_IP) {
        ip_receive(payload, payload_len);
    }
    // остальные ethertype молча отбрасываем
}
