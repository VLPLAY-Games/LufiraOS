#pragma once

#include "lib/types.h"

/*
 * AES-128-GCM (FIPS 197 + NIST SP 800-38D) — единственный шифр TLS-клиента
 * (net/tls.c предлагает только TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 = 0xC02F).
 * Только 128-битный ключ и 12-байтный IV — вариант, который требует RFC 5288
 * для TLS 1.2.
 *
 * GCM — поточный режим на основе CTR, поэтому расшифрование использует тот
 * же шифрующий блок; обратного AES (InvSubBytes и т.д.) здесь нет вообще.
 */

#define AES_GCM_TAG_SIZE 16
#define AES_GCM_IV_SIZE  12

typedef struct {
    uint32_t round_keys[44]; // AES-128: 11 раундовых ключей по 4 слова

    // Умножение GF(2^128) из GHASH (SP 800-38D §6.3): второй множитель (H)
    // по определению сдвигается на бит вправо с приведением по R=11100001||0^120.
    // Эта последовательность 128 сдвигов зависит только от ключа, поэтому
    // считается один раз в aes_gcm_init(), а умножение — просто XOR нужных строк.
    // 2КБ на контекст ради ~4x скорости: через этот код проходит всё тело пакета.
    uint8_t  h_bits[128][16]; // h_bits[0] == H, h_bits[i] == H, сдвинутый на i
} aes_gcm_ctx_t;

void aes_gcm_init(aes_gcm_ctx_t *ctx, const uint8_t key[16]);

// Шифрует plaintext -> ciphertext (len байт) и считает тег по aad||ciphertext.
// ciphertext может совпадать с plaintext — CTR побайтово независим.
void aes_gcm_encrypt(aes_gcm_ctx_t *ctx, const uint8_t iv[AES_GCM_IV_SIZE],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *plaintext, uint32_t len,
                     uint8_t *ciphertext, uint8_t tag[AES_GCM_TAG_SIZE]);

// Проверяет тег и расшифровывает только если он совпал; тег считается по
// шифртексту ДО расшифрования, поэтому работа "на месте" корректна.
// 0 — тег совпал, -1 — нет (plaintext не записан; см. tls.c, фатальная ошибка).
int aes_gcm_decrypt(aes_gcm_ctx_t *ctx, const uint8_t iv[AES_GCM_IV_SIZE],
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *ciphertext, uint32_t len,
                    uint8_t *plaintext, const uint8_t tag[AES_GCM_TAG_SIZE]);
