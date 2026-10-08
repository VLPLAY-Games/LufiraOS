#pragma once

#include "lib/types.h"

/*
 * Минимальный HTTP/1.1-клиент "скачать один URL целиком в буфер" — то,
 * что стоит за SYS_NET_FETCH (см. контракт в syscall.h) и, через него,
 * за dlpg sync/upgrade. Собирает URL-разбор, dns.c/ip_str_to_addr(),
 * tcp.c (http://) и tls.c (https://), разбор ответа.
 *
 * Объём урезан (как у tcp.c/tls.c):
 *  - только GET, только HTTP/1.1, всегда Connection: close;
 *  - ПЕРЕНАПРАВЛЕНИЯ НЕ СЛЕДУЮТСЯ — 301/302/307/308 возвращаются как
 *    обычный результат со своим статусом, решение идти по Location —
 *    за вызывающим (dlpg.c). Осознанно: авторедирект требует политики
 *    числа прыжков и повторного разбора URL, а нужные нам адреса
 *    raw.githubusercontent.com отдаются напрямую;
 *  - нет userinfo/IPv6-литералов в URL, keep-alive, пайплайнинга,
 *    сжатия, кук, аутентификации, условных запросов;
 *  - тело кладётся в буфер вызывающего целиком, без потоковой выдачи.
 * Поддерживаются Content-Length, Transfer-Encoding: chunked и framing
 * "до закрытия соединения" — на практике встречаются все три.
 */

/*
 * Коды ошибок ОБЯЗАНЫ численно совпадать с NET_FETCH_E* из
 * libc/include/lufira/syscall.h: sys_net_fetch() отдаёт результат
 * http_fetch() в userspace как есть, а dlpg.c сравнивает именно с этими
 * константами. Менять значения здесь в одиночку нельзя.
 */
#define HTTP_FETCH_EBADURL  (-1) // не распознан URL (нужна схема http:// или https://)
#define HTTP_FETCH_EDNS     (-2) // не удалось резолвить хост (dns.c)
#define HTTP_FETCH_ECONNECT (-3) // TCP-соединение не установилось (tcp.c)
#define HTTP_FETCH_ETLS     (-4) // TLS-рукопожатие не удалось (tls.c, только https://)
#define HTTP_FETCH_EHTTP    (-5) // ответ сервера не разобрался как HTTP
#define HTTP_FETCH_ENOSPC   (-6) // тело ответа больше out_cap
#define HTTP_FETCH_ENODEV   (-7) // сетевая карта не найдена

// Скачивает url (http:// или https://) целиком в out_buf (до out_cap
// байт). Возвращает длину тела (>=0, <= out_cap) при успехе; status_out
// (может быть NULL) получает HTTP статус-код, если разбор дошёл до
// заголовков, НЕЗАВИСИМО от 2xx/4xx/5xx — 404 это успешно скачанное
// тело с кодом 404, не ошибка. HTTP_FETCH_EHTTP — только для "ответ
// вообще не разобрался как HTTP".
int http_fetch(const char *url, void *out_buf, unsigned long out_cap, int *status_out);
