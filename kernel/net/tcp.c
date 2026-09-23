#include "tcp.h"
#include "net.h"
#include "ip.h"
#include "eth.h"
#include "drivers/net/rtl8139.h"
#include "system/timer/pit.h"
#include "lib/string.h"
#include "system/devmode/devmode.h"

#define TCP_MAX_SEGMENT_DATA 1460u

typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_SYN_SENT,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_REMOTE_CLOSED,
} tcp_conn_state_t;

typedef struct {
    tcp_conn_state_t state;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t snd_next;
    uint32_t rcv_next;

    uint8_t  stage_buf[TCP_MAX_SEGMENT_DATA];
    uint16_t stage_len;
    int      has_data;
} tcp_conn_t;

static tcp_conn_t g_conn;
static uint16_t g_next_local_port = 49152;

// Собирает псевдо-заголовок (12 байт, только для чексума) + настоящий TCP-
// заголовок (+данные) в одном скретч-буфере, считает чексум по ВСЕМУ этому
// буферу, затем отправляет через ip_send() только настоящий TCP-сегмент
// (без псевдо-заголовка — тот существует исключительно для арифметики
// чексума, на провод никогда не попадает).
static int tcp_send_segment(uint8_t flags, const void *data, uint16_t data_len) {
    uint8_t buf[12 + 20 + TCP_MAX_SEGMENT_DATA];

    uint32_t src_ip_n = htonl(net_get_config()->our_ip);
    uint32_t dst_ip_n = htonl(g_conn.remote_ip);
    memcpy(buf, &src_ip_n, 4);
    memcpy(buf + 4, &dst_ip_n, 4);
    buf[8] = 0;
    buf[9] = IP_PROTO_TCP;
    uint16_t tcp_len = (uint16_t)(20 + data_len);
    uint16_t tcp_len_n = htons(tcp_len);
    memcpy(buf + 10, &tcp_len_n, 2);

    tcp_header_t *hdr = (tcp_header_t *)(buf + 12);
    hdr->src_port = htons(g_conn.local_port);
    hdr->dst_port = htons(g_conn.remote_port);
    hdr->seq = htonl(g_conn.snd_next);
    hdr->ack = htonl((flags & TCP_FLAG_ACK) ? g_conn.rcv_next : 0);
    hdr->data_offset_reserved = (uint8_t)(5u << 4);
    hdr->flags = flags;
    hdr->window = htons(4096);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data_len > 0 && data) memcpy(buf + 12 + 20, data, data_len);

    hdr->checksum = htons(net_checksum(buf, (uint32_t)(12 + tcp_len)));

    return ip_send(g_conn.remote_ip, IP_PROTO_TCP, hdr, tcp_len);
}

int tcp_connect(uint32_t remote_ip, uint16_t remote_port) {
    if (!rtl8139_found()) return -1;

    g_conn.state = TCP_STATE_CLOSED;
    g_conn.remote_ip = remote_ip;
    g_conn.remote_port = remote_port;
    g_conn.local_port = g_next_local_port++;
    if (g_next_local_port == 0) g_next_local_port = 49152;
    // Произвольный ISN — единственное соединение зараз, конфликтов не бывает.
    g_conn.snd_next = (uint32_t)(pit_get_ticks() * 65537u + 12345u);
    g_conn.rcv_next = 0;
    g_conn.has_data = 0;
    g_conn.stage_len = 0;

    if (tcp_send_segment(TCP_FLAG_SYN, NULL, 0) != 0) return -1;
    g_conn.state = TCP_STATE_SYN_SENT;
    g_conn.snd_next++; // SYN занимает один номер последовательности

    uint64_t deadline = pit_get_ticks() + 300; // ~3000мс
    while (pit_get_ticks() < deadline) {
        rtl8139_poll();
        if (g_conn.state == TCP_STATE_ESTABLISHED) return 0;
        if (g_conn.state == TCP_STATE_CLOSED) return -1; // RST
        pit_wait_ms(1);
    }

    g_conn.state = TCP_STATE_CLOSED;
    return -1;
}

int tcp_send(const void *data, uint16_t len) {
    if (g_conn.state != TCP_STATE_ESTABLISHED) return -1;

    const uint8_t *p = (const uint8_t *)data;
    uint16_t remaining = len;
    while (remaining > 0) {
        uint16_t chunk = remaining > TCP_MAX_SEGMENT_DATA ? (uint16_t)TCP_MAX_SEGMENT_DATA : remaining;
        if (tcp_send_segment(TCP_FLAG_ACK | TCP_FLAG_PSH, p, chunk) != 0) return -1;
        g_conn.snd_next += chunk;
        p += chunk;
        remaining = (uint16_t)(remaining - chunk);
    }
    return 0;
}

int tcp_recv_poll(uint8_t **out_ptr, uint16_t *out_len) {
    rtl8139_poll();
    if (g_conn.has_data) {
        if (out_ptr) *out_ptr = g_conn.stage_buf;
        if (out_len) *out_len = g_conn.stage_len;
        return 1;
    }
    return 0;
}

void tcp_ack_consumed(void) {
    g_conn.has_data = 0;
    tcp_send_segment(TCP_FLAG_ACK, NULL, 0);
}

int tcp_is_remote_closed(void) {
    return g_conn.state == TCP_STATE_REMOTE_CLOSED;
}

void tcp_close(void) {
    if (g_conn.state == TCP_STATE_ESTABLISHED || g_conn.state == TCP_STATE_REMOTE_CLOSED) {
        tcp_send_segment(TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        g_conn.snd_next++;
    }

    uint64_t deadline = pit_get_ticks() + 100; // best-effort ~1000мс
    while (pit_get_ticks() < deadline && g_conn.state != TCP_STATE_CLOSED) {
        rtl8139_poll();
        pit_wait_ms(1);
    }

    g_conn.state = TCP_STATE_CLOSED;
}

void tcp_receive(uint32_t src_ip, const void *payload, uint16_t len) {
    if (len < 20) return;
    if (g_conn.state == TCP_STATE_CLOSED) return;
    if (src_ip != g_conn.remote_ip) return;

    const tcp_header_t *hdr = (const tcp_header_t *)payload;
    if (ntohs(hdr->src_port) != g_conn.remote_port) return;
    if (ntohs(hdr->dst_port) != g_conn.local_port) return;

    uint8_t flags = hdr->flags;

    if (flags & TCP_FLAG_RST) {
        g_conn.state = TCP_STATE_CLOSED;
        return;
    }

    uint8_t data_offset = (uint8_t)((hdr->data_offset_reserved >> 4) * 4);
    if (data_offset < 20 || data_offset > len) return;
    uint16_t data_len = (uint16_t)(len - data_offset);
    const uint8_t *data = (const uint8_t *)payload + data_offset;
    uint32_t seg_seq = ntohl(hdr->seq);

    if (g_conn.state == TCP_STATE_SYN_SENT) {
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && ntohl(hdr->ack) == g_conn.snd_next) {
            g_conn.rcv_next = seg_seq + 1;
            tcp_send_segment(TCP_FLAG_ACK, NULL, 0);
            g_conn.state = TCP_STATE_ESTABLISHED;
        }
        return;
    }

    // Данные принимаем, только если пришли строго по порядку И предыдущий
    // принятый кусок уже потреблён вызывающей стороной (иначе — молча
    // отбрасываем; документированное упрощение без буфера reassembly, см. план).
    if (data_len > 0 && seg_seq == g_conn.rcv_next && !g_conn.has_data) {
        uint16_t copy_len = data_len > sizeof(g_conn.stage_buf)
                                 ? (uint16_t)sizeof(g_conn.stage_buf)
                                 : data_len;
        memcpy(g_conn.stage_buf, data, copy_len);
        g_conn.stage_len = copy_len;
        g_conn.has_data = 1;
        g_conn.rcv_next += copy_len;
        // ACK на этот кусок шлёт tcp_ack_consumed() после того, как
        // вызывающая сторона его заберёт — не раньше, иначе при полном
        // stage_buf мы бы подтвердили байты, которые на самом деле обрезали.
    }

    if ((flags & TCP_FLAG_FIN) && (seg_seq + data_len == g_conn.rcv_next)) {
        g_conn.rcv_next++;
        tcp_send_segment(TCP_FLAG_ACK, NULL, 0);
        g_conn.state = TCP_STATE_REMOTE_CLOSED;
    }
}
