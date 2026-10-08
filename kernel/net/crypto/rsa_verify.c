#include "rsa_verify.h"
#include "sha256.h"
#include "bignum.h"
#include "lib/string.h"

// ========== Минимальный обход DER/ASN.1 ==========

#define DER_TAG_INTEGER       0x02
#define DER_TAG_BIT_STRING    0x03
#define DER_TAG_SEQUENCE      0x30
#define DER_TAG_CONTEXT_0     0xA0 // [0] EXPLICIT — так закодировано поле version в TBSCertificate

// Разбирает один TLV, начинающийся в buf[pos]. Возвращает 0 и заполняет
// тег, длину заголовка (тег + байты длины) и длину значения; -1, если
// структура не влезает в buf_len или использует форму длины, которой в
// сертификатах не бывает (неопределённая длина, длина больше 4 байт).
static int der_read(const uint8_t *buf, uint32_t buf_len, uint32_t pos,
                    uint8_t *out_tag, uint32_t *out_hdr_len, uint32_t *out_val_len) {
    if (pos + 2 > buf_len) return -1;

    uint8_t tag = buf[pos];
    uint8_t first = buf[pos + 1];
    uint32_t hdr = 2;
    uint32_t len;

    if ((first & 0x80) == 0) {
        len = first; // короткая форма: длина прямо в байте
    } else {
        uint32_t n = first & 0x7F;
        if (n == 0 || n > 4) return -1; // 0 = неопределённая длина (только BER), >4 — нереалистично
        if (pos + 2 + n > buf_len) return -1;
        len = 0;
        for (uint32_t i = 0; i < n; i++) len = (len << 8) | buf[pos + 2 + i];
        hdr = 2 + n;
    }

    if (len > buf_len - pos - hdr) return -1; // значение вылезает за буфер

    *out_tag = tag;
    *out_hdr_len = hdr;
    *out_val_len = len;
    return 0;
}

// Пропускает один TLV целиком: возвращает позицию СРАЗУ ПОСЛЕ него, или
// -1. Если expect_tag не 0, дополнительно требует совпадения тега.
static int64_t der_skip(const uint8_t *buf, uint32_t buf_len, uint32_t pos, uint8_t expect_tag) {
    uint8_t tag; uint32_t hdr, len;
    if (der_read(buf, buf_len, pos, &tag, &hdr, &len) != 0) return -1;
    if (expect_tag != 0 && tag != expect_tag) return -1;
    return (int64_t)pos + hdr + len;
}

// Входит внутрь TLV: возвращает позицию его ЗНАЧЕНИЯ и его длину.
static int der_enter(const uint8_t *buf, uint32_t buf_len, uint32_t pos, uint8_t expect_tag,
                     uint32_t *out_pos, uint32_t *out_len) {
    uint8_t tag; uint32_t hdr, len;
    if (der_read(buf, buf_len, pos, &tag, &hdr, &len) != 0) return -1;
    if (expect_tag != 0 && tag != expect_tag) return -1;
    *out_pos = pos + hdr;
    *out_len = len;
    return 0;
}

// Читает DER INTEGER как беззнаковое целое: отдаёт указатель на его байты
// без ведущего нулевого байта (DER добавляет его, когда старший бит
// значения единичный, чтобы INTEGER не читался как отрицательный).
static int der_read_uint(const uint8_t *buf, uint32_t buf_len, uint32_t pos,
                         const uint8_t **out_bytes, uint32_t *out_len, uint32_t *out_next) {
    uint8_t tag; uint32_t hdr, len;
    if (der_read(buf, buf_len, pos, &tag, &hdr, &len) != 0) return -1;
    if (tag != DER_TAG_INTEGER || len == 0) return -1;

    const uint8_t *p = buf + pos + hdr;
    uint32_t n = len;
    while (n > 1 && p[0] == 0x00) { p++; n--; }

    *out_bytes = p;
    *out_len = n;
    *out_next = pos + hdr + len;
    return 0;
}

int x509_extract_rsa_pubkey(const uint8_t *der, uint32_t der_len, rsa_public_key_t *out) {
    if (!der || !out || der_len < 16) return -1;

    /*
     * Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue }
     * TBSCertificate ::= SEQUENCE {
     *     [0] version (ОПЦИОНАЛЬНО — в v1 отсутствует),
     *     serialNumber INTEGER, signature AlgorithmIdentifier (SEQUENCE),
     *     issuer Name (SEQUENCE), validity (SEQUENCE), subject Name (SEQUENCE),
     *     subjectPublicKeyInfo SubjectPublicKeyInfo, ... }
     * Нас интересует только последнее поле, поэтому всё предыдущее просто
     * пропускается по тегам, без чтения содержимого.
     */
    uint32_t cert_pos, cert_len;
    if (der_enter(der, der_len, 0, DER_TAG_SEQUENCE, &cert_pos, &cert_len) != 0) return -1;

    uint32_t tbs_pos, tbs_len;
    if (der_enter(der, der_len, cert_pos, DER_TAG_SEQUENCE, &tbs_pos, &tbs_len) != 0) return -1;
    uint32_t tbs_end = tbs_pos + tbs_len;

    uint32_t p = tbs_pos;
    uint8_t tag; uint32_t hdr, len;

    if (der_read(der, tbs_end, p, &tag, &hdr, &len) != 0) return -1;
    if (tag == DER_TAG_CONTEXT_0) { // version присутствует (v2/v3) — пропускаем
        p += hdr + len;
    }

    int64_t next;
    next = der_skip(der, tbs_end, p, DER_TAG_INTEGER);  if (next < 0) return -1; p = (uint32_t)next; // serialNumber
    next = der_skip(der, tbs_end, p, DER_TAG_SEQUENCE); if (next < 0) return -1; p = (uint32_t)next; // signature
    next = der_skip(der, tbs_end, p, DER_TAG_SEQUENCE); if (next < 0) return -1; p = (uint32_t)next; // issuer
    next = der_skip(der, tbs_end, p, DER_TAG_SEQUENCE); if (next < 0) return -1; p = (uint32_t)next; // validity
    next = der_skip(der, tbs_end, p, DER_TAG_SEQUENCE); if (next < 0) return -1; p = (uint32_t)next; // subject

    // SubjectPublicKeyInfo ::= SEQUENCE { algorithm AlgorithmIdentifier, subjectPublicKey BIT STRING }
    uint32_t spki_pos, spki_len;
    if (der_enter(der, tbs_end, p, DER_TAG_SEQUENCE, &spki_pos, &spki_len) != 0) return -1;
    uint32_t spki_end = spki_pos + spki_len;

    next = der_skip(der, spki_end, spki_pos, DER_TAG_SEQUENCE); // algorithm (OID не проверяем)
    if (next < 0) return -1;

    uint32_t bits_pos, bits_len;
    if (der_enter(der, spki_end, (uint32_t)next, DER_TAG_BIT_STRING, &bits_pos, &bits_len) != 0) return -1;
    if (bits_len < 2) return -1;
    // Первый байт BIT STRING — число неиспользуемых бит в последнем байте;
    // для DER-ключа он всегда 0, и сам ключ начинается после него.
    if (der[bits_pos] != 0x00) return -1;
    uint32_t key_pos = bits_pos + 1;
    uint32_t key_end = bits_pos + bits_len;

    // RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }.
    // Если ключ не RSA (EC/Ed25519), эта структура не разберётся — и это
    // корректный отказ: проверять подпись нам всё равно было бы нечем.
    uint32_t rsa_pos, rsa_len;
    if (der_enter(der, key_end, key_pos, DER_TAG_SEQUENCE, &rsa_pos, &rsa_len) != 0) return -1;
    uint32_t rsa_end = rsa_pos + rsa_len;

    const uint8_t *mod_bytes; uint32_t mod_len, after_mod;
    if (der_read_uint(der, rsa_end, rsa_pos, &mod_bytes, &mod_len, &after_mod) != 0) return -1;

    const uint8_t *exp_bytes; uint32_t exp_len, after_exp;
    if (der_read_uint(der, rsa_end, after_mod, &exp_bytes, &exp_len, &after_exp) != 0) return -1;

    if (mod_len < 64 || mod_len > BIGNUM_MAX_BYTES) return -1; // короче 512 бит — не настоящий RSA-ключ
    if (exp_len == 0 || exp_len > 8) return -1;

    out->modulus = mod_bytes;
    out->modulus_len = mod_len;
    out->exponent = exp_bytes;
    out->exponent_len = exp_len;
    return 0;
}

// Фиксированный префикс DigestInfo для SHA-256 (RFC 8017 §9.2, notes):
// SEQUENCE { SEQUENCE { OID 2.16.840.1.101.3.4.2.1, NULL }, OCTET STRING (32) }
static const uint8_t SHA256_DIGEST_INFO_PREFIX[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

#define RSA_PKCS1_PREFIX_LEN (sizeof(SHA256_DIGEST_INFO_PREFIX))
#define RSA_DIGEST_INFO_LEN  (RSA_PKCS1_PREFIX_LEN + SHA256_DIGEST_SIZE)

int rsa_pkcs1_sha256_verify(const rsa_public_key_t *key,
                            const uint8_t *sig, uint32_t sig_len,
                            const uint8_t *msg, uint32_t msg_len) {
    if (!key || !sig || !msg) return -1;
    if (key->modulus_len > BIGNUM_MAX_BYTES) return -1;

    // Подпись RSA всегда ровно размера модуля (RFC 8017 §8.2.2 шаг 1).
    if (sig_len != key->modulus_len) return -1;
    // В закодированном сообщении обязаны поместиться 0x00 0x01, минимум
    // 8 байт 0xFF, разделитель 0x00 и сам DigestInfo.
    if (sig_len < RSA_DIGEST_INFO_LEN + 11) return -1;

    static uint8_t em[BIGNUM_MAX_BYTES]; // ~512 байт — слишком много для стека ядра
    if (bignum_modexp(em, sig_len, sig, sig_len,
                      key->exponent, key->exponent_len,
                      key->modulus, key->modulus_len) != 0) {
        return -1;
    }

    /*
     * EM должен быть ровно 0x00 || 0x01 || PS || 0x00 || DigestInfo, где
     * PS — не меньше 8 байт 0xFF. Сравнение ПОЛНОЕ и жёсткое: любая
     * вольность тут (например, "найдём DigestInfo где-нибудь в хвосте")
     * — это классическая дыра в проверке PKCS#1 v1.5, позволяющая
     * подделать подпись при малом показателе. Поэтому и padding, и
     * позиция DigestInfo проверяются точно.
     */
    if (em[0] != 0x00 || em[1] != 0x01) return -1;

    uint32_t i = 2;
    while (i < sig_len && em[i] == 0xFF) i++;
    if (i - 2 < 8) return -1;                       // PS короче 8 байт
    if (i >= sig_len || em[i] != 0x00) return -1;   // разделитель
    i++;

    if (sig_len - i != RSA_DIGEST_INFO_LEN) return -1; // DigestInfo обязан занять весь остаток

    uint8_t diff = 0;
    for (uint32_t j = 0; j < RSA_PKCS1_PREFIX_LEN; j++) {
        diff |= (uint8_t)(em[i + j] ^ SHA256_DIGEST_INFO_PREFIX[j]);
    }

    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256(msg, msg_len, digest);
    for (uint32_t j = 0; j < SHA256_DIGEST_SIZE; j++) {
        diff |= (uint8_t)(em[i + RSA_PKCS1_PREFIX_LEN + j] ^ digest[j]);
    }

    return (diff == 0) ? 0 : -1;
}
