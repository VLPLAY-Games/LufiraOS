#pragma once

#include "lib/types.h"

/*
 * SHA-256 (FIPS 180-4) — своя реализация, потому что ядро freestanding и
 * никакой libc/libcrypto тут нет вообще. Нужна целиком ради TLS 1.2
 * (net/tls.c): хеш рукопожатия (Finished), HMAC-SHA256 (crypto/hmac.c) и
 * через него PRF TLS 1.2, а также digest под проверку подписи
 * ServerKeyExchange (RSA PKCS#1 v1.5, см. tls.c).
 *
 * Файл намеренно НЕ зависит ни от чего кроме memcpy/memset (lib/string.h) —
 * тот же исходник собирается и внутрь ядра, и обычным хостовым gcc в
 * crypto/selftest/ для прогона контрольных векторов (см. selftest/README).
 */

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

typedef struct {
    uint32_t state[8];
    uint64_t total_len;                  // всего байт, скормленных update()
    uint8_t  block[SHA256_BLOCK_SIZE];   // недобранный до 64 байт хвост
    uint32_t block_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);

// Завершает хеш. Контекст после этого использовать нельзя (padding уже
// вмешан) — если нужен "снимок" промежуточного состояния (именно так
// tls.c берёт хеш рукопожатия, не прерывая его накопление), контекст
// копируется присваиванием структуры, и final() зовётся на копии.
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

// Разовый хеш одного буфера.
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);
