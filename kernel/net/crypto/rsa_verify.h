#pragma once

#include "lib/types.h"

/*
 * Две операции, нужные net/tls.c для аутентификации сервера: достать
 * открытый RSA-ключ из DER-сертификата X.509 и проверить им подпись RSA
 * PKCS#1 v1.5 с SHA-256 (подпись ServerKeyExchange).
 *
 * ЭТО НЕ ВАЛИДАЦИЯ СЕРТИФИКАТА: нет проверки цепочки до CA (корневых
 * сертификатов в ОС нет), срока действия, CN/SubjectAltName, расширений,
 * KeyUsage/BasicConstraints, CRL/OCSP. Разбор идёт только до
 * subjectPublicKeyInfo, остальное пропускается не читая (подробнее —
 * SYS_NET_FETCH в kernel/system/syscall/syscall.h и tls.c). Итог: доказано
 * владение закрытым ключом к сертификату, но НЕ принадлежность сертификата
 * запрошенному хосту.
 *
 * Разбор DER — по месту: rsa_public_key_t хранит указатели в буфер
 * сертификата. Буфер должен жить, пока ключ используется (в tls.c — до
 * конца рукопожатия).
 */

typedef struct {
    const uint8_t *modulus;      // без ведущего нулевого байта DER INTEGER
    uint32_t       modulus_len;
    const uint8_t *exponent;
    uint32_t       exponent_len;
} rsa_public_key_t;

// Находит subjectPublicKeyInfo в DER-сертификате X.509 и разбирает в нём
// RSAPublicKey SEQUENCE { INTEGER modulus, INTEGER publicExponent }.
// 0 = успех, -1 = не разобралось (битый DER/не-RSA ключ — итог один: аутентифицировать нечем).
int x509_extract_rsa_pubkey(const uint8_t *der, uint32_t der_len, rsa_public_key_t *out);

// Проверяет подпись RSA PKCS#1 v1.5 (RFC 8017 §8.2.2) над SHA-256(msg).
// 0 = верна, -1 = нет/не разобралась — вызывающий обязан обрывать соединение (см. tls.c).
int rsa_pkcs1_sha256_verify(const rsa_public_key_t *key,
                            const uint8_t *sig, uint32_t sig_len,
                            const uint8_t *msg, uint32_t msg_len);
