#pragma once

#include "lib/types.h"

/*
 * AES-128 (FIPS 197) в режиме GCM (NIST SP 800-38D) — единственный шифр,
 * который умеет наш TLS-клиент (net/tls.c предлагает ровно один
 * cipher suite, TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 = 0xC02F, см. его
 * комментарий). Только 128-битный ключ (AES-192/256 не нужны) и только
 * 12-байтный IV — то есть ровно тот вариант GCM, который требует
 * RFC 5288 для TLS 1.2.
 *
 * Расшифрование реализовано через тот же шифрующий блок (GCM — поточный
 * режим на базе CTR), поэтому обратного преобразования AES (InvSubBytes и
 * т.д.) здесь нет вообще.
 */

#define AES_GCM_TAG_SIZE 16
#define AES_GCM_IV_SIZE  12

typedef struct {
    uint32_t round_keys[44]; // AES-128: 11 раундовых ключей по 4 слова

    // Умножение в GF(2^128) из GHASH, расписанное прямо по определению
    // (NIST SP 800-38D §6.3), перебирает 128 бит первого множителя и на
    // каждом шаге сдвигает второй (H) на бит вправо с приведением по
    // R = 11100001||0^120. Последовательность этих 128 сдвигов зависит
    // ТОЛЬКО от H, то есть только от ключа — поэтому считается один раз в
    // aes_gcm_init() и дальше умножение это просто XOR тех её элементов,
    // которым соответствуют единичные биты. 2КБ на контекст (их два на
    // соединение, по одному на направление) в обмен на ~4x по скорости:
    // через этот код проходит ВСЁ тело скачиваемого пакета (dlpg upgrade,
    // сотни килобайт), а не только рукопожатие.
    uint8_t  h_bits[128][16]; // h_bits[0] == H, h_bits[i] == H, сдвинутый на i
} aes_gcm_ctx_t;

void aes_gcm_init(aes_gcm_ctx_t *ctx, const uint8_t key[16]);

// Шифрует len байт plaintext -> ciphertext и считает тег аутентификации по
// aad || ciphertext. ciphertext может совпадать с plaintext (шифрование
// "на месте" безопасно — CTR побайтово независим).
void aes_gcm_encrypt(aes_gcm_ctx_t *ctx, const uint8_t iv[AES_GCM_IV_SIZE],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *plaintext, uint32_t len,
                     uint8_t *ciphertext, uint8_t tag[AES_GCM_TAG_SIZE]);

// Проверяет тег и, ТОЛЬКО если он совпал, расшифровывает. plaintext может
// совпадать с ciphertext: тег считается по шифртексту ДО расшифрования,
// поэтому работа "на месте" корректна. Возвращает 0 при совпадении тега,
// -1 при несовпадении (в этом случае plaintext не записан — на подделанную
// запись вызывающий не должен увидеть вообще никаких "почти правильных"
// данных, см. tls.c, где это сразу фатальная ошибка соединения).
int aes_gcm_decrypt(aes_gcm_ctx_t *ctx, const uint8_t iv[AES_GCM_IV_SIZE],
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *ciphertext, uint32_t len,
                    uint8_t *plaintext, const uint8_t tag[AES_GCM_TAG_SIZE]);
