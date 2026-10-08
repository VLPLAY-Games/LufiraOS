#include "aes_gcm.h"
#include "lib/string.h"

// ========== AES-128 (только шифрующее направление, см. aes_gcm.h) ==========

static const uint8_t AES_SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

// Rcon для расширения ключа (x^(i-1) в GF(2^8)); для AES-128 нужны ровно 10.
static const uint8_t AES_RCON[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

// Умножение на x в GF(2^8) по модулю x^8+x^4+x^3+x+1 (0x11b).
static inline uint8_t xtime(uint8_t b) {
    return (uint8_t)((b << 1) ^ ((b & 0x80) ? 0x1b : 0x00));
}

// Состояние AES здесь хранится как 16 байт "по столбцам": s[4*c + r] это
// элемент state[r][c] из FIPS 197 — ровно тот же порядок, в котором байты
// лежат во входном/выходном блоке, так что загрузка/выгрузка это memcpy.
static void aes_add_round_key(uint8_t s[16], const uint32_t *w) {
    for (int c = 0; c < 4; c++) {
        uint32_t k = w[c];
        s[4 * c + 0] ^= (uint8_t)(k >> 24);
        s[4 * c + 1] ^= (uint8_t)(k >> 16);
        s[4 * c + 2] ^= (uint8_t)(k >> 8);
        s[4 * c + 3] ^= (uint8_t)(k);
    }
}

static void aes_sub_bytes(uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];
}

// Строка r циклически сдвигается влево на r (строка 0 — не сдвигается).
// В раскладке "по столбцам" элементы строки r это s[r], s[4+r], s[8+r], s[12+r].
static void aes_shift_rows(uint8_t s[16]) {
    uint8_t t;
    t = s[1];  s[1] = s[5];  s[5] = s[9];   s[9] = s[13];  s[13] = t;   // влево на 1
    t = s[2];  s[2] = s[10]; s[10] = t;                                  // влево на 2
    t = s[6];  s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3];   s[3] = t;    // влево на 3
}

static void aes_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t a0 = s[4 * c + 0], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
        s[4 * c + 0] = (uint8_t)(xtime(a0) ^ (uint8_t)(xtime(a1) ^ a1) ^ a2 ^ a3);
        s[4 * c + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (uint8_t)(xtime(a2) ^ a2) ^ a3);
        s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (uint8_t)(xtime(a3) ^ a3));
        s[4 * c + 3] = (uint8_t)((uint8_t)(xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
}

static void aes_encrypt_block(const uint32_t *rk, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);

    aes_add_round_key(s, rk);
    for (int round = 1; round < 10; round++) {
        aes_sub_bytes(s);
        aes_shift_rows(s);
        aes_mix_columns(s);
        aes_add_round_key(s, rk + round * 4);
    }
    // Последний раунд — без MixColumns (FIPS 197 §5.1).
    aes_sub_bytes(s);
    aes_shift_rows(s);
    aes_add_round_key(s, rk + 40);

    memcpy(out, s, 16);
}

static void aes128_expand_key(const uint8_t key[16], uint32_t w[44]) {
    for (int i = 0; i < 4; i++) {
        w[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
               ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];
    }
    for (int i = 4; i < 44; i++) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24); // RotWord
            t = ((uint32_t)AES_SBOX[(t >> 24) & 0xFF] << 24) |  // SubWord
                ((uint32_t)AES_SBOX[(t >> 16) & 0xFF] << 16) |
                ((uint32_t)AES_SBOX[(t >> 8) & 0xFF] << 8) |
                ((uint32_t)AES_SBOX[t & 0xFF]);
            t ^= (uint32_t)AES_RCON[i / 4 - 1] << 24;
        }
        w[i] = w[i - 4] ^ t;
    }
}

// ========== GHASH (NIST SP 800-38D §6.3-6.4) ==========

// y = y * H в GF(2^128). Таблица h_bits заранее содержит H, сдвинутый на
// 0..127 бит с приведением (см. комментарий в aes_gcm.h) — остаётся
// сложить (XOR) те её строки, которым соответствуют единичные биты y,
// перебираемые СТАРШИМ БИТОМ ПЕРВОГО БАЙТА вперёд (порядок бит в GCM).
static void ghash_mul(const aes_gcm_ctx_t *ctx, uint8_t y[16]) {
    uint8_t z[16];
    memset(z, 0, 16);

    for (int i = 0; i < 128; i++) {
        if ((y[i >> 3] >> (7 - (i & 7))) & 1) {
            const uint8_t *v = ctx->h_bits[i];
            for (int j = 0; j < 16; j++) z[j] ^= v[j];
        }
    }
    memcpy(y, z, 16);
}

// Домешивает len байт в хеш GHASH, добивая последний неполный блок нулями
// (тот самый padding 0^v/0^u из определения S, SP 800-38D §6.5 шаг 5).
static void ghash_update(const aes_gcm_ctx_t *ctx, uint8_t y[16],
                         const uint8_t *data, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = len - off;
        if (n > 16) n = 16;
        for (uint32_t j = 0; j < n; j++) y[j] ^= data[off + j];
        ghash_mul(ctx, y);
        off += n;
    }
}

// ========== Общая часть encrypt/decrypt ==========

static void gcm_inc32(uint8_t counter[16]) {
    // Инкрементируются ТОЛЬКО младшие 4 байта, с переносом внутри них и
    // без переноса в IV-часть (inc32 из SP 800-38D §6.2).
    for (int i = 15; i >= 12; i--) {
        if (++counter[i] != 0) break;
    }
}

// GCTR: XOR данных с потоком AES_K(counter), counter инкрементируется
// inc32 на каждый блок. out может совпадать с in.
static void gcm_ctr(const aes_gcm_ctx_t *ctx, uint8_t counter[16],
                    const uint8_t *in, uint32_t len, uint8_t *out) {
    uint8_t ks[16];
    uint32_t off = 0;

    while (off < len) {
        aes_encrypt_block(ctx->round_keys, counter, ks);
        gcm_inc32(counter);

        uint32_t n = len - off;
        if (n > 16) n = 16;
        for (uint32_t j = 0; j < n; j++) out[off + j] = (uint8_t)(in[off + j] ^ ks[j]);
        off += n;
    }
}

// S = GHASH_H(A || 0^v || C || 0^u || [len(A)]_64 || [len(C)]_64), затем
// T = GCTR(J0, S) (SP 800-38D §7.1 шаги 5-6). Длины — в БИТАХ, big-endian.
static void gcm_tag(const aes_gcm_ctx_t *ctx, const uint8_t j0[16],
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *ciphertext, uint32_t len,
                    uint8_t tag[AES_GCM_TAG_SIZE]) {
    uint8_t s[16];
    memset(s, 0, 16);

    ghash_update(ctx, s, aad, aad_len);
    ghash_update(ctx, s, ciphertext, len);

    uint8_t lengths[16];
    uint64_t aad_bits = (uint64_t)aad_len * 8u;
    uint64_t txt_bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++) {
        lengths[i]     = (uint8_t)(aad_bits >> (56 - i * 8));
        lengths[8 + i] = (uint8_t)(txt_bits >> (56 - i * 8));
    }
    ghash_update(ctx, s, lengths, 16);

    uint8_t counter[16];
    memcpy(counter, j0, 16);
    gcm_ctr(ctx, counter, s, 16, tag);
}

void aes_gcm_init(aes_gcm_ctx_t *ctx, const uint8_t key[16]) {
    aes128_expand_key(key, ctx->round_keys);

    // H = AES_K(0^128), затем 128 его последовательных сдвигов вправо с
    // приведением по R = 11100001||0^120 — см. комментарий у h_bits.
    uint8_t zero[16];
    memset(zero, 0, 16);
    aes_encrypt_block(ctx->round_keys, zero, ctx->h_bits[0]);

    for (int i = 1; i < 128; i++) {
        const uint8_t *prev = ctx->h_bits[i - 1];
        uint8_t *cur = ctx->h_bits[i];
        int lsb = prev[15] & 1;
        for (int j = 15; j > 0; j--) {
            cur[j] = (uint8_t)((prev[j] >> 1) | ((prev[j - 1] & 1) << 7));
        }
        cur[0] = (uint8_t)(prev[0] >> 1);
        if (lsb) cur[0] ^= 0xe1;
    }
}

// J0 для 12-байтного IV — это IV || 0^31 || 1 (SP 800-38D §7.1 шаг 2,
// случай len(IV) == 96 бит; другие длины IV TLS 1.2 не использует и мы их
// не поддерживаем, см. aes_gcm.h).
static void gcm_build_j0(const uint8_t iv[AES_GCM_IV_SIZE], uint8_t j0[16]) {
    memcpy(j0, iv, AES_GCM_IV_SIZE);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
}

void aes_gcm_encrypt(aes_gcm_ctx_t *ctx, const uint8_t iv[AES_GCM_IV_SIZE],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *plaintext, uint32_t len,
                     uint8_t *ciphertext, uint8_t tag[AES_GCM_TAG_SIZE]) {
    uint8_t j0[16], counter[16];
    gcm_build_j0(iv, j0);

    memcpy(counter, j0, 16);
    gcm_inc32(counter); // поток шифрования начинается с inc32(J0), сам J0 — только для тега
    gcm_ctr(ctx, counter, plaintext, len, ciphertext);

    gcm_tag(ctx, j0, aad, aad_len, ciphertext, len, tag);
}

int aes_gcm_decrypt(aes_gcm_ctx_t *ctx, const uint8_t iv[AES_GCM_IV_SIZE],
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *ciphertext, uint32_t len,
                    uint8_t *plaintext, const uint8_t tag[AES_GCM_TAG_SIZE]) {
    uint8_t j0[16], counter[16], expected[AES_GCM_TAG_SIZE];
    gcm_build_j0(iv, j0);

    // Тег считается по ШИФРТЕКСТУ и ДО расшифрования — именно поэтому
    // plaintext может совпадать с ciphertext, и именно поэтому при
    // несовпадении вызывающий не получает ни одного расшифрованного байта.
    gcm_tag(ctx, j0, aad, aad_len, ciphertext, len, expected);

    uint8_t diff = 0;
    for (int i = 0; i < AES_GCM_TAG_SIZE; i++) diff |= (uint8_t)(expected[i] ^ tag[i]);
    if (diff != 0) return -1; // сравнение без раннего выхода - не сливаем, на каком байте разошлось

    memcpy(counter, j0, 16);
    gcm_inc32(counter);
    gcm_ctr(ctx, counter, ciphertext, len, plaintext);
    return 0;
}
