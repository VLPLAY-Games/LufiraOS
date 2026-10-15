// dhcp.c — минимальный DHCPv4-клиент (RFC 2131/2132), см. dhcp.h.
#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "system/timer/pit.h"
#include "system/process/process.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"
#include "lib/string.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY   2
#define DHCP_HTYPE_ETHERNET 1
#define DHCP_HLEN_ETHERNET  6
#define DHCP_MAGIC_COOKIE   0x63825363u
#define DHCP_FLAG_BROADCAST 0x8000u // просим сервер отвечать широковещательно —
                                     // своего IP у нас ещё нет (см. ip_receive())

#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS_SERVER     6
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_REQ_LIST 55
#define DHCP_OPT_END            255

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

#define DHCP_FIXED_LEN  236 // BOOTP-часть заголовка, до magic cookie (RFC 2131 §2)
#define DHCP_PACKET_MAX (DHCP_FIXED_LEN + 4 + 32) // + cookie + наши (короткие) опции

typedef struct {
    int      msg_type;    // -1, если опция 53 не нашлась
    uint32_t offered_ip;  // yiaddr
    uint32_t server_id;
    uint32_t subnet_mask;
    uint32_t router;
    uint32_t dns_server;
} dhcp_reply_info_t;

static int mac_equal(const uint8_t a[6], const uint8_t b[6]) {
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

// op/htype/hlen/hops/xid/flags/chaddr + magic cookie + опции (DISCOVER:
// только тип сообщения и param request list; REQUEST: ещё requested-IP и
// server-id). Возвращает итоговую длину пакета.
static int dhcp_build_packet(uint8_t *buf, uint32_t xid, uint8_t msg_type,
                              const uint8_t mac[6], uint32_t requested_ip, uint32_t server_id) {
    memset(buf, 0, DHCP_FIXED_LEN);
    buf[0] = DHCP_OP_BOOTREQUEST;
    buf[1] = DHCP_HTYPE_ETHERNET;
    buf[2] = DHCP_HLEN_ETHERNET;
    buf[3] = 0; // hops

    uint32_t xid_n = htonl(xid);
    memcpy(buf + 4, &xid_n, 4);

    uint16_t flags_n = htons(DHCP_FLAG_BROADCAST);
    memcpy(buf + 10, &flags_n, 2);
    // secs(2)=0, ciaddr/yiaddr/siaddr/giaddr(4*4)=0 — уже обнулены memset'ом

    memcpy(buf + 28, mac, 6); // chaddr — первые 6 из 16 байт, остаток паддинг нулями

    uint32_t cookie_n = htonl(DHCP_MAGIC_COOKIE);
    memcpy(buf + DHCP_FIXED_LEN, &cookie_n, 4);

    int pos = DHCP_FIXED_LEN + 4;
    buf[pos++] = DHCP_OPT_MSG_TYPE; buf[pos++] = 1; buf[pos++] = msg_type;

    if (msg_type == DHCP_MSG_REQUEST) {
        buf[pos++] = DHCP_OPT_REQUESTED_IP; buf[pos++] = 4;
        uint32_t ip_n = htonl(requested_ip);
        memcpy(buf + pos, &ip_n, 4); pos += 4;

        buf[pos++] = DHCP_OPT_SERVER_ID; buf[pos++] = 4;
        uint32_t sid_n = htonl(server_id);
        memcpy(buf + pos, &sid_n, 4); pos += 4;
    }

    buf[pos++] = DHCP_OPT_PARAM_REQ_LIST; buf[pos++] = 3;
    buf[pos++] = DHCP_OPT_SUBNET_MASK;
    buf[pos++] = DHCP_OPT_ROUTER;
    buf[pos++] = DHCP_OPT_DNS_SERVER;

    buf[pos++] = DHCP_OPT_END;
    return pos;
}

// Проверяет op/xid/chaddr/magic cookie, разбирает опции. 0 = это наш ответ
// (out->msg_type валиден), -1 = не наш пакет/битый/нет опции 53.
static int dhcp_parse_reply(const uint8_t *data, uint16_t len, uint32_t xid,
                             const uint8_t mac[6], dhcp_reply_info_t *out) {
    if (len < DHCP_FIXED_LEN + 4) return -1;
    if (data[0] != DHCP_OP_BOOTREPLY) return -1;

    uint32_t got_xid;
    memcpy(&got_xid, data + 4, 4);
    if (ntohl(got_xid) != xid) return -1;

    if (!mac_equal(data + 28, mac)) return -1; // чужой ответ на том же сегменте

    uint32_t cookie;
    memcpy(&cookie, data + DHCP_FIXED_LEN, 4);
    if (ntohl(cookie) != DHCP_MAGIC_COOKIE) return -1;

    uint32_t yiaddr_n;
    memcpy(&yiaddr_n, data + 16, 4);

    out->msg_type = -1;
    out->offered_ip = ntohl(yiaddr_n);
    out->server_id = 0;
    out->subnet_mask = 0;
    out->router = 0;
    out->dns_server = 0;

    int pos = DHCP_FIXED_LEN + 4;
    while (pos < len) {
        uint8_t opt = data[pos++];
        if (opt == DHCP_OPT_END) break;
        if (opt == 0) continue; // PAD
        if (pos >= len) break;
        uint8_t opt_len = data[pos++];
        if (pos + opt_len > len) break;

        switch (opt) {
            case DHCP_OPT_MSG_TYPE:
                if (opt_len >= 1) out->msg_type = data[pos];
                break;
            case DHCP_OPT_SERVER_ID: {
                uint32_t v;
                if (opt_len >= 4) { memcpy(&v, data + pos, 4); out->server_id = ntohl(v); }
                break;
            }
            case DHCP_OPT_SUBNET_MASK: {
                uint32_t v;
                if (opt_len >= 4) { memcpy(&v, data + pos, 4); out->subnet_mask = ntohl(v); }
                break;
            }
            case DHCP_OPT_ROUTER: {
                uint32_t v;
                if (opt_len >= 4) { memcpy(&v, data + pos, 4); out->router = ntohl(v); }
                break;
            }
            case DHCP_OPT_DNS_SERVER: {
                uint32_t v;
                if (opt_len >= 4) { memcpy(&v, data + pos, 4); out->dns_server = ntohl(v); }
                break;
            }
            default: break;
        }
        pos += opt_len;
    }

    return (out->msg_type >= 0) ? 0 : -1;
}

// Одна фаза DISCOVER->OFFER или REQUEST->ACK/NAK: до 3 широковещательных
// попыток, каждая со своим ~3000мс бюджетом (тот же порядок, что у
// dns_resolve()/tcp_connect()), всё вместе не длиннее overall_deadline.
// *inout_reply на входе для REQUEST несёт offered_ip/server_id из OFFER;
// на успешном выходе — разобранный ответ (OFFER или ACK).
static int dhcp_exchange(uint8_t msg_type, uint32_t xid, const uint8_t mac[6],
                          uint64_t overall_deadline, dhcp_reply_info_t *inout_reply,
                          int expect_msg_type, int *out_got_nak) {
    uint8_t pkt[DHCP_PACKET_MAX];

    for (int attempt = 0; attempt < 3 && pit_get_ticks() < overall_deadline; attempt++) {
        int plen = dhcp_build_packet(pkt, xid, msg_type, mac,
                                      inout_reply->offered_ip, inout_reply->server_id);
        if (udp_send(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, (uint16_t)plen) != 0) {
            return -1;
        }

        uint64_t deadline = pit_get_ticks() + 300; // ~3000мс на попытку
        if (deadline > overall_deadline) deadline = overall_deadline;

        while (pit_get_ticks() < deadline) {
            uint32_t src_ip; uint16_t src_port; uint8_t *data; uint16_t len;
            if (udp_recv_poll(&src_ip, &src_port, &data, &len) && src_port == DHCP_SERVER_PORT) {
                dhcp_reply_info_t reply;
                if (dhcp_parse_reply(data, len, xid, mac, &reply) == 0) {
                    if (reply.msg_type == expect_msg_type) {
                        *inout_reply = reply;
                        return 0;
                    }
                    if (reply.msg_type == DHCP_MSG_NAK) {
                        if (out_got_nak) *out_got_nak = 1;
                        return -1;
                    }
                    // Чужой/поздний ответ на предыдущую попытку — продолжаем ждать.
                }
            }
            process_sleep(1); // НАЙДЕННЫЙ БАГ — см. net_lock_acquire() в net.h
        }
    }
    return -1;
}

int dhcp_start(uint32_t timeout_ms) {
    if (!net_found()) return -1;

    uint8_t mac[6];
    memcpy(mac, net_get_mac(), 6);

    // Не криптографически случайный (и не обязан быть) — опознаём СВОИ
    // ответы по паре (xid, chaddr) всё равно, см. dhcp_parse_reply().
    uint32_t xid = (uint32_t)(pit_get_ticks() * 2654435761u) + 0x9E3779B9u;

    udp_listen(DHCP_CLIENT_PORT);
    uint64_t overall_deadline = pit_get_ticks() + (timeout_ms + 9) / 10;

    dhcp_reply_info_t reply;
    memset(&reply, 0, sizeof(reply));

    if (dhcp_exchange(DHCP_MSG_DISCOVER, xid, mac, overall_deadline, &reply, DHCP_MSG_OFFER, NULL) != 0) {
        DLOG("[DHCP] no OFFER received\n");
        udp_stop_listen();
        return -1;
    }

    int got_nak = 0;
    int ack_ok = dhcp_exchange(DHCP_MSG_REQUEST, xid, mac, overall_deadline, &reply, DHCP_MSG_ACK, &got_nak) == 0;

    udp_stop_listen();

    if (!ack_ok) {
        DLOG(got_nak ? "[DHCP] server sent NAK\n" : "[DHCP] no ACK received\n");
        return -1;
    }

    const net_config_t *old = net_get_config();
    net_set_config(reply.offered_ip,
                   reply.subnet_mask ? reply.subnet_mask : old->netmask,
                   reply.router ? reply.router : old->gateway,
                   reply.dns_server ? reply.dns_server : old->dns_server);

    char ip_str[16];
    ip_addr_to_str(reply.offered_ip, ip_str);
    klog("[DHCP] lease acquired: %s", ip_str);
    DLOG("[DHCP] lease acquired: %s\n", ip_str);
    return 0;
}
