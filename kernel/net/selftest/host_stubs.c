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

// В тестах всегда указывается числовой адрес (127.0.0.1), так что до DNS
// дело не доходит; заглушка существует только для линковки.
int dns_resolve(const char *hostname, uint32_t *out_ip) {
    (void)hostname; (void)out_ip;
    return -1;
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

    ssize_t n = recv(g_sock, g_stage, sizeof(g_stage), 0);
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
