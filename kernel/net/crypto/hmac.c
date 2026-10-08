#include "hmac.h"
#include "lib/string.h"

// Локальная strlen для label'ов PRF: lib/string.h её объявляет, но этот
// файл должен собираться и хостовым gcc в selftest/ без остального ядра,
// а тащить туда весь lib/string.c незачем — label'ы тут всегда короткие
// литералы из tls.c.
static size_t hmac_label_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const void *key, size_t key_len) {
    uint8_t k[SHA256_BLOCK_SIZE];
    uint8_t pad[SHA256_BLOCK_SIZE];

    // Ключ длиннее блока хешируется (RFC 2104 §2), короче — добивается
    // нулями справа.
    memset(k, 0, sizeof(k));
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, k);
    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }

    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) pad[i] = (uint8_t)(k[i] ^ 0x36); // ipad
    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, pad, SHA256_BLOCK_SIZE);

    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) pad[i] = (uint8_t)(k[i] ^ 0x5c); // opad
    sha256_init(&ctx->outer);
    sha256_update(&ctx->outer, pad, SHA256_BLOCK_SIZE);

    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
}

void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const void *data, size_t len) {
    sha256_update(&ctx->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]) {
    uint8_t inner_digest[SHA256_DIGEST_SIZE];
    sha256_final(&ctx->inner, inner_digest);
    sha256_update(&ctx->outer, inner_digest, SHA256_DIGEST_SIZE);
    sha256_final(&ctx->outer, out);
}

void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t len,
                 uint8_t out[SHA256_DIGEST_SIZE]) {
    hmac_sha256_ctx_t ctx;
    hmac_sha256_init(&ctx, key, key_len);
    hmac_sha256_update(&ctx, data, len);
    hmac_sha256_final(&ctx, out);
}

void tls12_prf_sha256(const void *secret, size_t secret_len,
                      const char *label,
                      const void *seed, size_t seed_len,
                      uint8_t *out, size_t out_len) {
    size_t label_len = hmac_label_len(label);

    // P_hash(secret, seed) = HMAC(secret, A(1)||seed) + HMAC(secret, A(2)||seed) + ...
    // где A(0) = seed, A(i) = HMAC(secret, A(i-1)), а "seed" для TLS это
    // label || настоящий seed (RFC 5246 §5). A(i) всегда ровно размер
    // дайджеста, поэтому буфер фиксированный.
    uint8_t a[SHA256_DIGEST_SIZE];
    hmac_sha256_ctx_t ctx;

    // A(1) = HMAC(secret, label || seed)
    hmac_sha256_init(&ctx, secret, secret_len);
    hmac_sha256_update(&ctx, label, label_len);
    hmac_sha256_update(&ctx, seed, seed_len);
    hmac_sha256_final(&ctx, a);

    size_t done = 0;
    while (done < out_len) {
        uint8_t block[SHA256_DIGEST_SIZE];

        hmac_sha256_init(&ctx, secret, secret_len);
        hmac_sha256_update(&ctx, a, SHA256_DIGEST_SIZE);
        hmac_sha256_update(&ctx, label, label_len);
        hmac_sha256_update(&ctx, seed, seed_len);
        hmac_sha256_final(&ctx, block);

        size_t take = out_len - done;
        if (take > SHA256_DIGEST_SIZE) take = SHA256_DIGEST_SIZE;
        memcpy(out + done, block, take);
        done += take;

        if (done < out_len) { // A(i+1) нужен только если будет следующий блок
            hmac_sha256(secret, secret_len, a, SHA256_DIGEST_SIZE, a);
        }
    }
}
