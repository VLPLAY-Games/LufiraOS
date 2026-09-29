#include "tcp.h"
#include "net.h"
#include "ip.h"
#include "eth.h"
#include "drivers/net/rtl8139.h"
#include "system/timer/pit.h"
#include "lib/string.h"
#include "system/devmode/devmode.h"

#define TCP_MAX_SEGMENT_DATA 1460u

// Ретрансмиссия: один незаконченный ("in-flight") сегмент за раз —
// stop-and-wait ARQ, а не настоящее скользящее окно (congestion
// control/несколько сегментов в полёте одновременно осознанно вне
// области, см. план). RTO фиксированный (без адаптивной оценки RTT/Карна),
// этого достаточно, чтобы не терять пакеты молча на practически-надёжном
// QEMU SLIRP-линке — не имитирует полноценный TCP.
#define TCP_RTO_TICKS   30   // ~300мс на попытку
#define TCP_MAX_RETRIES 5

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

    // Копия последнего отправленного (SYN/данные/FIN) сегмента — на случай,
    // если peer_acked_seq не догонит его вовремя (см. tcp_retransmit_if_needed()).
    uint8_t  last_segment_buf[TCP_MAX_SEGMENT_DATA];
    uint16_t last_segment_len;
    uint8_t  last_segment_flags;
    uint32_t last_segment_seq;      // seq, с которым сегмент был отправлен
    uint32_t last_segment_end_seq;  // seq, начиная с которого сегмент считается ПОДТВЕРЖДЁННЫМ
    uint64_t last_send_tick;
    int      retry_count;
    uint32_t peer_acked_seq;        // максимальный увиденный ACK от собеседника (кумулятивный)
} tcp_conn_t;

static tcp_conn_t g_conn;
static uint16_t g_next_local_port = 49152;

// Собирает псевдо-заголовок (12 байт, только для чексума) + настоящий TCP-
// заголовок (+данные) в одном скретч-буфере, считает чексум по ВСЕМУ этому
// буферу, затем отправляет через ip_send() только настоящий TCP-сегмент
// (без псевдо-заголовка — тот существует исключительно для арифметики
// чексума, на провод никогда не попадает). Явный seq (а не всегда
// g_conn.snd_next) — ретрансмиссии повторно шлют СТАРЫЙ, уже израсходованный
// seq, не текущий.
static int tcp_send_segment_seq(uint8_t flags, const void *data, uint16_t data_len, uint32_t seq) {
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
    hdr->seq = htonl(seq);
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

// Обычная отправка (не ретрансмиссия): использует ТЕКУЩИЙ g_conn.snd_next и
// запоминает сегмент как "в полёте" для возможной ретрансмиссии —
// last_send_tick/retry_count сбрасываются, peer_acked_seq НЕ трогается (это
// состояние собеседника, а не наше). Вызывающий сам продвигает snd_next
// после успешного вызова (SYN/FIN — на 1, данные — на data_len), как и
// раньше.
static int tcp_send_segment(uint8_t flags, const void *data, uint16_t data_len) {
    uint32_t seq = g_conn.snd_next;
    if (tcp_send_segment_seq(flags, data, data_len, seq) != 0) return -1;

    g_conn.last_segment_flags = flags;
    g_conn.last_segment_len = data_len;
    if (data_len > 0 && data) memcpy(g_conn.last_segment_buf, data, data_len);
    g_conn.last_segment_seq = seq;
    uint32_t consumed = data_len;
    if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) consumed += 1;
    g_conn.last_segment_end_seq = seq + consumed;
    g_conn.last_send_tick = pit_get_ticks();
    g_conn.retry_count = 0;

    return 0;
}

// Резонно вызывать на каждой итерации любого поллинг-цикла, ждущего ACK
// (tcp_connect()/tcp_send()/tcp_close()) — если последний отправленный
// сегмент ещё не подтверждён (peer_acked_seq < last_segment_end_seq) и с
// последней отправки прошло больше TCP_RTO_TICKS, шлёт его ЕЩЁ РАЗ с тем же
// seq. После TCP_MAX_RETRIES неудачных попыток сдаётся и переводит
// соединение в CLOSED — вызывающий цикл при этом сам выходит по своей же
// проверке состояния/дедлайна, как и раньше при реальном таймауте.
static void tcp_retransmit_if_needed(void) {
    if (g_conn.state == TCP_STATE_CLOSED) return;
    if ((int32_t)(g_conn.peer_acked_seq - g_conn.last_segment_end_seq) >= 0) return; // уже подтверждено

    if (pit_get_ticks() - g_conn.last_send_tick < TCP_RTO_TICKS) return;

    if (g_conn.retry_count >= TCP_MAX_RETRIES) {
        g_conn.state = TCP_STATE_CLOSED;
        return;
    }

    g_conn.retry_count++;
    g_conn.last_send_tick = pit_get_ticks();
    tcp_send_segment_seq(g_conn.last_segment_flags, g_conn.last_segment_buf,
                          g_conn.last_segment_len, g_conn.last_segment_seq);
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
    g_conn.peer_acked_seq = g_conn.snd_next; // ничего своего пока не отправлено

    if (tcp_send_segment(TCP_FLAG_SYN, NULL, 0) != 0) return -1;
    g_conn.state = TCP_STATE_SYN_SENT;
    g_conn.snd_next++; // SYN занимает один номер последовательности

    uint64_t deadline = pit_get_ticks() + 300; // ~3000мс общий бюджет, включая ретрансмиссии SYN
    while (pit_get_ticks() < deadline) {
        rtl8139_poll();
        tcp_retransmit_if_needed();
        if (g_conn.state == TCP_STATE_ESTABLISHED) return 0;
        if (g_conn.state == TCP_STATE_CLOSED) return -1; // RST или исчерпаны ретраи
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
        uint32_t end_seq = g_conn.last_segment_end_seq;
        g_conn.snd_next += chunk;

        // stop-and-wait: не отправляем следующий кусок, пока этот не
        // подтверждён (или окончательно не сдались) — см. tcp_retransmit_if_needed().
        uint64_t deadline = pit_get_ticks() + 300; // ~3000мс бюджет на ЭТОТ сегмент
        while ((int32_t)(g_conn.peer_acked_seq - end_seq) < 0 && pit_get_ticks() < deadline) {
            rtl8139_poll();
            tcp_retransmit_if_needed();
            if (g_conn.state == TCP_STATE_CLOSED) return -1;
            pit_wait_ms(1);
        }
        if ((int32_t)(g_conn.peer_acked_seq - end_seq) < 0) {
            g_conn.state = TCP_STATE_CLOSED;
            return -1; // так и не подтвердили после всех ретраев
        }

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

// ACK-only (пустой) — реактивный ответ, а не полезная нагрузка, которую
// нужно ретранслировать при потере (если он потеряется, следующий же ACK
// или ретрансмиссия ПРОТИВОПОЛОЖНОЙ стороны всё равно всё поправит,
// стандартное свойство кумулятивных ACK в TCP) — намеренно
// tcp_send_segment_seq(), а НЕ tcp_send_segment(): последняя перезаписала
// бы last_segment_*/retry_count слежение за настоящим in-flight сегментом
// данных, если он в этот момент как раз ждёт подтверждения.
static void tcp_send_ack_only(void) {
    tcp_send_segment_seq(TCP_FLAG_ACK, NULL, 0, g_conn.snd_next);
}

void tcp_ack_consumed(void) {
    g_conn.has_data = 0;
    tcp_send_ack_only();
}

int tcp_is_remote_closed(void) {
    return g_conn.state == TCP_STATE_REMOTE_CLOSED;
}

void tcp_close(void) {
    if (g_conn.state == TCP_STATE_ESTABLISHED || g_conn.state == TCP_STATE_REMOTE_CLOSED) {
        tcp_send_segment(TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        g_conn.snd_next++;
    }

    uint64_t deadline = pit_get_ticks() + 100; // best-effort ~1000мс, включая ретрансмиссии FIN
    while (pit_get_ticks() < deadline && g_conn.state != TCP_STATE_CLOSED) {
        rtl8139_poll();
        tcp_retransmit_if_needed();
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

    // Кумулятивный ACK от собеседника — только НАПЕРЁД (signed-diff, а не
    // прямое сравнение: seq/ack — 32-битные и оборачиваются, см. RFC 1982;
    // при обычной длине соединений этой ОС переполнение практически не
    // случается, но сравнение всё равно должно быть корректным). Именно
    // это поле tcp_retransmit_if_needed() использует, чтобы понять, нужно
    // ли повторно слать последний сегмент.
    if (flags & TCP_FLAG_ACK) {
        uint32_t incoming_ack = ntohl(hdr->ack);
        if ((int32_t)(incoming_ack - g_conn.peer_acked_seq) > 0) {
            g_conn.peer_acked_seq = incoming_ack;
        }
    }

    if (g_conn.state == TCP_STATE_SYN_SENT) {
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && ntohl(hdr->ack) == g_conn.snd_next) {
            g_conn.rcv_next = seg_seq + 1;
            tcp_send_ack_only();
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
        tcp_send_ack_only();
        g_conn.state = TCP_STATE_REMOTE_CLOSED;
    }
}
