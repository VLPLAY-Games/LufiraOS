#include "http_client.h"
#include "net.h"
#include "tcp.h"
#include "tls.h"
#include "dns.h"
#include "drivers/net/rtl8139.h"
#include "system/timer/pit.h"
#include "system/devmode/devmode.h"
#include "lib/string.h"

/*
 * См. http_client.h — там же расписано, что сознательно НЕ поддерживается
 * (перенаправления, keep-alive, сжатие, userinfo/IPv6 в URL).
 *
 * Чтение организовано тем же приёмом, что и везде в этом стеке (tcp.c,
 * dns.c, tls.c): блокирующий цикл "опросить -> проверить -> pit_wait_ms(1)"
 * с бюджетом в тиках PIT. Два бюджета: на ОДНУ порцию данных и на весь
 * ответ целиком — первый ловит "сервер замолчал", второй не даёт
 * бесконечно тянуть ответ, который капает по байту.
 */

#define HTTP_PORT_DEFAULT       80
#define HTTPS_PORT_DEFAULT     443

#define HTTP_HOST_MAX          256
#define HTTP_PATH_MAX          512
// Накопитель статус-строки и заголовков. GitHub/Fastly присылают их
// довольно много (CSP, кеш, трассировка), 8КБ — с запасом.
#define HTTP_HEADER_MAX       8192
// Порция за один conn_read(). Должна быть НЕ МЕНЬШЕ того, что может
// отдать tcp_recv_poll() за раз (~1460), иначе лишнее придётся отбросить
// — а отбрасывать принятые байты нельзя, поток станет битым.
#define HTTP_READ_CHUNK       2048
#define HTTP_REQUEST_MAX      (HTTP_PATH_MAX + HTTP_HOST_MAX + 128)

// ~3000мс на одну порцию — тот же порядок, что у tcp_connect()/dns_resolve().
#define HTTP_READ_BUDGET_TICKS      300
// ~60с на весь ответ: пакет .lpg может быть на сотни килобайт, а канал
// QEMU SLIRP + программный AES-GCM (см. crypto/aes_gcm.h) быстрыми не
// назовёшь.
#define HTTP_TOTAL_BUDGET_TICKS    6000

typedef struct {
    int      is_https;
    char     host[HTTP_HOST_MAX];
    uint16_t port;
    char     path[HTTP_PATH_MAX];
} http_url_t;

// Текущее соединение: http:// идёт напрямую через tcp.c, https:// — через
// tls.c (который сам поднимает TCP внутри). Соединение, как и везде в
// этом стеке, единственное.
static int g_is_tls;

// ========== Транспорт ==========

static int conn_send(const void *data, uint16_t len) {
    return g_is_tls ? tls_send(data, len) : tcp_send(data, len);
}

// >0 — сколько байт прочитано, 0 — соединение корректно закрыто,
// -1 — ошибка или не дождались ничего за бюджет.
static int conn_read(uint8_t *buf, uint16_t cap) {
    if (g_is_tls) {
        // tls_recv() блокируется со своим бюджетом сам и уже имеет ровно
        // нужную семантику ">0 / 0 = закрыто / -1".
        return tls_recv(buf, cap);
    }

    uint64_t deadline = pit_get_ticks() + HTTP_READ_BUDGET_TICKS;
    for (;;) {
        uint8_t *p; uint16_t n;
        if (tcp_recv_poll(&p, &n)) {
            uint16_t take = (n > cap) ? cap : n;
            memcpy(buf, p, take);
            tcp_ack_consumed();
            // take < n означало бы молча потерянные байты; cap у всех
            // вызывающих >= HTTP_READ_CHUNK > размера куска tcp.c, так что
            // этого не бывает — но если когда-нибудь случится, честнее
            // оборвать, чем отдать дырявый поток.
            if (take < n) {
                DLOG("[HTTP] read chunk truncated (%u > %u)\n", (uint32_t)n, (uint32_t)cap);
                return -1;
            }
            return (int)take;
        }
        if (tcp_is_remote_closed()) return 0;
        if (pit_get_ticks() >= deadline) return -1;
        pit_wait_ms(1);
    }
}

static void conn_close(void) {
    if (g_is_tls) tls_close();
    else tcp_close();
}

// ========== Разбор URL ==========

static int http_parse_url(const char *url, http_url_t *out) {
    if (!url) return -1;

    const char *p;
    if (strncmp(url, "http://", 7) == 0) {
        out->is_https = 0;
        out->port = HTTP_PORT_DEFAULT;
        p = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        out->is_https = 1;
        out->port = HTTPS_PORT_DEFAULT;
        p = url + 8;
    } else {
        return -1; // любая другая схема (или её отсутствие) — не наше дело
    }

    // Хост: до '/', ':' или конца строки. userinfo ('@') и IPv6-литералы
    // ('[') явно отвергаем, а не разбираем криво (см. объём в http_client.h).
    uint32_t hl = 0;
    while (*p && *p != '/' && *p != ':') {
        if (*p == '@' || *p == '[' || *p == ']') return -1;
        if (hl + 1 >= HTTP_HOST_MAX) return -1;
        out->host[hl++] = *p++;
    }
    if (hl == 0) return -1;
    out->host[hl] = '\0';

    if (*p == ':') {
        p++;
        uint32_t port = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (uint32_t)(*p - '0');
            if (port > 65535) return -1;
            digits++;
            p++;
        }
        if (digits == 0 || port == 0) return -1;
        out->port = (uint16_t)port;
    }

    if (*p == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
        return 0;
    }
    if (*p != '/') return -1; // после хоста/порта может идти только путь

    uint32_t pl = 0;
    while (*p) {
        if (pl + 1 >= HTTP_PATH_MAX) return -1;
        out->path[pl++] = *p++;
    }
    out->path[pl] = '\0';
    return 0;
}

// ========== Сборка запроса ==========

// Простое добавление строки в буфер с контролем границы. Возвращает новую
// длину или -1.
static int req_append(char *buf, int len, int cap, const char *s) {
    while (*s) {
        if (len + 1 >= cap) return -1;
        buf[len++] = *s++;
    }
    return len;
}

static int req_append_u16(char *buf, int len, int cap, uint16_t v) {
    char digits[6];
    int n = 0;
    if (v == 0) digits[n++] = '0';
    while (v > 0) { digits[n++] = (char)('0' + (v % 10)); v = (uint16_t)(v / 10); }
    while (n > 0) {
        if (len + 1 >= cap) return -1;
        buf[len++] = digits[--n];
    }
    return len;
}

static int http_build_request(char *buf, int cap, const http_url_t *u) {
    int len = 0;
    len = req_append(buf, len, cap, "GET ");
    if (len >= 0) len = req_append(buf, len, cap, u->path);
    if (len >= 0) len = req_append(buf, len, cap, " HTTP/1.1\r\nHost: ");
    if (len >= 0) len = req_append(buf, len, cap, u->host);
    // Порт в Host указываем только если он нестандартный — так принято, и
    // лишний ":443" иногда мешает виртуальному хостингу.
    uint16_t default_port = u->is_https ? HTTPS_PORT_DEFAULT : HTTP_PORT_DEFAULT;
    if (len >= 0 && u->port != default_port) {
        len = req_append(buf, len, cap, ":");
        if (len >= 0) len = req_append_u16(buf, len, cap, u->port);
    }
    if (len >= 0) {
        len = req_append(buf, len, cap,
                         "\r\nUser-Agent: LufiraOS-dlpg/1.0\r\n"
                         "Accept: */*\r\n"
                         // Connection: close делает ответ однозначным и
                         // заодно даёт законный способ обрамления тела
                         // "до закрытия соединения", если сервер не
                         // пришлёт ни Content-Length, ни chunked.
                         "Connection: close\r\n\r\n");
    }
    return len;
}

// ========== Разбор заголовков ==========

// Сравнивает начало строки с именем заголовка (регистронезависимо) и
// проверяет, что сразу за ним идёт ':'. Возвращает указатель на значение
// (с пропущенными пробелами) или NULL.
static const char *header_value(const char *line, const char *name) {
    const char *l = line;
    const char *n = name;
    while (*n) {
        if (*l == '\0' || to_lower(*l) != to_lower(*n)) return NULL;
        l++; n++;
    }
    if (*l != ':') return NULL;
    l++;
    while (*l == ' ' || *l == '\t') l++;
    return l;
}

// Есть ли в значении заголовка подстрока needle (регистронезависимо) —
// нужно для "Transfer-Encoding: chunked", который может прийти и в виде
// списка ("gzip, chunked").
static int value_contains(const char *value, const char *needle) {
    for (const char *s = value; *s; s++) {
        const char *a = s, *b = needle;
        while (*b && to_lower(*a) == to_lower(*b)) { a++; b++; }
        if (*b == '\0') return 1;
    }
    return 0;
}

static unsigned long parse_ulong(const char *s) {
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned long)(*s - '0');
        s++;
    }
    return v;
}

// ========== Декодер chunked ==========

typedef enum {
    CHUNK_SIZE = 0,   // накапливаем шестнадцатеричный размер
    CHUNK_SIZE_EXT,   // после ';' — расширения чанка, игнорируем до конца строки
    CHUNK_SIZE_LF,    // увидели '\r' в строке размера, ждём '\n'
    CHUNK_DATA,       // копируем remaining байт данных
    CHUNK_DATA_CR,    // после данных ждём '\r'
    CHUNK_DATA_LF,    // ...затем '\n'
    CHUNK_TRAILER,    // после нулевого чанка: пропускаем трейлеры до пустой строки
    CHUNK_DONE,
} chunk_phase_t;

typedef struct {
    chunk_phase_t phase;
    unsigned long remaining;    // сколько байт данных текущего чанка осталось
    unsigned long size;         // разбираемый размер
    int           size_digits;
    int           trailer_chars; // непустая ли текущая строка трейлера
} chunk_state_t;

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Пропускает через себя len входных байт, складывая декодированное тело в
 * out[*off..]. Возвращает 0 — продолжаем, 1 — тело закончилось (пришёл
 * нулевой чанк и его трейлеры), HTTP_FETCH_EHTTP — битое обрамление,
 * HTTP_FETCH_ENOSPC — тело не влезает в out_cap.
 */
static int chunked_feed(chunk_state_t *st, const uint8_t *in, uint32_t len,
                        uint8_t *out, unsigned long out_cap, unsigned long *off) {
    uint32_t i = 0;
    while (i < len) {
        if (st->phase == CHUNK_DONE) return 1;

        if (st->phase == CHUNK_DATA) {
            // Единственная фаза, где стоит копировать пачкой, а не побайтово.
            unsigned long avail = len - i;
            unsigned long take = (avail < st->remaining) ? avail : st->remaining;
            if (*off + take > out_cap) return HTTP_FETCH_ENOSPC;
            memcpy(out + *off, in + i, take);
            *off += take;
            i += (uint32_t)take;
            st->remaining -= take;
            if (st->remaining == 0) st->phase = CHUNK_DATA_CR;
            continue;
        }

        char c = (char)in[i++];
        switch (st->phase) {
        case CHUNK_SIZE: {
            int d = hex_digit(c);
            if (d >= 0) {
                if (st->size_digits > 8) return HTTP_FETCH_EHTTP; // чанк >4ГБ — явно мусор
                st->size = (st->size << 4) | (unsigned long)d;
                st->size_digits++;
            } else if (c == ';') {
                if (st->size_digits == 0) return HTTP_FETCH_EHTTP;
                st->phase = CHUNK_SIZE_EXT;
            } else if (c == '\r') {
                if (st->size_digits == 0) return HTTP_FETCH_EHTTP;
                st->phase = CHUNK_SIZE_LF;
            } else {
                return HTTP_FETCH_EHTTP;
            }
            break;
        }
        case CHUNK_SIZE_EXT:
            if (c == '\r') st->phase = CHUNK_SIZE_LF;
            break;
        case CHUNK_SIZE_LF:
            if (c != '\n') return HTTP_FETCH_EHTTP;
            if (st->size == 0) {
                st->phase = CHUNK_TRAILER;
                st->trailer_chars = 0;
            } else {
                st->remaining = st->size;
                st->phase = CHUNK_DATA;
            }
            st->size = 0;
            st->size_digits = 0;
            break;
        case CHUNK_DATA_CR:
            if (c != '\r') return HTTP_FETCH_EHTTP;
            st->phase = CHUNK_DATA_LF;
            break;
        case CHUNK_DATA_LF:
            if (c != '\n') return HTTP_FETCH_EHTTP;
            st->phase = CHUNK_SIZE;
            break;
        case CHUNK_TRAILER:
            // Трейлеры нам не нужны — ищем просто пустую строку, которая
            // их завершает (её же мы увидим сразу, если трейлеров нет).
            if (c == '\n') {
                if (st->trailer_chars == 0) {
                    st->phase = CHUNK_DONE;
                    return 1;
                }
                st->trailer_chars = 0;
            } else if (c != '\r') {
                st->trailer_chars++;
            }
            break;
        default:
            return HTTP_FETCH_EHTTP;
        }
    }
    return (st->phase == CHUNK_DONE) ? 1 : 0;
}

// ========== Основная функция ==========

// Статические буферы: суммарно ~10КБ, в стек ядра такое класть нельзя.
static char    g_header_buf[HTTP_HEADER_MAX];
static uint8_t g_read_buf[HTTP_READ_CHUNK];
static char    g_request_buf[HTTP_REQUEST_MAX];

int http_fetch(const char *url, void *out_buf, unsigned long out_cap, int *status_out) {
    if (!rtl8139_found()) return HTTP_FETCH_ENODEV;

    http_url_t u;
    if (http_parse_url(url, &u) != 0) return HTTP_FETCH_EBADURL;

    // Числовой адрес хоста резолвить не нужно (и нечем, если DNS не
    // отвечает) — ровно та же развилка, что была у wget, см. net.h.
    uint32_t ip;
    if (ip_str_to_addr(u.host, &ip) != 0) {
        if (dns_resolve(u.host, &ip) != 0) {
            DLOG("[HTTP] DNS resolution failed for %s\n", u.host);
            return HTTP_FETCH_EDNS;
        }
    }

    int req_len = http_build_request(g_request_buf, (int)sizeof(g_request_buf), &u);
    if (req_len < 0) return HTTP_FETCH_EBADURL; // не влез — считаем URL негодным

    g_is_tls = u.is_https;
    if (u.is_https) {
        if (tls_connect(ip, u.port, u.host) != 0) return HTTP_FETCH_ETLS;
    } else {
        if (tcp_connect(ip, u.port) != 0) return HTTP_FETCH_ECONNECT;
    }

    if (conn_send(g_request_buf, (uint16_t)req_len) != 0) {
        conn_close();
        return u.is_https ? HTTP_FETCH_ETLS : HTTP_FETCH_ECONNECT;
    }

    // --- Приём: сначала заголовки целиком, затем тело ---
    uint64_t total_deadline = pit_get_ticks() + HTTP_TOTAL_BUDGET_TICKS;

    uint32_t hdr_len = 0;
    uint32_t body_start = 0; // смещение начала тела внутри g_header_buf
    int headers_done = 0;
    int conn_closed = 0;

    while (!headers_done) {
        if (pit_get_ticks() >= total_deadline) {
            conn_close();
            return HTTP_FETCH_EHTTP;
        }

        int n = conn_read(g_read_buf, sizeof(g_read_buf));
        if (n < 0) { conn_close(); return HTTP_FETCH_EHTTP; }
        if (n == 0) {
            // Соединение закрылось, не прислав пустой строки после
            // заголовков — валидного ответа не получилось.
            conn_close();
            return HTTP_FETCH_EHTTP;
        }

        if (hdr_len + (uint32_t)n > HTTP_HEADER_MAX) {
            DLOG("[HTTP] headers exceed %u bytes\n", (uint32_t)HTTP_HEADER_MAX);
            conn_close();
            return HTTP_FETCH_EHTTP;
        }
        memcpy(g_header_buf + hdr_len, g_read_buf, (uint32_t)n);
        uint32_t search_from = (hdr_len >= 3) ? (hdr_len - 3) : 0;
        hdr_len += (uint32_t)n;

        // Ищем пустую строку "\r\n\r\n", начиная чуть раньше дописанного
        // — разделитель мог оказаться разрезан между порциями.
        for (uint32_t i = search_from; i + 4 <= hdr_len; i++) {
            if (g_header_buf[i] == '\r' && g_header_buf[i + 1] == '\n' &&
                g_header_buf[i + 2] == '\r' && g_header_buf[i + 3] == '\n') {
                g_header_buf[i] = '\0'; // обрезаем на разделителе, дальше — тело
                body_start = i + 4;
                headers_done = 1;
                break;
            }
        }
    }

    /*
     * Статус-строка: "HTTP/1.x SSS текст". Разбираем строго — именно
     * непрошедший этот разбор ответ и есть HTTP_FETCH_EHTTP (в отличие от
     * любого валидного кода, включая 404, который ошибкой НЕ считается,
     * см. http_client.h).
     */
    if (strncmp(g_header_buf, "HTTP/1.", 7) != 0) {
        DLOG("[HTTP] malformed status line\n");
        conn_close();
        return HTTP_FETCH_EHTTP;
    }
    const char *sp = g_header_buf + 7;
    if (*sp < '0' || *sp > '9') { conn_close(); return HTTP_FETCH_EHTTP; }
    sp++;
    if (*sp != ' ') { conn_close(); return HTTP_FETCH_EHTTP; }
    sp++;
    if (sp[0] < '0' || sp[0] > '9' || sp[1] < '0' || sp[1] > '9' ||
        sp[2] < '0' || sp[2] > '9') {
        conn_close();
        return HTTP_FETCH_EHTTP;
    }
    int status = (sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0');
    if (status_out) *status_out = status;
    DLOG("[HTTP] %s%s -> %d\n", u.is_https ? "https://" : "http://", u.host, status);

    // Заголовки: нужны ровно два. Разбор идёт по уже обрезанной на
    // разделителе области g_header_buf, строки терминируем на месте.
    int has_content_length = 0;
    unsigned long content_length = 0;
    int is_chunked = 0;

    uint32_t line_start = 0;
    // Пропускаем саму статус-строку.
    while (line_start < body_start && g_header_buf[line_start] != '\n') line_start++;
    if (line_start < body_start) line_start++;

    while (line_start < body_start && g_header_buf[line_start] != '\0') {
        uint32_t e = line_start;
        while (e < body_start && g_header_buf[e] != '\r' && g_header_buf[e] != '\n' &&
               g_header_buf[e] != '\0') {
            e++;
        }
        char saved = g_header_buf[e];
        g_header_buf[e] = '\0';

        const char *v;
        if ((v = header_value(g_header_buf + line_start, "content-length")) != NULL) {
            has_content_length = 1;
            content_length = parse_ulong(v);
        } else if ((v = header_value(g_header_buf + line_start, "transfer-encoding")) != NULL) {
            if (value_contains(v, "chunked")) is_chunked = 1;
        }

        g_header_buf[e] = saved;
        if (e >= body_start) break;
        // Переходим к следующей строке (через CRLF или одиночный LF).
        line_start = e;
        while (line_start < body_start &&
               (g_header_buf[line_start] == '\r' || g_header_buf[line_start] == '\n')) {
            line_start++;
        }
    }

    // chunked имеет приоритет над Content-Length (RFC 7230 §3.3.3) — если
    // пришло и то и другое, Content-Length игнорируется.
    if (is_chunked) has_content_length = 0;

    uint8_t *out = (uint8_t *)out_buf;
    unsigned long off = 0;
    chunk_state_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.phase = CHUNK_SIZE;

    // Байты тела, уже пришедшие вместе с заголовками.
    const uint8_t *leftover = (const uint8_t *)g_header_buf + body_start;
    uint32_t leftover_len = hdr_len - body_start;

    // Если Content-Length нулевой или тела вообще нет — тело уже "целиком
    // получено", и ни одного чтения делать не надо (иначе цикл ниже ждал
    // бы данных, которых не будет, весь свой бюджет).
    const uint8_t *in = leftover;
    uint32_t in_len = leftover_len;

    for (;;) {
        if (in_len > 0) {
            if (is_chunked) {
                int rc = chunked_feed(&chunk, in, in_len, out, out_cap, &off);
                if (rc < 0) { conn_close(); return rc; }
            } else {
                uint32_t take = in_len;
                if (has_content_length) {
                    unsigned long need = content_length - off; // off <= content_length — см. ниже
                    if ((unsigned long)take > need) take = (uint32_t)need;
                }
                if (off + take > out_cap) { conn_close(); return HTTP_FETCH_ENOSPC; }
                if (take > 0) memcpy(out + off, in, take);
                off += take;
            }
            in_len = 0;
        }

        // Условия завершения проверяются ДО попытки дочитать — иначе
        // пустое тело (Content-Length: 0) или уже дочитанное целиком
        // заставили бы ждать несуществующие данные.
        if (is_chunked) {
            if (chunk.phase == CHUNK_DONE) break;
        } else if (has_content_length) {
            if (off >= content_length) break;
        }

        if (conn_closed) {
            // Соединение закрыто. Для обрамления "до закрытия" это и есть
            // нормальный конец тела; для Content-Length/chunked — обрыв.
            if (!has_content_length && !is_chunked) break;
            DLOG("[HTTP] connection closed before body end (%u of %u)\n",
                 (uint32_t)off, (uint32_t)content_length);
            conn_close();
            return HTTP_FETCH_EHTTP;
        }

        if (pit_get_ticks() >= total_deadline) {
            DLOG("[HTTP] total response budget exhausted\n");
            conn_close();
            return HTTP_FETCH_EHTTP;
        }

        int n = conn_read(g_read_buf, sizeof(g_read_buf));
        if (n < 0) { conn_close(); return HTTP_FETCH_EHTTP; }
        if (n == 0) {
            conn_closed = 1;
            continue;
        }
        in = g_read_buf;
        in_len = (uint32_t)n;
    }

    conn_close();
    return (int)off;
}
