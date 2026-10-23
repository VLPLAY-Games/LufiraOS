#pragma once

#include "lib/types.h"

/*
 * SHA-256 (FIPS 180-4) — перенос lufira-packages/daemons/netd/crypto/
 * sha256.c в ядро для users.c (хэш пароля вместо FNV-1a — см. комментарий
 * там). Та же реализация, зависит только от memcpy/memset (lib/string.h).
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
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

// Разовый хеш одного буфера.
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);
