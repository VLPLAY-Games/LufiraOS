#include "dns.h"
#include "udp.h"
#include "net.h"
#include "drivers/net/rtl8139.h"
#include "system/timer/pit.h"
#include "lib/string.h"

#define DNS_PORT                53
#define DNS_QUERY_BUDGET_TICKS  300 // ~3000мс - тот же бюджет, что tcp_connect()/icmp_ping_wait()

static uint16_t g_next_dns_txn_id = 1;

// Пишет QNAME ("example.com" -> 0x07'e''x''a''m''p''l''e'0x03'c''o''m'0x00)
// начиная с buf[pos]. Возвращает pos ПОСЛЕ терминатора, или -1 (пустой/
// слишком длинный label, или имя не влезло в буфер).
static int dns_write_qname(uint8_t *buf, int pos, int buf_cap, const char *hostname) {
    const char *label_start = hostname;
    for (;;) {
        const char *p = label_start;
        int label_len = 0;
        while (*p && *p != '.') { p++; label_len++; }
        if (label_len == 0 || label_len > 63) return -1;
        if (pos + 1 + label_len + 1 > buf_cap) return -1; // +1 терминатор на всякий случай
        buf[pos++] = (uint8_t)label_len;
        memcpy(buf + pos, label_start, (size_t)label_len);
        pos += label_len;
        if (*p == '\0') break;
        label_start = p + 1;
    }
    buf[pos++] = 0;
    return pos;
}

// Пропускает закодированное DNS-имя с buf[pos] (обычные labels,
// опционально завершённые compression-указателем, см. RFC 1035 §4.1.4) —
// само имя не разворачивается, нужна только позиция СРАЗУ ПОСЛЕ него (both
// в вопросе, который мы сами же составили, и в ответах, где имя может быть
// сжато указателем на вопрос). Возвращает -1 при выходе за buf_len.
static int dns_skip_name(const uint8_t *buf, int pos, int buf_len) {
    while (pos < buf_len) {
        uint8_t b = buf[pos];
        if ((b & 0xC0) == 0xC0) {
            if (pos + 2 > buf_len) return -1;
            return pos + 2; // указатель - всегда ровно 2 байта, дальше его не разворачиваем
        }
        if (b == 0) return pos + 1; // корневой (пустой) label - конец имени
        if (pos + 1 + b > buf_len) return -1;
        pos += 1 + b;
    }
    return -1;
}

int dns_resolve(const char *hostname, uint32_t *out_ip) {
    if (!rtl8139_found()) return -1;
    if (!hostname || !hostname[0] || !out_ip) return -1;

    uint16_t txn_id = g_next_dns_txn_id++;
    if (g_next_dns_txn_id == 0) g_next_dns_txn_id = 1; // 0 легален как ID, но пусть будет предсказуемо ненулевым

    uint8_t query[12 + 256 + 4];
    query[0] = (uint8_t)(txn_id >> 8);
    query[1] = (uint8_t)txn_id;
    query[2] = 0x01; query[3] = 0x00; // flags: QR=0,Opcode=0,RD=1
    query[4] = 0x00; query[5] = 0x01; // QDCOUNT=1
    query[6] = 0x00; query[7] = 0x00; // ANCOUNT=0
    query[8] = 0x00; query[9] = 0x00; // NSCOUNT=0
    query[10] = 0x00; query[11] = 0x00; // ARCOUNT=0

    int pos = dns_write_qname(query, 12, (int)sizeof(query) - 4, hostname);
    if (pos < 0) return -1;
    query[pos++] = 0x00; query[pos++] = 0x01; // QTYPE = A
    query[pos++] = 0x00; query[pos++] = 0x01; // QCLASS = IN

    uint16_t local_port = (uint16_t)(50000u + (txn_id % 10000u));
    udp_listen(local_port);

    uint32_t dns_ip = net_get_config()->dns_server;
    if (udp_send(dns_ip, local_port, DNS_PORT, query, (uint16_t)pos) != 0) {
        udp_stop_listen();
        return -1;
    }

    uint64_t deadline = pit_get_ticks() + DNS_QUERY_BUDGET_TICKS;
    while (pit_get_ticks() < deadline) {
        uint32_t src_ip; uint16_t src_port; uint8_t *data; uint16_t len;
        if (udp_recv_poll(&src_ip, &src_port, &data, &len) &&
            src_ip == dns_ip && src_port == DNS_PORT && len >= 12) {
            uint16_t resp_id = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
            int is_response = (data[2] & 0x80) != 0;

            if (resp_id == txn_id && is_response) {
                int rcode = data[3] & 0x0F;
                uint16_t qdcount = (uint16_t)(((uint16_t)data[4] << 8) | data[5]);
                uint16_t ancount = (uint16_t)(((uint16_t)data[6] << 8) | data[7]);

                if (rcode != 0 || ancount == 0) { udp_stop_listen(); return -1; } // NXDOMAIN/SERVFAIL/пусто

                int p = 12;
                for (uint16_t i = 0; i < qdcount; i++) {
                    p = dns_skip_name(data, p, len);
                    if (p < 0 || p + 4 > len) { udp_stop_listen(); return -1; }
                    p += 4; // QTYPE+QCLASS вопроса
                }

                for (uint16_t i = 0; i < ancount; i++) {
                    p = dns_skip_name(data, p, len);
                    if (p < 0 || p + 10 > len) { udp_stop_listen(); return -1; }
                    uint16_t rtype  = (uint16_t)(((uint16_t)data[p] << 8) | data[p + 1]);
                    uint16_t rclass = (uint16_t)(((uint16_t)data[p + 2] << 8) | data[p + 3]);
                    uint16_t rdlen  = (uint16_t)(((uint16_t)data[p + 8] << 8) | data[p + 9]);
                    p += 10;
                    if (p + rdlen > len) { udp_stop_listen(); return -1; }

                    if (rtype == 1 && rclass == 1 && rdlen == 4) { // A/IN
                        *out_ip = ((uint32_t)data[p] << 24) | ((uint32_t)data[p + 1] << 16) |
                                  ((uint32_t)data[p + 2] << 8) | (uint32_t)data[p + 3];
                        udp_stop_listen();
                        return 0;
                    }
                    p += rdlen; // не A (напр. CNAME) - пропускаем, смотрим следующую запись
                }
                // Ответ пришёл, но без A-записи в ANCOUNT записях (напр.
                // голый CNAME без glue) - сдаёмся, как и при NXDOMAIN.
                udp_stop_listen();
                return -1;
            }
            // ID не совпал/не ответ - не наш пакет (вперемешку мог прийти
            // поздний ответ на предыдущий резолв) - продолжаем ждать.
        }
        pit_wait_ms(1);
    }

    udp_stop_listen();
    return -1;
}
