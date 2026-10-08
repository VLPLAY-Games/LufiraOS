#include "udp.h"
#include "net.h"
#include "ip.h"
#include "eth.h"
#include "drivers/net/rtl8139.h"
#include "lib/string.h"

// DNS-запросы/ответы (единственный сегодняшний потребитель, см. dns.c) —
// маленькие, UDP_MAX_PAYLOAD даёт большой запас над типичными ~50-300
// байтами, не подбирая его впритык под один конкретный протокол.
#define UDP_MAX_PAYLOAD (ETH_MTU_PAYLOAD - IP_HEADER_LEN - UDP_HEADER_LEN)

typedef struct {
    int listening;
    uint16_t local_port;
    int has_data;
    uint32_t src_ip;
    uint16_t src_port;
    uint8_t buf[UDP_MAX_PAYLOAD];
    uint16_t len;
} udp_state_t;

static udp_state_t g_udp;

int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *data, uint16_t len) {
    if (len > UDP_MAX_PAYLOAD) return -1;

    // Тот же приём псевдо-заголовка, что и tcp_send_segment_seq() (tcp.c):
    // псевдо-заголовок участвует только в арифметике чексума, на провод
    // уходит лишь настоящий UDP-заголовок+данные через ip_send() ниже.
    uint8_t buf[12 + UDP_HEADER_LEN + UDP_MAX_PAYLOAD];

    uint32_t src_ip_n = htonl(net_get_config()->our_ip);
    uint32_t dst_ip_n = htonl(dst_ip);
    memcpy(buf, &src_ip_n, 4);
    memcpy(buf + 4, &dst_ip_n, 4);
    buf[8] = 0;
    buf[9] = IP_PROTO_UDP;
    uint16_t udp_len = (uint16_t)(UDP_HEADER_LEN + len);
    uint16_t udp_len_n = htons(udp_len);
    memcpy(buf + 10, &udp_len_n, 2);

    udp_header_t *hdr = (udp_header_t *)(buf + 12);
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons(udp_len);
    hdr->checksum = 0;

    if (len > 0 && data) memcpy(buf + 12 + UDP_HEADER_LEN, data, len);

    uint16_t csum = net_checksum(buf, (uint32_t)(12 + udp_len));
    // RFC 768: вычисленный чексум 0x0000 передаётся как 0xFFFF (поле 0x0000
    // зарезервировано под "чексум не считался" — редкий, но законный случай
    // одно-дополнительной суммы, лучше обработать явно, чем отправить
    // датаграм, который примут за непроверенный).
    hdr->checksum = htons(csum == 0 ? 0xFFFFu : csum);

    return ip_send(dst_ip, IP_PROTO_UDP, hdr, udp_len);
}

void udp_listen(uint16_t local_port) {
    g_udp.listening = 1;
    g_udp.local_port = local_port;
    g_udp.has_data = 0;
}

void udp_stop_listen(void) {
    g_udp.listening = 0;
    g_udp.has_data = 0;
}

int udp_recv_poll(uint32_t *out_src_ip, uint16_t *out_src_port, uint8_t **out_ptr, uint16_t *out_len) {
    rtl8139_poll();
    if (g_udp.has_data) {
        if (out_src_ip) *out_src_ip = g_udp.src_ip;
        if (out_src_port) *out_src_port = g_udp.src_port;
        if (out_ptr) *out_ptr = g_udp.buf;
        if (out_len) *out_len = g_udp.len;
        // Один датаграм за раз (как tcp.c's stage_buf) — потребляем сразу,
        // следующий udp_receive() уже может писать поверх.
        g_udp.has_data = 0;
        return 1;
    }
    return 0;
}

void udp_receive(uint32_t src_ip, const void *payload, uint16_t len) {
    if (len < UDP_HEADER_LEN) return;
    if (!g_udp.listening) return;

    const udp_header_t *hdr = (const udp_header_t *)payload;
    if (ntohs(hdr->dst_port) != g_udp.local_port) return;

    uint16_t data_len = (uint16_t)(len - UDP_HEADER_LEN);
    if (data_len > sizeof(g_udp.buf)) data_len = (uint16_t)sizeof(g_udp.buf);
    memcpy(g_udp.buf, (const uint8_t *)payload + UDP_HEADER_LEN, data_len);
    g_udp.len = data_len;
    g_udp.src_ip = src_ip;
    g_udp.src_port = ntohs(hdr->src_port);
    g_udp.has_data = 1;
}
