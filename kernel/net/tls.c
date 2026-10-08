#include "tls.h"
#include "tcp.h"
#include "crypto/sha256.h"
#include "crypto/hmac.h"
#include "crypto/aes_gcm.h"
#include "crypto/x25519.h"
#include "crypto/bignum.h"
#include "crypto/rsa_verify.h"
#include "drivers/net/rtl8139.h"
#include "system/timer/pit.h"
#include "system/devmode/devmode.h"
#include "lib/string.h"

/*
 * ===================== ОБЪЁМ И ЕГО ГРАНИЦЫ =====================
 *
 * Это клиент ТОЛЬКО TLS 1.2 и ТОЛЬКО с одним набором шифров:
 * TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xC02F). Если сервер выберет
 * что-то другое — рукопожатие обрывается. Единственная причина, по которой
 * выбран именно этот набор: его поддерживают все, кто нам нужен
 * (github.com/raw.githubusercontent.com за Fastly), и он целиком
 * собирается из пяти примитивов, которые можно написать и проверить по
 * контрольным векторам за обозримое время (см. kernel/net/crypto/ и
 * crypto/selftest/).
 *
 * Чего здесь НЕТ и не планируется: TLS 1.3 (другая схема вывода ключей и
 * key_share вместо ServerKeyExchange), TLS 1.0/1.1, возобновление сессий
 * (session ID/tickets), переустановка соединения (renegotiation),
 * клиентские сертификаты, сжатие, несколько одновременных соединений
 * (состояние статическое, как и у tcp.c), фрагментация ПЕРЕДАВАЕМЫХ
 * записей крупнее TLS_TX_PLAIN_MAX в одну запись, OCSP/SCT/ALPN.
 *
 * ===================== ДОВЕРИЕ: ЧТО ИМЕННО ПРОВЕРЯЕТСЯ =====================
 *
 * Проверяется РОВНО ОДНО: подпись под ServerKeyExchange сходится с
 * открытым RSA-ключом из первого (leaf) сертификата, который сервер сам
 * же и прислал. Это доказывает, что на другом конце TCP-соединения сидит
 * владелец закрытого ключа к предъявленному сертификату — то есть
 * отсекает тупую подмену трафика кем-то, у кого вообще нет никакого
 * валидного ключа, и отсекает подмену параметров ECDHE на лету.
 *
 * Чего это НЕ даёт: цепочка сертификатов НЕ проверяется — ни до
 * доверенного корневого CA (набора корней в этой ОС просто нет), ни по
 * сроку действия, ни по совпадению имени хоста с CN/SubjectAltName.
 * Атакующий, способный перехватить соединение и предъявить СВОЙ
 * самоподписанный сертификат с собственным ключом, пройдёт эту проверку.
 * Это сознательно принятое урезание объёма (полноценная валидация требует
 * встроенного набора корневых сертификатов, разбора расширений X.509 и
 * сверки имён), а не недосмотр — то же самое, слово в слово, написано у
 * SYS_NET_FETCH в kernel/system/syscall/syscall.h, потому что это часть
 * контракта, видимая userspace.
 *
 * ===================== МОДЕЛЬ РАБОТЫ =====================
 *
 * Как и tcp_connect()/dns_resolve()/icmp_ping_wait(): блокирующая функция,
 * внутри которой цикл "пока не вышел бюджет тиков PIT — опрашивать карту,
 * проверять состояние, pit_wait_ms(1)". Никакой новой модели
 * конкурентности не вводится.
 *
 * ВАЖНО про приём: tcp_recv_poll() отдаёт ОДИН уже пришедший кусок (до
 * ~1460 байт) и ничего не склеивает. TLS-запись же может быть до ~16КБ,
 * то есть заведомо приходит несколькими кусками, и границы кусков с
 * границами записей не совпадают никак. Поэтому входящие байты
 * накапливаются в g_tls.rx и целые записи выковыриваются оттуда уже сами
 * (см. tls_read_record()). Ровно так же handshake-СООБЩЕНИЕ может быть
 * разрезано между записями — его собирает g_tls.hs (см. tls_hs_next()).
 */

// ========== Константы протокола ==========

#define TLS_CT_CHANGE_CIPHER_SPEC 20
#define TLS_CT_ALERT              21
#define TLS_CT_HANDSHAKE          22
#define TLS_CT_APPLICATION_DATA   23

#define TLS_HS_CLIENT_HELLO        1
#define TLS_HS_SERVER_HELLO        2
#define TLS_HS_NEW_SESSION_TICKET  4
#define TLS_HS_CERTIFICATE        11
#define TLS_HS_SERVER_KEY_EXCHANGE 12
#define TLS_HS_CERTIFICATE_REQUEST 13
#define TLS_HS_SERVER_HELLO_DONE  14
#define TLS_HS_CLIENT_KEY_EXCHANGE 16
#define TLS_HS_FINISHED           20

#define TLS_CIPHER_ECDHE_RSA_AES128_GCM_SHA256 0xC02Fu
#define TLS_GROUP_X25519                       0x001Du
#define TLS_SIGALG_RSA_PKCS1_SHA256            0x0401u

#define TLS_ALERT_CLOSE_NOTIFY 0

#define TLS_VERIFY_DATA_LEN 12
#define TLS_MASTER_SECRET_LEN 48
// client_write_key(16) + server_write_key(16) + client_write_IV(4) +
// server_write_IV(4). MAC-ключей нет: GCM — это AEAD, отдельного MAC в
// RFC 5288 не предусмотрено.
#define TLS_KEY_BLOCK_LEN 40
#define TLS_FIXED_IV_LEN 4
#define TLS_EXPLICIT_NONCE_LEN 8

// ========== Размеры буферов ==========

#define TLS_MAX_PLAINTEXT 16384
// Запас поверх открытого текста: 8 байт явного nonce + 16 байт тега GCM;
// плюс поле длины записи 16-битное, так что сервер в принципе может
// объявить больше — всё, что не влезает, считаем фатально битым.
#define TLS_REC_CAP       (TLS_MAX_PLAINTEXT + 512)
// Накопитель сырых TCP-байт: целая запись с заголовком плюс один кусок
// tcp_recv_poll() "про запас", чтобы приём следующей записи уже начинался,
// пока текущая разбирается.
#define TLS_RX_CAP        (5 + TLS_REC_CAP + 1600)
// Сборка одного handshake-сообщения. Самое большое из них — Certificate с
// полной цепочкой; 16КБ с запасом хватает (цепочка GitHub/Fastly ~4-5КБ).
#define TLS_HS_CAP        16384
// Записи МЫ отправляем мелкие (запрос HTTP) — незачем держать 16КБ.
#define TLS_TX_PLAIN_MAX  4096
#define TLS_TX_CAP        (5 + TLS_EXPLICIT_NONCE_LEN + TLS_TX_PLAIN_MAX + AES_GCM_TAG_SIZE)
// Буфер под ClientHello: 2+32+1+2+2+2 фиксированной части, расширения, и
// имя хоста в SNI. 512 байт — с большим запасом.
#define TLS_CLIENT_HELLO_CAP 512

// ========== Бюджеты времени (тики PIT, 10мс) ==========

// Тот же порядок, что у tcp_connect()/dns_resolve() — ~3000мс на ожидание
// одной записи от сервера.
#define TLS_RECORD_BUDGET_TICKS 300
// Всё рукопожатие целиком: несколько обменов плюс довольно медленная (см.
// bignum.h) проверка подписи RSA.
#define TLS_HANDSHAKE_BUDGET_TICKS 1500

typedef struct {
    int active;         // рукопожатие завершено, можно send/recv
    int fatal;          // случилась неустранимая ошибка — всё дальше отказывает
    int closed;         // пришёл close_notify либо TCP FIN и данных больше нет

    // --- приём ---
    uint8_t  rx[TLS_RX_CAP];
    uint32_t rx_len;
    uint8_t  rec[TLS_REC_CAP]; // payload текущей записи (расшифрованный, если шифрование включено)
    uint32_t rec_len;
    uint32_t app_off;          // сколько из rec[] уже отдано через tls_recv()
    uint32_t app_len;          // сколько в rec[] прикладных данных всего

    // --- сборка handshake-сообщений ---
    uint8_t  hs[TLS_HS_CAP];
    uint32_t hs_len;
    uint32_t hs_consume;       // сколько байт с начала hs[] освободить на следующем вызове

    // --- рукопожатие ---
    sha256_ctx_t hs_hash;      // хеш всех handshake-сообщений по порядку
    sha256_ctx_t hs_hash_before; // его снимок ДО последнего выданного сообщения (нужен для Finished)
    uint8_t  client_random[32];
    uint8_t  server_random[32];
    uint8_t  client_priv[X25519_KEY_SIZE];
    uint8_t  client_pub[X25519_KEY_SIZE];
    uint8_t  master_secret[TLS_MASTER_SECRET_LEN];

    // Открытый ключ из leaf-сертификата — именно КОПИЯ, а не указатели в
    // hs[]. x509_extract_rsa_pubkey() разбирает DER по месту и отдаёт
    // указатели внутрь переданного буфера (см. rsa_verify.h), а hs[]
    // сдвигается при выдаче каждого следующего handshake-сообщения — то
    // есть к моменту разбора ServerKeyExchange байты сертификата там уже
    // затёрты. Поэтому модуль и экспонента копируются сразу.
    uint8_t  cert_modulus[BIGNUM_MAX_BYTES];
    uint32_t cert_modulus_len;
    uint8_t  cert_exponent[8];
    uint32_t cert_exponent_len;

    // --- шифрование записей ---
    aes_gcm_ctx_t enc;
    aes_gcm_ctx_t dec;
    uint8_t  client_iv[TLS_FIXED_IV_LEN];
    uint8_t  server_iv[TLS_FIXED_IV_LEN];
    uint64_t client_seq;
    uint64_t server_seq;
    int      encrypt_active;
    int      decrypt_active;

    uint8_t  tx[TLS_TX_CAP];
} tls_state_t;

static tls_state_t g_tls;

// ========== Источник случайности ==========

static inline uint64_t tls_read_tsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/*
 * Единственные источники непредсказуемости, доступные этому ядру: счётчик
 * тактов процессора (rdtsc) и счётчик тиков PIT. Генератора случайных
 * чисел в системе нет вообще, RDRAND не проверяется (его может не быть).
 *
 * ЭТО СЛАБАЯ СЛУЧАЙНОСТЬ, и это осознанно задокументировано, а не
 * замаскировано: на реальном железе младшие биты rdtsc между вызовами
 * действительно непредсказуемы, но в детерминированном эмуляторе
 * предсказуемость заметно выше. Последствие: секретный скаляр ECDHE и
 * client_random в теории угадываемы, то есть долговременной секретности
 * сессии (forward secrecy) эта реализация по-настоящему не даёт. Для
 * задачи "скачать публичный индекс пакетов и публичные .lpg-файлы по
 * https" этого достаточно, для передачи секретов — нет.
 *
 * Пул прокручивается через SHA-256, чтобы вызванная дважды подряд функция
 * не отдавала коррелированные байты.
 */
static void tls_gather_random(uint8_t *out, uint32_t len) {
    static uint8_t pool[SHA256_DIGEST_SIZE];
    static uint64_t counter = 0;

    uint32_t done = 0;
    while (done < len) {
        uint8_t mix[SHA256_DIGEST_SIZE + 8 * 5];
        memcpy(mix, pool, SHA256_DIGEST_SIZE);

        uint64_t samples[5];
        samples[0] = tls_read_tsc();
        samples[1] = pit_get_ticks();
        samples[2] = tls_read_tsc();
        samples[3] = counter++;
        samples[4] = tls_read_tsc();
        for (int i = 0; i < 5; i++) {
            for (int b = 0; b < 8; b++) {
                mix[SHA256_DIGEST_SIZE + i * 8 + b] = (uint8_t)(samples[i] >> (b * 8));
            }
        }

        sha256(mix, sizeof(mix), pool);

        uint32_t take = len - done;
        if (take > SHA256_DIGEST_SIZE) take = SHA256_DIGEST_SIZE;
        memcpy(out + done, pool, take);
        done += take;
    }
}

// ========== Передача записей ==========

// AAD для AES-GCM в TLS 1.2 (RFC 5246 §6.2.3.3): номер записи, тип,
// версия и длина ОТКРЫТОГО текста.
static void tls_build_aad(uint8_t aad[13], uint64_t seq, uint8_t type, uint16_t plain_len) {
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(seq >> (56 - i * 8));
    aad[8] = type;
    aad[9] = 3; aad[10] = 3;
    aad[11] = (uint8_t)(plain_len >> 8);
    aad[12] = (uint8_t)plain_len;
}

// Отправляет ОДНУ запись с указанным типом. До ChangeCipherSpec — как
// есть, после — зашифрованной по RFC 5288: payload это
// explicit_nonce(8) || ciphertext || tag(16), а сам nonce для GCM это
// client_write_IV(4) || explicit_nonce(8). В качестве явного nonce берётся
// номер записи — он гарантированно не повторяется в рамках соединения,
// чего GCM и требует (повтор nonce при одном ключе ломает GCM полностью).
static int tls_write_record(uint8_t type, const void *data, uint16_t len) {
    if (len > TLS_TX_PLAIN_MAX) return -1;

    g_tls.tx[0] = type;
    g_tls.tx[1] = 3;
    g_tls.tx[2] = 3;

    uint16_t payload_len;
    if (!g_tls.encrypt_active) {
        payload_len = len;
        if (len > 0 && data) memcpy(g_tls.tx + 5, data, len);
    } else {
        payload_len = (uint16_t)(TLS_EXPLICIT_NONCE_LEN + len + AES_GCM_TAG_SIZE);

        uint8_t nonce[AES_GCM_IV_SIZE];
        memcpy(nonce, g_tls.client_iv, TLS_FIXED_IV_LEN);
        for (int i = 0; i < 8; i++) {
            nonce[TLS_FIXED_IV_LEN + i] = (uint8_t)(g_tls.client_seq >> (56 - i * 8));
        }
        memcpy(g_tls.tx + 5, nonce + TLS_FIXED_IV_LEN, TLS_EXPLICIT_NONCE_LEN);

        uint8_t aad[13];
        tls_build_aad(aad, g_tls.client_seq, type, len);

        uint8_t *ct = g_tls.tx + 5 + TLS_EXPLICIT_NONCE_LEN;
        aes_gcm_encrypt(&g_tls.enc, nonce, aad, sizeof(aad),
                        (const uint8_t *)data, len, ct, ct + len);
    }

    g_tls.tx[3] = (uint8_t)(payload_len >> 8);
    g_tls.tx[4] = (uint8_t)payload_len;

    if (tcp_send(g_tls.tx, (uint16_t)(5 + payload_len)) != 0) {
        g_tls.fatal = 1;
        return -1;
    }
    if (g_tls.encrypt_active) g_tls.client_seq++;
    return 0;
}

// ========== Приём записей ==========

// Один опрос TCP с дописыванием пришедшего в накопитель g_tls.rx.
// 1 = что-то дописали, 0 = данных пока нет.
static int tls_rx_poll_once(void) {
    uint8_t *p; uint16_t n;
    if (!tcp_recv_poll(&p, &n)) return 0;

    if (n > 0) {
        if (g_tls.rx_len + n <= TLS_RX_CAP) {
            memcpy(g_tls.rx + g_tls.rx_len, p, n);
            g_tls.rx_len += n;
        } else {
            // Переполнение накопителя значит, что сервер прислал запись
            // больше объявленного TLS-максимума (или мы потеряли
            // синхронизацию с границами записей) — продолжать нельзя.
            DLOG("[TLS] rx buffer overflow (%u + %u)\n", g_tls.rx_len, (uint32_t)n);
            g_tls.fatal = 1;
        }
    }
    // ACK отправляем в любом случае: даже на пути ошибки оставлять TCP
    // без подтверждения незачем, соединение всё равно сейчас закроется.
    tcp_ack_consumed();
    return 1;
}

/*
 * Ждёт и возвращает одну ЦЕЛУЮ запись: out_type — ContentType,
 * out_payload/out_len — её содержимое (уже расшифрованное, если
 * шифрование приёма включено). Возвращает 1 = запись есть, 0 = вышел
 * бюджет или соединение закрылось (смотреть g_tls.closed), -1 = фатальная
 * ошибка (в т.ч. не сошёлся тег GCM).
 */
static int tls_read_record(uint32_t budget_ticks, uint8_t *out_type,
                           uint8_t **out_payload, uint32_t *out_len) {
    uint64_t deadline = pit_get_ticks() + budget_ticks;

    for (;;) {
        if (g_tls.fatal) return -1;

        if (g_tls.rx_len >= 5) {
            uint8_t type = g_tls.rx[0];
            uint32_t declared = ((uint32_t)g_tls.rx[3] << 8) | g_tls.rx[4];

            if (g_tls.rx[1] != 3 || declared > TLS_REC_CAP) {
                DLOG("[TLS] bad record header: ver %u.%u len %u\n",
                     g_tls.rx[1], g_tls.rx[2], declared);
                g_tls.fatal = 1;
                return -1;
            }

            if (g_tls.rx_len >= 5 + declared) {
                memcpy(g_tls.rec, g_tls.rx + 5, declared);
                g_tls.rec_len = declared;

                uint32_t consumed = 5 + declared;
                memmove(g_tls.rx, g_tls.rx + consumed, g_tls.rx_len - consumed);
                g_tls.rx_len -= consumed;

                if (g_tls.decrypt_active) {
                    if (g_tls.rec_len < TLS_EXPLICIT_NONCE_LEN + AES_GCM_TAG_SIZE) {
                        DLOG("[TLS] encrypted record too short (%u)\n", g_tls.rec_len);
                        g_tls.fatal = 1;
                        return -1;
                    }
                    uint32_t ct_len = g_tls.rec_len - TLS_EXPLICIT_NONCE_LEN - AES_GCM_TAG_SIZE;

                    uint8_t nonce[AES_GCM_IV_SIZE];
                    memcpy(nonce, g_tls.server_iv, TLS_FIXED_IV_LEN);
                    memcpy(nonce + TLS_FIXED_IV_LEN, g_tls.rec, TLS_EXPLICIT_NONCE_LEN);

                    uint8_t aad[13];
                    tls_build_aad(aad, g_tls.server_seq, type, (uint16_t)ct_len);

                    uint8_t *ct = g_tls.rec + TLS_EXPLICIT_NONCE_LEN;
                    if (aes_gcm_decrypt(&g_tls.dec, nonce, aad, sizeof(aad),
                                        ct, ct_len, ct, ct + ct_len) != 0) {
                        // Не сошёлся тег — запись подделана либо потеряна
                        // синхронизация номеров. По RFC это фатально
                        // всегда, без попыток восстановиться.
                        DLOG("[TLS] GCM tag mismatch on record type %u\n", type);
                        g_tls.fatal = 1;
                        return -1;
                    }
                    g_tls.server_seq++;

                    // Сдвигаем открытый текст в начало rec[], чтобы
                    // вызывающему не приходилось помнить про смещение
                    // явного nonce.
                    memmove(g_tls.rec, ct, ct_len);
                    g_tls.rec_len = ct_len;
                }

                *out_type = type;
                *out_payload = g_tls.rec;
                *out_len = g_tls.rec_len;
                return 1;
            }
        }

        if (!tls_rx_poll_once()) {
            // Данных нет И удалённая сторона прислала FIN — больше ничего
            // не придёт, дожидаться нечего.
            if (tcp_is_remote_closed()) {
                g_tls.closed = 1;
                return 0;
            }
            if (pit_get_ticks() >= deadline) return 0;
            pit_wait_ms(1);
        }
    }
}

// ========== Сборка handshake-сообщений ==========

// Достаёт из g_tls.hs следующее целое handshake-сообщение, если оно там
// уже целиком есть. Побочный эффект: домешивает его (вместе с 4-байтным
// заголовком) в хеш рукопожатия, предварительно сохранив снимок хеша ДО
// него — именно этот снимок нужен для проверки verify_data в Finished,
// которое само в свой же хеш не входит.
static int tls_hs_next(uint8_t *out_type, const uint8_t **out_body, uint32_t *out_len) {
    if (g_tls.hs_consume > 0) {
        memmove(g_tls.hs, g_tls.hs + g_tls.hs_consume, g_tls.hs_len - g_tls.hs_consume);
        g_tls.hs_len -= g_tls.hs_consume;
        g_tls.hs_consume = 0;
    }

    if (g_tls.hs_len < 4) return 0;
    uint32_t body_len = ((uint32_t)g_tls.hs[1] << 16) | ((uint32_t)g_tls.hs[2] << 8) | g_tls.hs[3];
    if (body_len > TLS_HS_CAP - 4) {
        DLOG("[TLS] handshake message too large (%u)\n", body_len);
        g_tls.fatal = 1;
        return -1;
    }
    if (g_tls.hs_len < 4 + body_len) return 0;

    g_tls.hs_hash_before = g_tls.hs_hash;
    sha256_update(&g_tls.hs_hash, g_tls.hs, 4 + body_len);

    *out_type = g_tls.hs[0];
    *out_body = g_tls.hs + 4;
    *out_len = body_len;
    // Освобождать место нельзя прямо сейчас: вызывающий ещё будет читать
    // тело по отданному указателю (разбор Certificate идёт по месту).
    g_tls.hs_consume = 4 + body_len;
    return 1;
}

// Ждёт следующее событие рукопожатия. 1 = пришло handshake-сообщение
// (type/body/len), 2 = пришла запись ChangeCipherSpec, 0 = бюджет
// вышел/соединение закрылось, -1 = ошибка (включая alert от сервера).
static int tls_next_handshake(uint32_t budget_ticks, uint8_t *out_type,
                              const uint8_t **out_body, uint32_t *out_len) {
    for (;;) {
        int r = tls_hs_next(out_type, out_body, out_len);
        if (r != 0) return r;

        uint8_t rec_type; uint8_t *payload; uint32_t payload_len;
        int rr = tls_read_record(budget_ticks, &rec_type, &payload, &payload_len);
        if (rr <= 0) return rr;

        if (rec_type == TLS_CT_HANDSHAKE) {
            if (g_tls.hs_len + payload_len > TLS_HS_CAP) {
                DLOG("[TLS] handshake reassembly overflow\n");
                g_tls.fatal = 1;
                return -1;
            }
            memcpy(g_tls.hs + g_tls.hs_len, payload, payload_len);
            g_tls.hs_len += payload_len;
            continue; // на следующей итерации tls_hs_next() попробует снова
        }

        if (rec_type == TLS_CT_CHANGE_CIPHER_SPEC) {
            if (payload_len != 1 || payload[0] != 1) {
                DLOG("[TLS] malformed ChangeCipherSpec\n");
                g_tls.fatal = 1;
                return -1;
            }
            return 2;
        }

        if (rec_type == TLS_CT_ALERT) {
            // Любой alert во время рукопожатия — конец: даже
            // close_notify здесь означает, что сервер нас не принял.
            DLOG("[TLS] alert during handshake: level %u desc %u\n",
                 payload_len > 0 ? payload[0] : 0,
                 payload_len > 1 ? payload[1] : 0);
            g_tls.fatal = 1;
            return -1;
        }

        // Прикладные данные до конца рукопожатия — нарушение протокола.
        DLOG("[TLS] unexpected record type %u during handshake\n", rec_type);
        g_tls.fatal = 1;
        return -1;
    }
}

// Отправляет handshake-сообщение (сама собирает 4-байтный заголовок) и
// домешивает его в хеш рукопожатия.
static int tls_send_handshake(uint8_t msg_type, const uint8_t *body, uint16_t body_len) {
    uint8_t header[4];
    header[0] = msg_type;
    header[1] = 0;
    header[2] = (uint8_t)(body_len >> 8);
    header[3] = (uint8_t)body_len;

    // Одно сообщение — одна запись; так как всё, что мы отправляем
    // (ClientHello/ClientKeyExchange/Finished), заведомо мелкое, собираем
    // заголовок и тело в один буфер и шлём разом.
    static uint8_t msg[4 + TLS_CLIENT_HELLO_CAP];
    if ((uint32_t)body_len + 4 > sizeof(msg)) return -1;
    memcpy(msg, header, 4);
    if (body_len > 0 && body) memcpy(msg + 4, body, body_len);

    sha256_update(&g_tls.hs_hash, msg, 4u + body_len);
    return tls_write_record(TLS_CT_HANDSHAKE, msg, (uint16_t)(4 + body_len));
}

// ========== ClientHello ==========

static uint32_t tls_cstr_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

// Собирает тело ClientHello в buf. Возвращает его длину или -1, если имя
// хоста настолько длинное, что не влезает (на практике невозможно).
static int tls_build_client_hello(uint8_t *buf, uint32_t cap, const char *sni_hostname) {
    uint32_t host_len = tls_cstr_len(sni_hostname);
    if (host_len == 0 || host_len > 255) return -1;
    // Фиксированная часть (41) + 4 расширения + SNI с именем. Проверяем
    // один раз здесь, дальше можно писать без проверок на каждый байт.
    if (41 + 2 + (9 + host_len) + 8 + 6 + 8 > cap) return -1;

    uint32_t p = 0;
    buf[p++] = 3; buf[p++] = 3;                  // client_version = TLS 1.2
    memcpy(buf + p, g_tls.client_random, 32); p += 32;
    buf[p++] = 0;                                 // session_id пустой (возобновления нет)
    buf[p++] = 0x00; buf[p++] = 0x02;             // cipher_suites: 2 байта
    buf[p++] = (uint8_t)(TLS_CIPHER_ECDHE_RSA_AES128_GCM_SHA256 >> 8);
    buf[p++] = (uint8_t)(TLS_CIPHER_ECDHE_RSA_AES128_GCM_SHA256);
    buf[p++] = 0x01; buf[p++] = 0x00;             // compression_methods: только null

    uint32_t ext_len_pos = p; p += 2;             // длину расширений заполним в конце
    uint32_t ext_start = p;

    // server_name (RFC 6066) — ОБЯЗАТЕЛЬНО: и GitHub, и Fastly выбирают и
    // маршрут, и сертификат по SNI; без него придёт чужой сертификат или
    // вообще отказ.
    buf[p++] = 0x00; buf[p++] = 0x00;
    uint32_t sni_body = 5 + host_len;
    buf[p++] = (uint8_t)(sni_body >> 8); buf[p++] = (uint8_t)sni_body;
    uint32_t list_len = 3 + host_len;
    buf[p++] = (uint8_t)(list_len >> 8); buf[p++] = (uint8_t)list_len;
    buf[p++] = 0x00;                              // name_type = host_name
    buf[p++] = (uint8_t)(host_len >> 8); buf[p++] = (uint8_t)host_len;
    memcpy(buf + p, sni_hostname, host_len); p += host_len;

    // supported_groups — только x25519 (единственная кривая, которую мы
    // умеем, см. crypto/x25519.h).
    buf[p++] = 0x00; buf[p++] = 0x0A;
    buf[p++] = 0x00; buf[p++] = 0x04;
    buf[p++] = 0x00; buf[p++] = 0x02;
    buf[p++] = (uint8_t)(TLS_GROUP_X25519 >> 8); buf[p++] = (uint8_t)TLS_GROUP_X25519;

    // ec_point_formats — только uncompressed. Для x25519 формат точки
    // вообще не применим, но многие серверы ожидают это расширение рядом
    // с supported_groups, и послать его дешевле, чем разбираться.
    buf[p++] = 0x00; buf[p++] = 0x0B;
    buf[p++] = 0x00; buf[p++] = 0x02;
    buf[p++] = 0x01; buf[p++] = 0x00;

    // signature_algorithms — РОВНО rsa_pkcs1_sha256 и ничего больше.
    // Перечислять то, что мы не умеем проверять, нельзя: сервер выберет
    // именно это и подпишет ServerKeyExchange алгоритмом, который мы не
    // осилим (см. rsa_pkcs1_sha256_verify()).
    buf[p++] = 0x00; buf[p++] = 0x0D;
    buf[p++] = 0x00; buf[p++] = 0x04;
    buf[p++] = 0x00; buf[p++] = 0x02;
    buf[p++] = (uint8_t)(TLS_SIGALG_RSA_PKCS1_SHA256 >> 8);
    buf[p++] = (uint8_t)TLS_SIGALG_RSA_PKCS1_SHA256;

    uint32_t ext_total = p - ext_start;
    buf[ext_len_pos] = (uint8_t)(ext_total >> 8);
    buf[ext_len_pos + 1] = (uint8_t)ext_total;

    return (int)p;
}

// ========== Разбор сообщений сервера ==========

static int tls_parse_server_hello(const uint8_t *body, uint32_t len) {
    // ServerHello: version(2) random(32) session_id_len(1) session_id
    // cipher_suite(2) compression(1) [extensions].
    if (len < 38) return -1;
    if (body[0] != 3 || body[1] != 3) {
        DLOG("[TLS] server picked version %u.%u, only 3.3 supported\n", body[0], body[1]);
        return -1;
    }
    memcpy(g_tls.server_random, body + 2, 32);

    uint32_t p = 34;
    uint32_t sid_len = body[p++];
    if (p + sid_len + 3 > len) return -1;
    p += sid_len; // сам session_id не нужен: возобновления сессий нет

    uint16_t suite = (uint16_t)(((uint16_t)body[p] << 8) | body[p + 1]);
    p += 2;
    if (suite != TLS_CIPHER_ECDHE_RSA_AES128_GCM_SHA256) {
        DLOG("[TLS] server picked cipher 0x%x, only 0xC02F supported\n", suite);
        return -1;
    }
    if (body[p] != 0) return -1; // compression обязан быть null

    // Расширения сервера сознательно не разбираются: ни одно из них нам
    // не нужно (ALPN не предлагали, тикеты не просили, renegotiation_info
    // ни на что не влияет без переустановки соединения).
    return 0;
}

// Достаёт открытый ключ из leaf-сертификата (первого в цепочке).
// Остальная цепочка не читается — проверять её всё равно нечем, см.
// раздел про доверие в начале файла.
static int tls_parse_certificate(const uint8_t *body, uint32_t len) {
    if (len < 6) return -1;
    uint32_t list_len = ((uint32_t)body[0] << 16) | ((uint32_t)body[1] << 8) | body[2];
    if (list_len + 3 > len) return -1;

    uint32_t cert_len = ((uint32_t)body[3] << 16) | ((uint32_t)body[4] << 8) | body[5];
    if (cert_len == 0 || cert_len + 6 > len) return -1;

    rsa_public_key_t key;
    if (x509_extract_rsa_pubkey(body + 6, cert_len, &key) != 0) {
        DLOG("[TLS] cannot extract RSA public key from leaf certificate\n");
        return -1;
    }
    if (key.modulus_len > sizeof(g_tls.cert_modulus) ||
        key.exponent_len > sizeof(g_tls.cert_exponent)) {
        return -1;
    }

    // Копируем, пока байты сертификата ещё живы в hs[] — см. комментарий
    // у полей cert_* в tls_state_t.
    memcpy(g_tls.cert_modulus, key.modulus, key.modulus_len);
    g_tls.cert_modulus_len = key.modulus_len;
    memcpy(g_tls.cert_exponent, key.exponent, key.exponent_len);
    g_tls.cert_exponent_len = key.exponent_len;

    DLOG("[TLS] leaf cert RSA key: %u-bit modulus\n", key.modulus_len * 8);
    return 0;
}

/*
 * ServerKeyExchange для ECDHE (RFC 4492 §5.4) в TLS 1.2:
 *   ServerECDHParams: curve_type(1)=3 named_curve(2) public_len(1) public
 *   SignatureAndHashAlgorithm(2)  signature_len(2)  signature
 * Подпись покрывает client_random || server_random || ServerECDHParams —
 * именно это связывает эфемерный ключ с сертификатом и не даёт подменить
 * параметры ECDHE на лету.
 */
static int tls_parse_server_key_exchange(const uint8_t *body, uint32_t len,
                                         uint8_t out_server_pub[X25519_KEY_SIZE]) {
    if (len < 4) return -1;
    if (body[0] != 3) { // 3 == named_curve
        DLOG("[TLS] unsupported ECDH curve_type %u\n", body[0]);
        return -1;
    }
    uint16_t group = (uint16_t)(((uint16_t)body[1] << 8) | body[2]);
    if (group != TLS_GROUP_X25519) {
        DLOG("[TLS] server picked group 0x%x, only x25519 supported\n", group);
        return -1;
    }
    uint32_t pub_len = body[3];
    if (pub_len != X25519_KEY_SIZE || 4 + pub_len + 4 > len) return -1;

    uint32_t params_len = 4 + pub_len; // ровно ServerECDHParams, подписываемая часть
    memcpy(out_server_pub, body + 4, X25519_KEY_SIZE);

    uint32_t p = params_len;
    uint16_t sigalg = (uint16_t)(((uint16_t)body[p] << 8) | body[p + 1]);
    p += 2;
    if (sigalg != TLS_SIGALG_RSA_PKCS1_SHA256) {
        DLOG("[TLS] server signed with alg 0x%x, only 0x0401 supported\n", sigalg);
        return -1;
    }
    uint32_t sig_len = ((uint32_t)body[p] << 8) | body[p + 1];
    p += 2;
    if (p + sig_len != len) return -1; // подпись обязана занять весь остаток сообщения

    // Подписанные данные склеиваем в один буфер: 32 + 32 + ServerECDHParams.
    uint8_t signed_data[32 + 32 + 4 + X25519_KEY_SIZE];
    memcpy(signed_data, g_tls.client_random, 32);
    memcpy(signed_data + 32, g_tls.server_random, 32);
    memcpy(signed_data + 64, body, params_len);

    rsa_public_key_t key;
    key.modulus = g_tls.cert_modulus;
    key.modulus_len = g_tls.cert_modulus_len;
    key.exponent = g_tls.cert_exponent;
    key.exponent_len = g_tls.cert_exponent_len;

    if (rsa_pkcs1_sha256_verify(&key, body + p, sig_len, signed_data, 64 + params_len) != 0) {
        // ЭТО ЕДИНСТВЕННАЯ проверка аутентичности сервера, которая здесь
        // вообще есть (см. раздел про доверие вверху файла) — молча
        // продолжить нельзя ни при каких условиях.
        DLOG("[TLS] ServerKeyExchange signature verification FAILED\n");
        return -1;
    }
    DLOG("[TLS] ServerKeyExchange signature OK\n");
    return 0;
}

// ========== Вывод ключей ==========

static void tls_derive_keys(const uint8_t premaster[X25519_KEY_SIZE]) {
    // master_secret = PRF(premaster, "master secret", client_random || server_random)
    uint8_t seed[64];
    memcpy(seed, g_tls.client_random, 32);
    memcpy(seed + 32, g_tls.server_random, 32);
    tls12_prf_sha256(premaster, X25519_KEY_SIZE, "master secret", seed, sizeof(seed),
                     g_tls.master_secret, TLS_MASTER_SECRET_LEN);

    // key_block = PRF(master_secret, "key expansion", server_random || client_random)
    // — порядок случайных чисел здесь ОБРАТНЫЙ относительно
    // master_secret, это не опечатка, а RFC 5246 §6.3.
    uint8_t key_seed[64];
    memcpy(key_seed, g_tls.server_random, 32);
    memcpy(key_seed + 32, g_tls.client_random, 32);

    uint8_t key_block[TLS_KEY_BLOCK_LEN];
    tls12_prf_sha256(g_tls.master_secret, TLS_MASTER_SECRET_LEN, "key expansion",
                     key_seed, sizeof(key_seed), key_block, sizeof(key_block));

    aes_gcm_init(&g_tls.enc, key_block);
    aes_gcm_init(&g_tls.dec, key_block + 16);
    memcpy(g_tls.client_iv, key_block + 32, TLS_FIXED_IV_LEN);
    memcpy(g_tls.server_iv, key_block + 36, TLS_FIXED_IV_LEN);

    memset(key_block, 0, sizeof(key_block));
}

// verify_data = PRF(master_secret, label, Hash(handshake_messages))[0..11].
// hash_state — снимок хеша рукопожатия на нужный момент (он продолжает
// накапливаться дальше, поэтому финализируется КОПИЯ).
static void tls_compute_verify_data(const sha256_ctx_t *hash_state, const char *label,
                                    uint8_t out[TLS_VERIFY_DATA_LEN]) {
    sha256_ctx_t copy = *hash_state;
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_final(&copy, digest);
    tls12_prf_sha256(g_tls.master_secret, TLS_MASTER_SECRET_LEN, label,
                     digest, sizeof(digest), out, TLS_VERIFY_DATA_LEN);
}

// ========== Рукопожатие ==========

static void tls_reset_state(void) {
    // Полная очистка: соединение единственное, и остатки ключей/счётчиков
    // от предыдущего не должны влиять на новое.
    memset(&g_tls, 0, sizeof(g_tls));
    sha256_init(&g_tls.hs_hash);
}

int tls_connect(uint32_t ip, uint16_t port, const char *sni_hostname) {
    if (!rtl8139_found() || !sni_hostname) return -1;

    tls_reset_state();

    if (tcp_connect(ip, port) != 0) {
        DLOG("[TLS] TCP connect failed\n");
        return -1;
    }

    uint64_t hs_deadline = pit_get_ticks() + TLS_HANDSHAKE_BUDGET_TICKS;

    // --- ClientHello ---
    tls_gather_random(g_tls.client_random, 32);
    tls_gather_random(g_tls.client_priv, X25519_KEY_SIZE);
    x25519_base(g_tls.client_pub, g_tls.client_priv);

    uint8_t hello[TLS_CLIENT_HELLO_CAP];
    int hello_len = tls_build_client_hello(hello, sizeof(hello), sni_hostname);
    if (hello_len < 0 ||
        tls_send_handshake(TLS_HS_CLIENT_HELLO, hello, (uint16_t)hello_len) != 0) {
        DLOG("[TLS] failed to send ClientHello\n");
        tcp_close();
        return -1;
    }

    // --- ServerHello .. ServerHelloDone ---
    uint8_t server_pub[X25519_KEY_SIZE];
    int got_hello = 0, got_cert = 0, got_ske = 0, got_done = 0;

    while (!got_done) {
        if (pit_get_ticks() >= hs_deadline) {
            DLOG("[TLS] handshake budget exhausted waiting for ServerHelloDone\n");
            tcp_close();
            return -1;
        }

        uint8_t msg_type; const uint8_t *body; uint32_t body_len;
        int r = tls_next_handshake(TLS_RECORD_BUDGET_TICKS, &msg_type, &body, &body_len);
        if (r != 1) { // 0 = таймаут/закрытие, 2 = CCS раньше времени, -1 = ошибка
            DLOG("[TLS] handshake read failed (r=%d)\n", r);
            tcp_close();
            return -1;
        }

        switch (msg_type) {
        case TLS_HS_SERVER_HELLO:
            if (got_hello || tls_parse_server_hello(body, body_len) != 0) { tcp_close(); return -1; }
            got_hello = 1;
            break;

        case TLS_HS_CERTIFICATE:
            if (!got_hello || got_cert ||
                tls_parse_certificate(body, body_len) != 0) { tcp_close(); return -1; }
            got_cert = 1;
            break;

        case TLS_HS_SERVER_KEY_EXCHANGE:
            if (!got_cert || got_ske ||
                tls_parse_server_key_exchange(body, body_len, server_pub) != 0) {
                tcp_close();
                return -1;
            }
            got_ske = 1;
            break;

        case TLS_HS_SERVER_HELLO_DONE:
            if (!got_ske) { tcp_close(); return -1; } // без проверенной подписи дальше идти нельзя
            got_done = 1;
            break;

        case TLS_HS_CERTIFICATE_REQUEST:
            // Клиентских сертификатов у нас нет и быть не может (см.
            // объём вверху файла) — отвечать пустым Certificate и
            // надеяться, что сервер не обязателен, смысла нет.
            DLOG("[TLS] server requested a client certificate - unsupported\n");
            tcp_close();
            return -1;

        default:
            DLOG("[TLS] unexpected handshake message %u\n", msg_type);
            tcp_close();
            return -1;
        }
    }

    // --- ECDHE: общий секрет ---
    uint8_t premaster[X25519_KEY_SIZE];
    x25519_scalarmult(premaster, g_tls.client_priv, server_pub);

    // Нулевой общий секрет означает, что сервер прислал точку малого
    // порядка — по RFC 7748 §6.1 такое соединение обязано обрываться
    // (иначе секрет предсказуем независимо от нашего скаляра).
    uint8_t zero_check = 0;
    for (int i = 0; i < X25519_KEY_SIZE; i++) zero_check |= premaster[i];
    if (zero_check == 0) {
        DLOG("[TLS] X25519 shared secret is all-zero (low-order point)\n");
        tcp_close();
        return -1;
    }

    // --- ClientKeyExchange ---
    uint8_t cke[1 + X25519_KEY_SIZE];
    cke[0] = X25519_KEY_SIZE;
    memcpy(cke + 1, g_tls.client_pub, X25519_KEY_SIZE);
    if (tls_send_handshake(TLS_HS_CLIENT_KEY_EXCHANGE, cke, sizeof(cke)) != 0) {
        tcp_close();
        return -1;
    }

    tls_derive_keys(premaster);
    memset(premaster, 0, sizeof(premaster));
    memset(g_tls.client_priv, 0, sizeof(g_tls.client_priv));

    // --- ChangeCipherSpec + Finished (наши) ---
    uint8_t ccs = 1;
    if (tls_write_record(TLS_CT_CHANGE_CIPHER_SPEC, &ccs, 1) != 0) { tcp_close(); return -1; }
    g_tls.encrypt_active = 1;
    g_tls.client_seq = 0;

    uint8_t client_verify[TLS_VERIFY_DATA_LEN];
    tls_compute_verify_data(&g_tls.hs_hash, "client finished", client_verify);
    if (tls_send_handshake(TLS_HS_FINISHED, client_verify, TLS_VERIFY_DATA_LEN) != 0) {
        tcp_close();
        return -1;
    }

    // --- ChangeCipherSpec + Finished (сервера) ---
    int server_ccs_seen = 0;
    for (;;) {
        if (pit_get_ticks() >= hs_deadline) {
            DLOG("[TLS] handshake budget exhausted waiting for server Finished\n");
            tcp_close();
            return -1;
        }

        uint8_t msg_type; const uint8_t *body; uint32_t body_len;
        int r = tls_next_handshake(TLS_RECORD_BUDGET_TICKS, &msg_type, &body, &body_len);

        if (r == 2) { // ChangeCipherSpec: с этого момента приём расшифровывается
            if (server_ccs_seen) { tcp_close(); return -1; }
            server_ccs_seen = 1;
            g_tls.decrypt_active = 1;
            g_tls.server_seq = 0;
            continue;
        }
        if (r != 1) {
            DLOG("[TLS] failed waiting for server Finished (r=%d)\n", r);
            tcp_close();
            return -1;
        }

        if (msg_type == TLS_HS_NEW_SESSION_TICKET) {
            // Мы тикетов не просили, но если сервер прислал — просто
            // игнорируем содержимое. В хеш рукопожатия оно при этом
            // ПОПАДАЕТ (это сделал tls_hs_next()), как и требует RFC 5077.
            DLOG("[TLS] ignoring NewSessionTicket (%u bytes)\n", body_len);
            continue;
        }

        if (msg_type != TLS_HS_FINISHED || !server_ccs_seen) {
            DLOG("[TLS] expected Finished, got %u (ccs_seen=%d)\n", msg_type, server_ccs_seen);
            tcp_close();
            return -1;
        }

        // verify_data сервера считается по хешу ВСЕХ предыдущих
        // handshake-сообщений, но БЕЗ самого Finished — для этого
        // tls_hs_next() и сохранил снимок hs_hash_before.
        uint8_t expected[TLS_VERIFY_DATA_LEN];
        tls_compute_verify_data(&g_tls.hs_hash_before, "server finished", expected);

        if (body_len != TLS_VERIFY_DATA_LEN) { tcp_close(); return -1; }
        uint8_t diff = 0;
        for (int i = 0; i < TLS_VERIFY_DATA_LEN; i++) diff |= (uint8_t)(body[i] ^ expected[i]);
        if (diff != 0) {
            DLOG("[TLS] server Finished verify_data mismatch\n");
            tcp_close();
            return -1;
        }
        break;
    }

    // Буфер сборки handshake-сообщений больше не нужен — освобождаем его
    // под возможные (неподдерживаемые) handshake-записи в рабочей фазе.
    g_tls.hs_len = 0;
    g_tls.hs_consume = 0;

    g_tls.active = 1;
    DLOG("[TLS] handshake complete with %s\n", sni_hostname);
    return 0;
}

// ========== Рабочая фаза ==========

int tls_send(const void *data, uint16_t len) {
    if (!g_tls.active || g_tls.fatal) return -1;
    if (len == 0) return 0;
    if (!data) return -1;

    const uint8_t *p = (const uint8_t *)data;
    uint16_t remaining = len;
    while (remaining > 0) {
        uint16_t chunk = remaining > TLS_TX_PLAIN_MAX ? (uint16_t)TLS_TX_PLAIN_MAX : remaining;
        if (tls_write_record(TLS_CT_APPLICATION_DATA, p, chunk) != 0) return -1;
        p += chunk;
        remaining = (uint16_t)(remaining - chunk);
    }
    return 0;
}

int tls_recv(uint8_t *buf, uint16_t maxlen) {
    if (!g_tls.active || !buf || maxlen == 0) return -1;

    for (;;) {
        // Сначала — остаток предыдущей записи (она могла не влезть в
        // maxlen вызывающего; это и есть "короткое чтение").
        if (g_tls.app_off < g_tls.app_len) {
            uint32_t avail = g_tls.app_len - g_tls.app_off;
            uint32_t take = (avail > maxlen) ? maxlen : avail;
            memcpy(buf, g_tls.rec + g_tls.app_off, take);
            g_tls.app_off += take;
            return (int)take;
        }

        if (g_tls.fatal) return -1;
        if (g_tls.closed) return 0;

        uint8_t type; uint8_t *payload; uint32_t payload_len;
        int r = tls_read_record(TLS_RECORD_BUDGET_TICKS, &type, &payload, &payload_len);
        if (r < 0) return -1;
        if (r == 0) {
            // Либо FIN (closed уже выставлен в tls_read_record()), либо
            // сервер замолчал на весь бюджет — второе для нашего сценария
            // (ответ на один GET с Connection: close) это ошибка.
            return g_tls.closed ? 0 : -1;
        }

        if (type == TLS_CT_APPLICATION_DATA) {
            g_tls.app_off = 0;
            g_tls.app_len = payload_len;
            continue; // отдадим на следующем витке, через общий код выше
        }

        if (type == TLS_CT_ALERT) {
            if (payload_len >= 2 && payload[1] == TLS_ALERT_CLOSE_NOTIFY) {
                g_tls.closed = 1;
                return 0; // корректное закрытие: это НЕ ошибка
            }
            DLOG("[TLS] fatal alert: level %u desc %u\n",
                 payload_len > 0 ? payload[0] : 0,
                 payload_len > 1 ? payload[1] : 0);
            g_tls.fatal = 1;
            return -1;
        }

        // Handshake-запись в рабочей фазе — это запрос на переустановку
        // соединения (renegotiation), которую мы не поддерживаем; всё
        // остальное тем более не ожидается.
        DLOG("[TLS] unexpected record type %u after handshake\n", type);
        g_tls.fatal = 1;
        return -1;
    }
}

void tls_close(void) {
    // close_notify — best-effort: если соединение уже сломано, слать
    // нечего, но tcp_close() позвать всё равно надо.
    if (g_tls.active && !g_tls.fatal && g_tls.encrypt_active) {
        uint8_t alert[2] = { 1, TLS_ALERT_CLOSE_NOTIFY }; // level = warning
        tls_write_record(TLS_CT_ALERT, alert, sizeof(alert));
    }
    tcp_close();
    memset(&g_tls, 0, sizeof(g_tls));
}
