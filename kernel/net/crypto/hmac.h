#pragma once

#include "lib/types.h"
#include "sha256.h"

/*
 * HMAC-SHA256 (RFC 2104) и построенный на нём PRF TLS 1.2 (RFC 5246 §5) —
 * ровно то, что нужно net/tls.c для вывода master_secret, key_block и
 * verify_data сообщений Finished. Других применений (и других хешей под
 * HMAC) в ядре нет, поэтому реализация жёстко привязана к SHA-256, а не
 * параметризована абстрактным "дайджестом" — см. тот же принцип
 * "ровно необходимый минимум" в tcp.c/dns.c.
 */

typedef struct {
    sha256_ctx_t inner;
    sha256_ctx_t outer;
} hmac_sha256_ctx_t;

void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const void *key, size_t key_len);
void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const void *data, size_t len);
void hmac_sha256_final(hmac_sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

// Разовый HMAC одного буфера.
void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t len,
                 uint8_t out[SHA256_DIGEST_SIZE]);

// PRF TLS 1.2 (RFC 5246 §5): P_SHA256(secret, label || seed), развёрнутый
// до out_len байт. label — обычная С-строка без завершающего нуля в
// вычислении ("master secret", "key expansion", "client finished",
// "server finished"); seed вызывающий склеивает сам (обычно
// client_random || server_random в том или другом порядке — порядок
// РАЗНЫЙ для master_secret и key_block, см. tls.c).
void tls12_prf_sha256(const void *secret, size_t secret_len,
                      const char *label,
                      const void *seed, size_t seed_len,
                      uint8_t *out, size_t out_len);
