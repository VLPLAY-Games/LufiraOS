/*
 * DEV-ONLY: хостовые заглушки под тот же набор функций, которым tls.c и
 * http_client.c пользуются в ядре (tcp.c, pit.c, rtl8139.c), но поверх
 * обычных POSIX-сокетов и часов Linux. НЕ входит в сборку ядра.
 *
 * Смысл: прогнать НЕИЗМЕНЁННЫЕ kernel/net/tls.c и
 * kernel/net/http_client.c против настоящего TLS-сервера (openssl
 * s_server) и настоящего HTTP-сервера на localhost. Живую сеть ядра этим
 * не проверить, но всё, что ниже драйвера — разбор записей, сборка
 * handshake-сообщений через границы кусков, вывод ключей, AES-GCM на
 * записях, проверка подписи сертификата, разбор HTTP и chunked —
 * проверяется ровно тем же кодом, который попадёт в ядро.
 *
 * Семантика заглушек повторяет документированную в kernel/net/tcp.h:
 * tcp_recv_poll() отдаёт ОДИН уже пришедший кусок (до 1460 байт) и ждёт
 * tcp_ack_consumed() перед следующим — чтобы проверялась именно та логика
 * пересборки, которая нужна в ядре.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define STUB_CHUNK 1460

static int      g_sock = -1;
static uint8_t  g_stage[STUB_CHUNK];
static uint16_t g_stage_len;
static int      g_has_data;
static int      g_remote_closed;

/*
 * Имитация активного вмешательства в канал: если задана переменная
 * окружения LUFIRA_TAMPER_OFFSET, в принятом потоке портится один бит по
 * этому абсолютному смещению. Нужно, чтобы проверить, что tls.c на
 * несошедшейся подписи ServerKeyExchange действительно ОБРЫВАЕТ
 * рукопожатие, а не продолжает его (см. раздел про доверие в tls.c);
 * смещение 11 попадает в server_random, который входит в подписанные
 * данные.
 */
static long     g_tamper_offset = -2; // -2 = ещё не читали окружение
static uint64_t g_rx_total;

int rtl8139_found(void) { return 1; }
void rtl8139_poll(void) { }

uint64_t pit_get_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // 100 Гц, как настоящий PIT в этой ОС (10мс на тик).
    return (uint64_t)ts.tv_sec * 100u + (uint64_t)(ts.tv_nsec / 10000000L);
}

void pit_wait_ms(uint32_t ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int ip_str_to_addr(const char *s, uint32_t *out_ip) {
    uint32_t parts[4];
    int n = 0;
    const char *p = s;
    for (n = 0; n < 4; n++) {
        if (*p < '0' || *p > '9') return -1;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
        if (v > 255) return -1;
        parts[n] = v;
        if (n < 3) { if (*p != '.') return -1; p++; }
    }
    if (*p != '\0' && *p != ' ') return -1;
    *out_ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 0;
}

/*
 * Резолвер хоста. По умолчанию ОТКАЗЫВАЕТ — ровно это и проверяет
 * run_url_checks() в test_net.c (успешный разбор URL виден по тому, что
 * дело дошло до HTTP_FETCH_EDNS, а не до EBADURL), и localhost-тестам
 * DNS не нужен вовсе: там адрес задан числом.
 *
 * Если выставлена LUFIRA_REAL_DNS=1 — используется настоящий
 * getaddrinfo() хоста. Это нужно, чтобы гонять tls.c против НАСТОЯЩЕГО
 * сервера в интернете (raw.githubusercontent.com): без разрешения имени
 * туда не попасть, а подставить IP вместо имени нельзя — тогда в SNI
 * уедет IP, и Fastly отдаст либо чужой сертификат, либо отказ (см.
 * расширение server_name в tls_build_client_hello()).
 */
int dns_resolve(const char *hostname, uint32_t *out_ip) {
    const char *real = getenv("LUFIRA_REAL_DNS");
    if (!real || real[0] != '1') return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // этот стек знает только IPv4, см. ip.c
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || !res) return -1;

    uint32_t net_order = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    const uint8_t *b = (const uint8_t *)&net_order;
    // Внутри этого стека адреса ходят в хостовом порядке (см. net.h), а
    // getaddrinfo() отдаёт сетевой — разбираем побайтово, без htonl.
    *out_ip = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
              ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    freeaddrinfo(res);
    return 0;
}

int tcp_connect(uint32_t remote_ip, uint16_t remote_port) {
    if (g_sock >= 0) close(g_sock);
    g_stage_len = 0;
    g_has_data = 0;
    g_remote_closed = 0;
    g_rx_total = 0;

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = (uint16_t)((remote_port >> 8) | (remote_port << 8));
    addr.sin_addr.s_addr = (uint32_t)(
        ((remote_ip & 0xFF) << 24) | (((remote_ip >> 8) & 0xFF) << 16) |
        (((remote_ip >> 16) & 0xFF) << 8) | ((remote_ip >> 24) & 0xFF));

    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(g_sock);
        g_sock = -1;
        return -1;
    }
    int flags = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

int tcp_send(const void *data, uint16_t len) {
    if (g_sock < 0) return -1;
    const uint8_t *p = (const uint8_t *)data;
    uint16_t sent = 0;
    while (sent < len) {
        ssize_t n = send(g_sock, p + sent, (size_t)(len - sent), 0);
        if (n > 0) { sent += (uint16_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { pit_wait_ms(1); continue; }
        return -1;
    }
    return 0;
}

int tcp_recv_poll(uint8_t **out_ptr, uint16_t *out_len) {
    if (g_has_data) {
        if (out_ptr) *out_ptr = g_stage;
        if (out_len) *out_len = g_stage_len;
        return 1;
    }
    if (g_sock < 0) return 0;

    // LUFIRA_RX_CHUNK позволяет зажать размер одного «сегмента» до
    // сколь угодно мелкого — так проверяется, что пересборка записей и
    // handshake-сообщений не зависит от того, по каким границам поток
    // порезан (в ядре границы кусков диктует TCP и они произвольны).
    size_t want = sizeof(g_stage);
    {
        const char *e = getenv("LUFIRA_RX_CHUNK");
        if (e) {
            long v = atol(e);
            if (v > 0 && (size_t)v < want) want = (size_t)v;
        }
    }
    ssize_t n = recv(g_sock, g_stage, want, 0);
    if (n > 0) {
        if (g_tamper_offset == -2) {
            const char *e = getenv("LUFIRA_TAMPER_OFFSET");
            g_tamper_offset = e ? atol(e) : -1;
        }
        if (g_tamper_offset >= 0 &&
            (uint64_t)g_tamper_offset >= g_rx_total &&
            (uint64_t)g_tamper_offset < g_rx_total + (uint64_t)n) {
            g_stage[(uint64_t)g_tamper_offset - g_rx_total] ^= 0x01;
            printf("[STUB] tampered byte at stream offset %ld\n", g_tamper_offset);
        }
        g_rx_total += (uint64_t)n;

        g_stage_len = (uint16_t)n;
        g_has_data = 1;
        if (out_ptr) *out_ptr = g_stage;
        if (out_len) *out_len = g_stage_len;
        return 1;
    }
    if (n == 0) g_remote_closed = 1;
    return 0;
}

void tcp_ack_consumed(void) {
    g_has_data = 0;
    g_stage_len = 0;
}

int tcp_is_remote_closed(void) { return g_remote_closed; }

void tcp_close(void) {
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
    g_has_data = 0;
    g_stage_len = 0;
}
