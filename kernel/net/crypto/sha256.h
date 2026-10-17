#pragma once

#include "lib/types.h"

/*
 * SHA-256 (FIPS 180-4) — своя реализация: ядро freestanding, libc/libcrypto
 * нет. Нужна для TLS 1.2 (net/tls.c): хеш рукопожатия (Finished),
 * HMAC-SHA256 (crypto/hmac.c) и через него PRF, digest для проверки подписи
 * ServerKeyExchange (RSA PKCS#1 v1.5).
 *
 * Зависит только от memcpy/memset (lib/string.h) — тот же исходник собирается
 * и в ядро, и хостовым gcc в crypto/selftest/ для контрольных векторов.
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

// Завершает хеш; контекст после этого не использовать (padding уже вмешан).
// Для "снимка" промежуточного состояния (так tls.c берёт хеш рукопожатия без
// прерывания накопления) — копировать контекст присваиванием и звать final() на копии.
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

// Разовый хеш одного буфера.
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);
