/*
 * DEV-ONLY: гоняет НЕИЗМЕНЁННЫЕ kernel/net/tls.c и
 * kernel/net/http_client.c против настоящих серверов на localhost через
 * заглушки транспорта из host_stubs.c. НЕ входит в сборку ядра — см.
 * README в этом каталоге.
 *
 *   ./test_net tls   <port>           — полное рукопожатие TLS 1.2 + GET
 *   ./test_net fetch <url>            — http_fetch() целиком
 *   ./test_net url                    — только разбор URL (без сети)
 *   ./test_net expect <url> <code>    — ждём именно такой код ошибки
 *   ./test_net cap <url> <out_cap>    — http_fetch() с тесным буфером
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../tls.h"
#include "../http_client.h"
#include "../crypto/sha256.h"

static int g_fail = 0;

static void run_tls(uint16_t port) {
    uint32_t ip = (127u << 24) | 1u; // 127.0.0.1 в хостовом порядке, как в этом стеке

    if (tls_connect(ip, port, "localhost") != 0) {
        g_fail++;
        printf("FAIL  tls_connect (full TLS 1.2 handshake vs openssl s_server)\n");
        return;
    }
    printf("PASS  tls_connect: handshake + ServerKeyExchange signature verified\n");

    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (tls_send(req, (uint16_t)strlen(req)) != 0) {
        g_fail++;
        printf("FAIL  tls_send\n");
        tls_close();
        return;
    }
    printf("PASS  tls_send: encrypted application-data record accepted by server\n");

    uint8_t buf[4096];
    long total = 0;
    int saw_status = 0;
    for (;;) {
        int n = tls_recv(buf, sizeof(buf));
        if (n < 0) { g_fail++; printf("FAIL  tls_recv returned -1 after %ld bytes\n", total); break; }
        if (n == 0) break; // close_notify / FIN
        if (total == 0 && n >= 12 && memcmp(buf, "HTTP/1.", 7) == 0) saw_status = 1;
        total += n;
    }
    printf("%s  tls_recv: %ld decrypted bytes%s\n",
           (total > 0 && saw_status) ? "PASS" : "FAIL", total,
           saw_status ? " starting with an HTTP status line" : " (no HTTP status line seen)");
    if (!(total > 0 && saw_status)) g_fail++;

    tls_close();
}

static void run_fetch(const char *url) {
    static uint8_t body[1u << 20];
    int status = -1;

    int n = http_fetch(url, body, sizeof(body), &status);
    if (n < 0) {
        g_fail++;
        printf("FAIL  http_fetch(%s) -> %d (status %d)\n", url, n, status);
        return;
    }
    printf("PASS  http_fetch(%s) -> %d bytes, status %d\n", url, n, status);

    // SHA-256 тела — чтобы сверить побайтовую целость с тем, что отдал
    // сервер (особенно важно для chunked: любая ошибка в декодере
    // проявится именно здесь, а не в длине).
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256(body, (size_t)n, digest);
    printf("      sha256: ");
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) printf("%02x", digest[i]);
    printf("\n");

    // Печатаем начало тела — удобно глазами сверить с тем, что отдал сервер.
    int show = n < 120 ? n : 120;
    printf("      body[0..%d]: ", show);
    for (int i = 0; i < show; i++) {
        unsigned char c = body[i];
        putchar((c >= 32 && c < 127) ? c : '.');
    }
    printf("\n");
}

// Разбор URL проверяется через поведение http_fetch() на явно негодных
// входах: сети они не касаются, до соединения дело не доходит.
static void expect_fetch_error(const char *url, int want) {
    int status = 0;
    uint8_t buf[16];
    int r = http_fetch(url, buf, sizeof(buf), &status);
    if (r == want) {
        printf("PASS  http_fetch(\"%s\") -> %d as expected\n", url, want);
    } else {
        g_fail++;
        printf("FAIL  http_fetch(\"%s\") -> %d, expected %d\n", url, r, want);
    }
}

static void run_url_checks(void) {
    expect_fetch_error("ftp://example.com/x", HTTP_FETCH_EBADURL);
    expect_fetch_error("example.com/x", HTTP_FETCH_EBADURL);
    expect_fetch_error("http://", HTTP_FETCH_EBADURL);
    expect_fetch_error("https://user:pass@example.com/x", HTTP_FETCH_EBADURL);
    expect_fetch_error("http://[::1]/x", HTTP_FETCH_EBADURL);
    expect_fetch_error("http://example.com:0/x", HTTP_FETCH_EBADURL);
    expect_fetch_error("http://example.com:99999/x", HTTP_FETCH_EBADURL);
    // Имя хоста: DNS в заглушке всегда отказывает, значит разбор URL
    // прошёл и дело дошло именно до резолва.
    expect_fetch_error("http://example.com/x", HTTP_FETCH_EDNS);
    expect_fetch_error("https://raw.githubusercontent.com/a/b", HTTP_FETCH_EDNS);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "url") == 0) {
        run_url_checks();
    } else if (argc >= 3 && strcmp(argv[1], "tls") == 0) {
        run_tls((uint16_t)atoi(argv[2]));
    } else if (argc >= 3 && strcmp(argv[1], "fetch") == 0) {
        run_fetch(argv[2]);
    } else if (argc >= 4 && strcmp(argv[1], "expect") == 0) {
        expect_fetch_error(argv[2], atoi(argv[3]));
    } else if (argc >= 4 && strcmp(argv[1], "cap") == 0) {
        static uint8_t small[1u << 20];
        unsigned long cap = (unsigned long)atol(argv[3]);
        int status = 0;
        int r = http_fetch(argv[2], small, cap, &status);
        printf("%s  http_fetch(%s, cap=%lu) -> %d (status %d)\n",
               (r == HTTP_FETCH_ENOSPC) ? "PASS" : "FAIL", argv[2], cap, r, status);
        if (r != HTTP_FETCH_ENOSPC) g_fail++;
    } else {
        printf("usage: %s url | tls <port> | fetch <url>\n", argv[0]);
        return 2;
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES PRESENT" : "OK",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
