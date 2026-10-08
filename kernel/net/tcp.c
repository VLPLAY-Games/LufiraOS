#include "tcp.h"
#include "net.h"
#include "ip.h"
#include "eth.h"
#include "drivers/net/rtl8139.h"
#include "system/timer/pit.h"
#include "lib/string.h"
#include "system/devmode/devmode.h"

#define TCP_MAX_SEGMENT_DATA 1460u

/*
 * НАЙДЕННЫЙ БАГ: приём терял все сегменты в пачке, кроме первого.
 *
 * Было: один буфер на один сегмент (stage_buf + has_data); tcp_receive()
 * отбрасывал новый сегмент, пока предыдущий не забран вызывающим, БЕЗ
 * продвижения rcv_next — тот терялся насовсем, до ретрансмиссии
 * собеседника. rtl8139_poll() вычерпывает из кольца до 8 кадров за вызов,
 * так что ЛЮБАЯ пачка сегментов (весь ответ длиннее MSS) теряла все
 * сегменты, кроме первого, и восстанавливалась только экспоненциальным
 * RTO собеседника (1.5с, 3.0с, 6.0с...) — живой pcap против
 * raw.githubusercontent.com подтвердил ровно это: первый сегмент
 * ACK'ался, остальные ждали ретрансмиссии. Budget ожидания одной записи у
 * tls.c/http_client.c — 300 тиков (3000мс); второй откат RTO в него уже
 * не укладывался, поэтому TLS обрывался сразу после Certificate, а HTTP
 * приносил обрезанное тело. Не ловилось раньше, потому что shell-ный
 * wget (shell/commands/net.c) имел щедрый 10с-без-данных бюджет и просто
 * выглядел медленным, а хостовые тесты (net/selftest/) подменяют tcp.c
 * сокетами, где терять нечему.
 *
 * Стало: настоящая очередь (rx_fifo) плюс честное окно в заголовке —
 * собеседник сам не шлёт больше, чем мы готовы принять. Семантика
 * tcp_recv_poll()/tcp_ack_consumed() не изменилась.
 *
 * Окно подобрано под кольцо приёма RTL8139 (8КБ): 4 сегмента (~6КБ)
 * помещаются и вычерпываются одним rtl8139_poll() (4 <= 8 кадров/тик);
 * больше — упрёмся в переполнение того же кольца этажом ниже.
 */
#define TCP_RX_FIFO_CAP   8192u
#define TCP_RX_WINDOW_MAX (4u * TCP_MAX_SEGMENT_DATA)

// Ретрансмиссия — один in-flight сегмент за раз (stop-and-wait ARQ, не
// скользящее окно/congestion control — осознанно вне области). RTO
// фиксированный, без адаптивной оценки RTT: достаточно для практически
// надёжного линка QEMU SLIRP, полноценный TCP не имитируем.
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

    // Очередь принятых ПО ПОРЯДКУ байт, ещё не забранных вызывающей
    // стороной. rx_handed — размер куска, отданного последним
    // tcp_recv_poll() и ожидающего tcp_ack_consumed(): пока он не ноль,
    // начало очереди не двигается, поэтому отданный указатель остаётся
    // действительным (дописывание новых сегментов идёт в хвост).
    uint8_t  rx_fifo[TCP_RX_FIFO_CAP];
    uint32_t rx_len;
    uint32_t rx_handed;

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

// Сколько байт мы готовы принять прямо сейчас — объявляется в каждом
// исходящем сегменте (в т.ч. ACK из tcp_ack_consumed(), так собеседник
// узнаёт об освободившемся месте). Ноль — законно: останавливает
// отправителя до разбора накопленного, вместо молчаливой потери.
static uint16_t tcp_rx_window(void) {
    uint32_t free_space = TCP_RX_FIFO_CAP - g_conn.rx_len;
    if (free_space > TCP_RX_WINDOW_MAX) free_space = TCP_RX_WINDOW_MAX;
    return (uint16_t)free_space;
}

// Собирает псевдо-заголовок (12 байт, только для чексума) + настоящий
// TCP-заголовок(+данные) в одном буфере, считает чексум по всему этому,
// но на провод через ip_send() уходит только настоящий сегмент.
// Явный seq (не всегда g_conn.snd_next): ретрансмиссии повторно шлют
// старый, уже израсходованный seq.
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
    hdr->window = htons(tcp_rx_window());
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data_len > 0 && data) memcpy(buf + 12 + 20, data, data_len);

    hdr->checksum = htons(net_checksum(buf, (uint32_t)(12 + tcp_len)));

    return ip_send(g_conn.remote_ip, IP_PROTO_TCP, hdr, tcp_len);
}

// Обычная отправка (не ретрансмиссия): шлёт с текущим snd_next и
// запоминает сегмент "в полёте" для ретрансмиссии (last_send_tick/
// retry_count); peer_acked_seq не трогает. Вызывающий сам продвигает
// snd_next после успеха (SYN/FIN — на 1, данные — на data_len).
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

// Вызывать на каждой итерации поллинг-цикла, ждущего ACK
// (tcp_connect()/tcp_send()/tcp_close()): если последний сегмент не
// подтверждён и прошло больше TCP_RTO_TICKS — шлёт его снова тем же seq.
// После TCP_MAX_RETRIES сдаётся и переводит соединение в CLOSED.
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
    g_conn.rx_len = 0;
    g_conn.rx_handed = 0;
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

    // Повторный вызов до tcp_ack_consumed() обязан отдать тот же кусок:
    // вызывающий мог уже скопировать прошлую длину, "выросшая" дала бы
    // ему дыру в потоке.
    if (g_conn.rx_handed == 0) {
        uint32_t take = g_conn.rx_len;
        if (take > TCP_MAX_SEGMENT_DATA) take = TCP_MAX_SEGMENT_DATA;
        g_conn.rx_handed = take;
    }
    if (g_conn.rx_handed == 0) return 0;

    if (out_ptr) *out_ptr = g_conn.rx_fifo;
    if (out_len) *out_len = (uint16_t)g_conn.rx_handed;
    return 1;
}

// ACK-only: реактивный ответ, не payload, которую нужно ретранслировать
// при потере (следующий ACK/ретрансмиссия другой стороны всё поправит —
// обычное свойство кумулятивных ACK). Намеренно tcp_send_segment_seq(),
// а не tcp_send_segment(): та перезаписала бы last_segment_*/retry_count
// слежение за настоящим in-flight сегментом данных.
static void tcp_send_ack_only(void) {
    tcp_send_segment_seq(TCP_FLAG_ACK, NULL, 0, g_conn.snd_next);
}

void tcp_ack_consumed(void) {
    if (g_conn.rx_handed > 0) {
        g_conn.rx_len -= g_conn.rx_handed;
        // Сдвиг, не кольцо: очередь маленькая, а линейная память отдаёт
        // вызывающему непрерывный кусок одним указателем, без склейки.
        memmove(g_conn.rx_fifo, g_conn.rx_fifo + g_conn.rx_handed, g_conn.rx_len);
        g_conn.rx_handed = 0;
    }
    // ACK здесь прежде всего обновляет окно: байты уже подтверждены в
    // tcp_receive(), а место под следующие освободилось только сейчас.
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

    // Кумулятивный ACK — только НАПЕРЁД (signed-diff: seq/ack 32-битные и
    // оборачиваются, RFC 1982). tcp_retransmit_if_needed() использует это
    // поле, чтобы решить, слать ли последний сегмент повторно.
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

    // Данные принимаем только строго по порядку (нет буфера пересборки
    // внеочередных сегментов, см. tcp.h); пришедшее по порядку дописывается
    // в очередь, а не вытесняет предыдущее (см. "НАЙДЕННЫЙ БАГ" выше).
    if (data_len > 0) {
        if (seg_seq == g_conn.rcv_next) {
            uint32_t free_space = TCP_RX_FIFO_CAP - g_conn.rx_len;
            uint32_t take = data_len < free_space ? data_len : free_space;
            if (take > 0) {
                memcpy(g_conn.rx_fifo + g_conn.rx_len, data, take);
                g_conn.rx_len += take;
                g_conn.rcv_next += take;
            }
            // Подтверждаем ровно столько, сколько приняли: если сегмент
            // влез не целиком, окно уже отразит занятость, остаток дошлют.
            tcp_send_ack_only();
        } else {
            // Не по порядку (дубликат/дыра): данных не берём, но ACK с
            // текущим rcv_next шлём — он говорит, с чего повторять.
            tcp_send_ack_only();
        }
    }

    if ((flags & TCP_FLAG_FIN) && (seg_seq + data_len == g_conn.rcv_next)) {
        g_conn.rcv_next++;
        tcp_send_ack_only();
        g_conn.state = TCP_STATE_REMOTE_CLOSED;
    }
}
