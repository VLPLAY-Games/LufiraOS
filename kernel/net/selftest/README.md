# kernel/net/selftest — DEV-ONLY, НЕ часть сборки ядра

Хостовые тесты для `kernel/net/crypto/*` и `kernel/net/{tls,http_client}.c`.
В `Makefile` ядра этого каталога НЕТ (`KERNEL_C_SOURCES` — явный список,
без `find`/`wildcard`), на `make` он никак не влияет и в образ не попадает.
Это рабочие «леса»: единственный способ проверить TLS/крипто в этой ОС,
пока живая сеть недоступна.

Главный принцип: проверяемые `.c` собираются хостовым `gcc` **без единого
`#ifdef`**. Вместо заголовков ядра подкладываются заглушки из этого
каталога (`lib/`, `drivers/net/`, `system/timer/`, `system/devmode/`),
которые подключаются раньше настоящих за счёт `-I.`. Если в «чистый
алгоритм» когда-нибудь просочится зависимость от ядра — эта сборка
сломается, и это задумано.

## 1. Контрольные векторы крипто-примитивов

```sh
cd kernel/net/selftest
gcc -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -I. -I.. \
    -o /tmp/t_crypto test_crypto.c \
    ../crypto/sha256.c ../crypto/hmac.c ../crypto/aes_gcm.c \
    ../crypto/x25519.c ../crypto/bignum.c ../crypto/rsa_verify.c
/tmp/t_crypto
```

Источники векторов:

| примитив | вектор |
|---|---|
| SHA-256 | FIPS 180-4 («abc», пустая строка, 56 байт, 10^6 × 'a') |
| HMAC-SHA256 | RFC 4231, случаи 1, 2, 3, 6 |
| PRF TLS 1.2 | общеизвестный вектор «test label» (NSS/mbedTLS), 100 байт |
| AES-128-GCM | McGrew/Viega «GCM Test Vectors», случаи 1-4 (они же в NIST SP 800-38D) |
| X25519 | RFC 7748 §5.2 (оба вектора) и §6.1 (ключи и общий секрет Alice/Bob) |
| modexp | 7^13 mod 11, 300^5 mod 251, x^0; плюс RSA-2048 через проверку подписи ниже |
| X.509 + RSA PKCS#1 v1.5 | `rsa_vectors.h` — НАСТОЯЩИЕ сертификат и подпись от openssl |

`rsa_vectors.h` сгенерирован так (повторяемо, кроме самого ключа):

```sh
openssl req -x509 -newkey rsa:2048 -keyout k.pem -out c.pem -days 1 -nodes \
        -subj "/CN=test.example" -sha256
openssl x509 -in c.pem -outform DER -out c.der
printf 'LufiraOS ServerKeyExchange test payload' > msg.bin
openssl dgst -sha256 -sign k.pem -out sig.bin msg.bin
# затем c.der/sig.bin/msg.bin выгружены в C-массивы
```

`bignum_modexp()` дополнительно сверялся с `pow(b, e, m)` из Python на 200
случайных входах (разрядности от 8 до 2048 бит, в т.ч. `base > mod` и
нулевой показатель) — расхождений нет.

## 2. TLS и HTTP против настоящих серверов на localhost

`host_stubs.c` подменяет `tcp.c`/`pit.c`/`rtl8139.c` обычными POSIX-сокетами
и часами хоста, сохраняя документированную семантику `tcp_recv_poll()`
(один пришедший кусок до 1460 байт + обязательный `tcp_ack_consumed()`) —
именно чтобы проверялась настоящая логика пересборки записей.

```sh
gcc -Wall -Wextra -std=gnu11 -g -fsanitize=address,undefined -I. -I.. \
    -o /tmp/t_net test_net.c host_stubs.c ../tls.c ../http_client.c \
    ../crypto/sha256.c ../crypto/hmac.c ../crypto/aes_gcm.c \
    ../crypto/x25519.c ../crypto/bignum.c ../crypto/rsa_verify.c

# TLS 1.2 сервер (тот же набор шифров и та же кривая, что мы предлагаем):
openssl s_server -accept 14433 -cert c.pem -key k.pem -tls1_2 \
        -cipher 'ECDHE-RSA-AES128-GCM-SHA256' -groups X25519 -www -quiet &

/tmp/t_net tls 14433                       # рукопожатие + GET + расшифровка ответа
/tmp/t_net fetch https://127.0.0.1:14433/  # то же через http_fetch()
/tmp/t_net url                             # разбор URL и коды ошибок
/tmp/t_net cap http://host/path 1000       # ENOSPC на тесном буфере
/tmp/t_net expect http://host/path -5      # ждём конкретный код ошибки

# Проверка, что подделка подписи ДЕЙСТВИТЕЛЬНО обрывает рукопожатие:
# портим один бит в server_random (он входит в подписанные данные).
LUFIRA_TAMPER_OFFSET=11 /tmp/t_net fetch https://127.0.0.1:14433/
```

Для HTTP-обрамлений (Content-Length / chunked / «до закрытия», 404, пустое
тело, 302 без перехода, мусор вместо статус-строки) использовался свой
маленький сервер на Python, отдающий одно и то же тело всеми способами;
совпадение проверялось по SHA-256 тела, который печатает сам `test_net`.

## Чего этими тестами проверить НЕЛЬЗЯ

Настоящий путь «ядро -> RTL8139 -> QEMU SLIRP -> интернет -> github.com»:
здесь нет ни драйвера, ни ядерного TCP (`tcp.c` подменён сокетами), ни
`dns.c`. Проверено всё, что НАД транспортом; сам транспорт и живой
`raw.githubusercontent.com` — только в запущенной ОС.
