#include "icmp.h"
#include "net.h"
#include "ip.h"
#include "drivers/net/rtl8139.h"
#include "system/timer/pit.h"
#include "lib/string.h"

#define ICMP_PING_PAYLOAD_LEN 32

typedef struct {
    int      waiting;
    uint32_t dst_ip;
    uint16_t id;
    uint16_t seq;
    uint64_t send_tick;
    int      received;
    uint64_t recv_tick;
} icmp_ping_state_t;

static icmp_ping_state_t g_ping;

int icmp_ping_send(uint32_t dst_ip, uint16_t id, uint16_t seq) {
    uint8_t packet[8 + ICMP_PING_PAYLOAD_LEN];
    packet[0] = (uint8_t)ICMP_TYPE_ECHO_REQUEST;
    packet[1] = 0;
    packet[2] = 0;
    packet[3] = 0;
    packet[4] = (uint8_t)(id >> 8);
    packet[5] = (uint8_t)id;
    packet[6] = (uint8_t)(seq >> 8);
    packet[7] = (uint8_t)seq;
    for (int i = 0; i < ICMP_PING_PAYLOAD_LEN; i++) {
        packet[8 + i] = (uint8_t)('a' + (i % 23));
    }

    // ВАЖНО: без htons() — в отличие от ip.c/tcp.c, где чексум кладётся
    // присваиванием в uint16_t-поле структуры (и htons() там как раз даёт
    // корректный порядок байт в памяти), здесь чексум раскладывается по
    // байтам ВРУЧНУЮ через >>8/&0xFF. net_checksum() уже возвращает
    // значение в "старший байт первым" представлении (см. её комментарий в
    // net.h) — то есть именно в том порядке, который нужен для прямой
    // записи в packet[2]/packet[3]. Дополнительный htons() здесь СВОПАЛ БЫ
    // байты ДВАЖДЫ и давал неверный чексум — что и происходило до этого
    // исправления (подтверждено разбором живого pcap-дампа: ICMP-эхо уходил
    // с неверным чексумом, QEMU SLIRP его молча отбрасывал, ответа не было).
    uint16_t csum = net_checksum(packet, sizeof(packet));
    packet[2] = (uint8_t)(csum >> 8);
    packet[3] = (uint8_t)csum;

    g_ping.waiting = 1;
    g_ping.dst_ip = dst_ip;
    g_ping.id = id;
    g_ping.seq = seq;
    g_ping.send_tick = pit_get_ticks();
    g_ping.received = 0;

    return ip_send(dst_ip, IP_PROTO_ICMP, packet, sizeof(packet));
}

int icmp_ping_wait(uint32_t timeout_ms, uint32_t *out_rtt_ms) {
    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;

    while (pit_get_ticks() < deadline) {
        rtl8139_poll();
        if (g_ping.received) {
            uint64_t ticks = g_ping.recv_tick - g_ping.send_tick;
            if (out_rtt_ms) *out_rtt_ms = (uint32_t)(ticks * 10);
            g_ping.waiting = 0;
            return 0;
        }
        pit_wait_ms(1);
    }

    g_ping.waiting = 0;
    return -1;
}

void icmp_receive(uint32_t src_ip, const void *payload, uint16_t len) {
    if (len < 8) return;
    const uint8_t *p = (const uint8_t *)payload;
    uint8_t type = p[0];
    uint16_t id  = (uint16_t)(((uint16_t)p[4] << 8) | p[5]);
    uint16_t seq = (uint16_t)(((uint16_t)p[6] << 8) | p[7]);

    if (type == ICMP_TYPE_ECHO_REQUEST) {
        uint16_t data_len = (uint16_t)(len - 8);
        if (data_len > 256) data_len = 256;

        uint8_t reply[8 + 256];
        reply[0] = (uint8_t)ICMP_TYPE_ECHO_REPLY;
        reply[1] = 0;
        reply[2] = 0;
        reply[3] = 0;
        reply[4] = p[4];
        reply[5] = p[5];
        reply[6] = p[6];
        reply[7] = p[7];
        memcpy(reply + 8, p + 8, data_len);

        // Без htons() — см. комментарий у аналогичного места в icmp_ping_send().
        uint16_t csum = net_checksum(reply, (uint32_t)(8 + data_len));
        reply[2] = (uint8_t)(csum >> 8);
        reply[3] = (uint8_t)csum;

        ip_send(src_ip, IP_PROTO_ICMP, reply, (uint16_t)(8 + data_len));
    } else if (type == ICMP_TYPE_ECHO_REPLY) {
        if (g_ping.waiting && g_ping.dst_ip == src_ip && g_ping.id == id && g_ping.seq == seq) {
            g_ping.received = 1;
            g_ping.recv_tick = pit_get_ticks();
        }
    }
}
