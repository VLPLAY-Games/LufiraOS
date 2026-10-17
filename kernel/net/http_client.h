#pragma once

#include "lib/types.h"

/*
 * Минимальный HTTP/1.1-клиент "скачать один URL целиком в буфер" — то,
 * что стоит за SYS_NET_FETCH (syscall.h). Собирает URL-разбор,
 * dns.c/ip_str_to_addr(), tcp.c (http://) и tls.c (https://), разбор ответа.
 *
 * Урезан: только GET, HTTP/1.1, Connection: close; редиректы (301/302/
 * 307/308) не следуются — возвращаются как обычный результат, Location
 * разбирает вызывающий (dlpg.c); нет userinfo/IPv6-литералов, keep-alive,
 * пайплайнинга, сжатия, кук, аутентификации, условных запросов; тело
 * кладётся в буфер целиком. Поддерживаются Content-Length, chunked и
 * framing "до закрытия соединения".
 */

/*
 * Коды ошибок должны численно совпадать с NET_FETCH_E* из
 * libc/include/lufira/syscall.h — sys_net_fetch() отдаёт результат как
 * есть, dlpg.c сравнивает с этими константами. Менять в одиночку нельзя.
 */
#define HTTP_FETCH_EBADURL  (-1) // не распознан URL (нужна схема http:// или https://)
#define HTTP_FETCH_EDNS     (-2) // не удалось резолвить хост (dns.c)
#define HTTP_FETCH_ECONNECT (-3) // TCP-соединение не установилось (tcp.c)
#define HTTP_FETCH_ETLS     (-4) // TLS-рукопожатие не удалось (tls.c, только https://)
#define HTTP_FETCH_EHTTP    (-5) // ответ сервера не разобрался как HTTP
#define HTTP_FETCH_ENOSPC   (-6) // тело ответа больше out_cap
#define HTTP_FETCH_ENODEV   (-7) // сетевая карта не найдена
#define HTTP_FETCH_EBUSY    (-8) // сеть занята другим fetch'ом дольше бюджета ожидания (net_lock_acquire(), net.h)

// Скачивает url (http:// или https://) целиком в out_buf (до out_cap
// байт). Возвращает длину тела (>=0, <= out_cap) при успехе; status_out
// (может быть NULL) получает HTTP статус-код, если разбор дошёл до
// заголовков, НЕЗАВИСИМО от 2xx/4xx/5xx — 404 это успешно скачанное
// тело с кодом 404, не ошибка. HTTP_FETCH_EHTTP — только для "ответ
// вообще не разобрался как HTTP".
int http_fetch(const char *url, void *out_buf, unsigned long out_cap, int *status_out);
