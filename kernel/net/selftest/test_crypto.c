/*
 * DEV-ONLY: хостовой прогон контрольных векторов для крипто-примитивов из
 * kernel/net/crypto/. НЕ входит в сборку ядра (в Makefile его нет) —
 * собирается и запускается руками, см. README в этом каталоге.
 *
 * Сами .c-файлы примитивов компилируются здесь БЕЗ ЕДИНОГО #ifdef: вместо
 * заголовков ядра подкладываются заглушки selftest/lib/{types,string}.h
 * (через -I.), которые просто тянут <stdint.h>/<string.h>. Если в
 * примитив когда-нибудь просочится зависимость от чего-то ещё из ядра,
 * эта сборка сломается — это и есть цель.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../crypto/sha256.h"
#include "../crypto/hmac.h"
#include "../crypto/aes_gcm.h"
#include "../crypto/x25519.h"
#include "../crypto/bignum.h"
#include "../crypto/rsa_verify.h"

static int g_fail = 0;

static void hex_to_bin(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned v = 0;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void check(const char *name, const uint8_t *got, const char *want_hex, size_t len) {
    uint8_t want[1024];
    hex_to_bin(want_hex, want, len);
    if (memcmp(got, want, len) == 0) {
        printf("PASS  %s\n", name);
    } else {
        g_fail++;
        printf("FAIL  %s\n      got  ", name);
        for (size_t i = 0; i < len; i++) printf("%02x", got[i]);
        printf("\n      want %s\n", want_hex);
    }
}

/* ---------------- SHA-256 (FIPS 180-4 примеры) ---------------- */
static void test_sha256(void) {
    uint8_t d[32];

    sha256("abc", 3, d);
    check("SHA256(\"abc\")", d,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32);

    sha256("", 0, d);
    check("SHA256(\"\")", d,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32);

    /* 448-битный пример из FIPS 180-4 — проверяет путь с двумя блоками
       padding (длина попадает в диапазон, где 0x80 не влезает до 56). */
    const char *msg2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(msg2, strlen(msg2), d);
    check("SHA256(56-byte msg)", d,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32);

    /* Миллион 'a' — многократный update(), проверяет 64-битный счётчик. */
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    for (int i = 0; i < 1000000; i++) sha256_update(&ctx, "a", 1);
    sha256_final(&ctx, d);
    check("SHA256(1e6 x 'a')", d,
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", 32);
}

/* ---------------- HMAC-SHA256 (RFC 4231) ---------------- */
static void test_hmac(void) {
    uint8_t d[32];

    uint8_t key1[20];
    memset(key1, 0x0b, sizeof(key1));
    hmac_sha256(key1, sizeof(key1), "Hi There", 8, d);
    check("HMAC-SHA256 RFC4231 case 1", d,
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);

    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    check("HMAC-SHA256 RFC4231 case 2", d,
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);

    /* Case 3: 20 x 0xaa ключ, 50 x 0xdd данные. */
    uint8_t key3[20], data3[50];
    memset(key3, 0xaa, sizeof(key3));
    memset(data3, 0xdd, sizeof(data3));
    hmac_sha256(key3, sizeof(key3), data3, sizeof(data3), d);
    check("HMAC-SHA256 RFC4231 case 3", d,
          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 32);

    /* Case 6: ключ 131 байт (длиннее блока -> хешируется). */
    uint8_t key6[131];
    memset(key6, 0xaa, sizeof(key6));
    hmac_sha256(key6, sizeof(key6), "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
    check("HMAC-SHA256 RFC4231 case 6", d,
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32);
}

/* ---------------- PRF TLS 1.2 ---------------- */
static void test_tls_prf(void) {
    /* Широко цитируемый вектор для PRF TLS 1.2 с SHA-256 (его же приводят
       в тестах NSS/mbedTLS): secret/seed по 16 байт, label "test label",
       100 байт вывода. */
    uint8_t secret[16], seed[16], out[100];
    hex_to_bin("9bbe436ba940f017b17652849a71db35", secret, sizeof(secret));
    hex_to_bin("a0ba9f936cda311827a6f796ffd5198c", seed, sizeof(seed));
    tls12_prf_sha256(secret, sizeof(secret), "test label", seed, sizeof(seed), out, sizeof(out));
    check("TLS1.2 PRF (SHA-256, 100 bytes)", out,
          "e3f229ba727be17b8d122620557cd453c2aab21d07c3d495329b52d4e61edb5a"
          "6b301791e90d35c9c9a46b4e14baf9af0fa022f7077def17abfd3797c0564bab"
          "4fbc91666e9def9b97fce34f796789baa48082d122ee42c5a72e5a5110fff701"
          "87347b66", 100);
}

/* ---------------- AES-128-GCM (NIST GCM test vectors) ---------------- */
static void test_aes_gcm(void) {
    aes_gcm_ctx_t ctx;
    uint8_t key[16], iv[12], tag[16], buf[64];

    /* NIST "GCM Test Vectors" (McGrew/Viega, gcm-spec.pdf) Test Case 1:
       K=0^128, IV=0^96, пустые P и A. */
    memset(key, 0, sizeof(key));
    memset(iv, 0, sizeof(iv));
    aes_gcm_init(&ctx, key);
    aes_gcm_encrypt(&ctx, iv, NULL, 0, NULL, 0, NULL, tag);
    check("AES-128-GCM case 1 tag", tag, "58e2fccefa7e3061367f1d57a4e7455a", 16);

    /* Test Case 2: K=0^128, IV=0^96, P=0^128, без A. */
    uint8_t pt16[16];
    memset(pt16, 0, sizeof(pt16));
    aes_gcm_encrypt(&ctx, iv, NULL, 0, pt16, 16, buf, tag);
    check("AES-128-GCM case 2 ct", buf, "0388dace60b6a392f328c2b971b2fe78", 16);
    check("AES-128-GCM case 2 tag", tag, "ab6e47d42cec13bdf53a67b21257bddf", 16);

    /* Test Case 3: K=feffe9928665731c6d6a8f9467308308, IV=cafebabefacedbaddecaf888,
       P = 64 байта, без A. */
    hex_to_bin("feffe9928665731c6d6a8f9467308308", key, 16);
    hex_to_bin("cafebabefacedbaddecaf888", iv, 12);
    uint8_t pt64[64];
    hex_to_bin("d9313225f88406e5a55909c5aff5269a"
               "86a7a9531534f7da2e4c303d8a318a72"
               "1c3c0c95956809532fcf0e2449a6b525"
               "b16aedf5aa0de657ba637b391aafd255", pt64, 64);
    aes_gcm_init(&ctx, key);
    aes_gcm_encrypt(&ctx, iv, NULL, 0, pt64, 64, buf, tag);
    check("AES-128-GCM case 3 ct", buf,
          "42831ec2217774244b7221b784d0d49c"
          "e3aa212f2c02a4e035c17e2329aca12e"
          "21d514b25466931c7d8f6a5aac84aa05"
          "1ba30b396a0aac973d58e091473f5985", 64);
    check("AES-128-GCM case 3 tag", tag, "4d5c2af327cd64a62cf35abd2ba6fab4", 16);

    /* Test Case 4: тот же ключ/IV, P = 60 байт, A = 20 байт — проверяет и
       padding неполного блока, и AAD. */
    uint8_t aad[20];
    hex_to_bin("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
    aes_gcm_encrypt(&ctx, iv, aad, 20, pt64, 60, buf, tag);
    check("AES-128-GCM case 4 ct", buf,
          "42831ec2217774244b7221b784d0d49c"
          "e3aa212f2c02a4e035c17e2329aca12e"
          "21d514b25466931c7d8f6a5aac84aa05"
          "1ba30b396a0aac973d58e091", 60);
    check("AES-128-GCM case 4 tag", tag, "5bc94fbc3221a5db94fae95ae7121a47", 16);

    /* Расшифрование "на месте" + отказ на подделанном теге. */
    uint8_t roundtrip[60];
    memcpy(roundtrip, buf, 60);
    if (aes_gcm_decrypt(&ctx, iv, aad, 20, roundtrip, 60, roundtrip, tag) == 0 &&
        memcmp(roundtrip, pt64, 60) == 0) {
        printf("PASS  AES-128-GCM decrypt in-place roundtrip\n");
    } else {
        g_fail++;
        printf("FAIL  AES-128-GCM decrypt in-place roundtrip\n");
    }

    uint8_t bad_tag[16];
    memcpy(bad_tag, tag, 16);
    bad_tag[15] ^= 1;
    if (aes_gcm_decrypt(&ctx, iv, aad, 20, buf, 60, roundtrip, bad_tag) == -1) {
        printf("PASS  AES-128-GCM rejects forged tag\n");
    } else {
        g_fail++;
        printf("FAIL  AES-128-GCM rejects forged tag\n");
    }
}

/* ---------------- X25519 (RFC 7748 §5.2 / §6.1) ---------------- */
static void test_x25519(void) {
    uint8_t scalar[32], point[32], out[32];

    /* RFC 7748 §5.2, первый вектор. */
    hex_to_bin("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bin("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);
    x25519_scalarmult(out, scalar, point);
    check("X25519 RFC7748 5.2 vector 1", out,
          "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);

    /* RFC 7748 §5.2, второй вектор. */
    hex_to_bin("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", scalar, 32);
    hex_to_bin("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", point, 32);
    x25519_scalarmult(out, scalar, point);
    check("X25519 RFC7748 5.2 vector 2", out,
          "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32);

    /* RFC 7748 §6.1: публичные ключи Alice/Bob от их секретных скаляров
       (проверяет базовую точку) и общий секрет в обе стороны. */
    uint8_t a_priv[32], b_priv[32], a_pub[32], b_pub[32], ss1[32], ss2[32];
    hex_to_bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a_priv, 32);
    hex_to_bin("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b_priv, 32);
    x25519_base(a_pub, a_priv);
    x25519_base(b_pub, b_priv);
    check("X25519 RFC7748 6.1 Alice public", a_pub,
          "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);
    check("X25519 RFC7748 6.1 Bob public", b_pub,
          "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);
    x25519_scalarmult(ss1, a_priv, b_pub);
    x25519_scalarmult(ss2, b_priv, a_pub);
    check("X25519 RFC7748 6.1 shared (a*B)", ss1,
          "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
    check("X25519 RFC7748 6.1 shared (b*A)", ss2,
          "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
}

/* ---------------- bignum modexp ---------------- */
// rc и out[0] считаются ДО вызова — иначе порядок вычисления аргументов
// не определён, и out[0] может быть прочитан до modexp.
static void expect_byte(const char *name, int rc, const uint8_t *out, uint8_t want) {
    if (rc == 0 && out[0] == want) {
        printf("PASS  %s\n", name);
    } else {
        g_fail++;
        printf("FAIL  %s (rc=%d, got %u, want %u)\n", name, rc, out[0], want);
    }
}

static void test_bignum(void) {
    uint8_t out[256];
    int rc;

    /* Мелкие, проверяемые руками случаи (сверено с Python pow()). */
    uint8_t base1[1] = { 7 }, exp1[1] = { 13 }, mod1[1] = { 11 };
    rc = bignum_modexp(out, 1, base1, 1, exp1, 1, mod1, 1);
    expect_byte("modexp 7^13 mod 11 == 2", rc, out, 2);

    /* base > mod — должен приводиться, а не отказывать: 300^5 mod 251 == 100. */
    uint8_t base2[2] = { 0x01, 0x2C }, exp2[1] = { 5 }, mod2[1] = { 251 };
    rc = bignum_modexp(out, 1, base2, 2, exp2, 1, mod2, 1);
    expect_byte("modexp 300^5 mod 251 == 100", rc, out, 100);

    /* Показатель 0 -> 1. */
    uint8_t exp0[1] = { 0 };
    rc = bignum_modexp(out, 1, base1, 1, exp0, 1, mod2, 1);
    expect_byte("modexp x^0 mod n == 1", rc, out, 1);

    /* Реалистичный размер — РЕАЛЬНЫЙ RSA-2048: модуль и подпись взяты из
       сертификата, который сгенерировал openssl (rsa_vectors.h), а
       эталоном служит сама проверка PKCS#1 v1.5 ниже: если modexp на
       2048 битах соврёт хоть в одном бите, восстановленный DigestInfo не
       совпадёт. Отдельно 2048-битный modexp прогонялся против Python
       pow() на 200 случайных входах, см. README. */
}

/* ---------------- X.509 + RSA PKCS#1 v1.5 (данные от openssl) ---------------- */
#include "rsa_vectors.h"

static void test_rsa_verify(void) {
    rsa_public_key_t key;

    if (x509_extract_rsa_pubkey(TEST_CERT_DER, sizeof(TEST_CERT_DER), &key) != 0) {
        g_fail++;
        printf("FAIL  x509_extract_rsa_pubkey (real openssl RSA-2048 cert)\n");
        return;
    }
    if (key.modulus_len == 256 && key.exponent_len == 3 &&
        key.exponent[0] == 0x01 && key.exponent[1] == 0x00 && key.exponent[2] == 0x01) {
        printf("PASS  x509_extract_rsa_pubkey (n=2048 bits, e=65537)\n");
    } else {
        g_fail++;
        printf("FAIL  x509_extract_rsa_pubkey: modulus_len=%u exponent_len=%u\n",
               key.modulus_len, key.exponent_len);
        return;
    }

    /* Настоящая подпись, сделанная `openssl dgst -sha256 -sign` — то же
       самое, что сервер кладёт в ServerKeyExchange. */
    if (rsa_pkcs1_sha256_verify(&key, TEST_SIG, sizeof(TEST_SIG),
                                TEST_MSG, sizeof(TEST_MSG)) == 0) {
        printf("PASS  rsa_pkcs1_sha256_verify accepts valid openssl signature\n");
    } else {
        g_fail++;
        printf("FAIL  rsa_pkcs1_sha256_verify rejected a VALID signature\n");
    }

    /* Негативные проверки — отказ обязателен, иначе проверка подписи не
       проверяет ничего (см. комментарий в rsa_verify.c). */
    uint8_t bad_sig[sizeof(TEST_SIG)];
    memcpy(bad_sig, TEST_SIG, sizeof(TEST_SIG));
    bad_sig[200] ^= 0x01;
    if (rsa_pkcs1_sha256_verify(&key, bad_sig, sizeof(bad_sig),
                                TEST_MSG, sizeof(TEST_MSG)) == -1) {
        printf("PASS  rsa_pkcs1_sha256_verify rejects tampered signature\n");
    } else {
        g_fail++;
        printf("FAIL  rsa_pkcs1_sha256_verify ACCEPTED a tampered signature\n");
    }

    uint8_t bad_msg[sizeof(TEST_MSG)];
    memcpy(bad_msg, TEST_MSG, sizeof(TEST_MSG));
    bad_msg[0] ^= 0x01;
    if (rsa_pkcs1_sha256_verify(&key, TEST_SIG, sizeof(TEST_SIG),
                                bad_msg, sizeof(bad_msg)) == -1) {
        printf("PASS  rsa_pkcs1_sha256_verify rejects tampered message\n");
    } else {
        g_fail++;
        printf("FAIL  rsa_pkcs1_sha256_verify ACCEPTED a tampered message\n");
    }

    if (rsa_pkcs1_sha256_verify(&key, TEST_SIG, sizeof(TEST_SIG) - 1,
                                TEST_MSG, sizeof(TEST_MSG)) == -1) {
        printf("PASS  rsa_pkcs1_sha256_verify rejects wrong signature length\n");
    } else {
        g_fail++;
        printf("FAIL  rsa_pkcs1_sha256_verify ACCEPTED a short signature\n");
    }

    /* Битый DER не должен ни разбираться, ни читать за пределами буфера
       (запускать этот тест полезно под -fsanitize=address, см. README). */
    uint8_t truncated[64];
    memcpy(truncated, TEST_CERT_DER, sizeof(truncated));
    if (x509_extract_rsa_pubkey(truncated, sizeof(truncated), &key) == -1) {
        printf("PASS  x509_extract_rsa_pubkey rejects truncated DER\n");
    } else {
        g_fail++;
        printf("FAIL  x509_extract_rsa_pubkey ACCEPTED truncated DER\n");
    }
}

int main(void) {
    test_sha256();
    test_hmac();
    test_tls_prf();
    test_aes_gcm();
    test_x25519();
    test_bignum();
    test_rsa_verify();

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES PRESENT" : "ALL VECTORS PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
