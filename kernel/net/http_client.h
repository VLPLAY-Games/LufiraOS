#pragma once

#include "lib/types.h"

/*
 * Минимальный HTTP/1.1-клиент "скачать один URL целиком в буфер" — ровно
 * то, что стоит за системным вызовом SYS_NET_FETCH (см. его подробный
 * контракт в kernel/system/syscall/syscall.h) и, через него, за dlpg
 * sync/upgrade. Собирает вместе всё остальное: разбор URL, dns.c (или
 * ip_str_to_addr(), если хост задан числом), tcp.c для http:// и tls.c
 * для https://, и разбор ответа.
 *
 * Объём сознательно урезан (как и у tcp.c/tls.c):
 *  - только метод GET, только HTTP/1.1, всегда с Connection: close;
 *  - ПЕРЕНАПРАВЛЕНИЯ НЕ СЛЕДУЮТСЯ. 301/302/307/308 возвращаются как
 *    обычный результат со своим статус-кодом — решать, идти ли по
 *    Location, предоставлено вызывающему (dlpg.c). Это осознанное
 *    ограничение, а не недоделка: автоматический переход требует и
 *    политики (сколько прыжков, можно ли с https на http), и повторного
 *    разбора URL, а нужные нам адреса raw.githubusercontent.com
 *    отдаются напрямую;
 *  - нет userinfo (user:pass@) и IPv6-литералов в URL;
 *  - нет keep-alive, пайплайнинга, сжатия (Content-Encoding), кук,
 *    аутентификации, условных запросов;
 *  - тело кладётся в буфер вызывающего целиком, потоковой выдачи наружу
 *    нет (dlpg.c и так нужен весь файл сразу).
 * Поддерживается при этом и Content-Length, и Transfer-Encoding: chunked,
 * и "до закрытия соединения" (framing в стиле HTTP/1.0) — на практике
 * встречаются все три.
 */

/*
 * Коды ошибок. ОБЯЗАНЫ численно совпадать с NET_FETCH_E* из
 * libc/include/lufira/syscall.h: sys_net_fetch() (syscall.c) возвращает
 * результат http_fetch() в userspace КАК ЕСТЬ, без перекодировки, а
 * dlpg.c сравнивает его именно с теми константами. Менять значения здесь
 * в одиночку нельзя — только синхронно с libc-заголовком.
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
// заголовков, НЕЗАВИСИМО от того, 2xx он или нет — 404 это НЕ ошибка
// http_fetch(), а успешно скачанное тело с кодом 404 (см. контракт
// SYS_NET_FETCH, syscall.h). HTTP_FETCH_EHTTP зарезервирован именно для
// "корректный HTTP-ответ вообще не удалось разобрать".
int http_fetch(const char *url, void *out_buf, unsigned long out_cap, int *status_out);
