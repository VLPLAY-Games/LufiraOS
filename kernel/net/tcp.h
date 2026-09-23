#pragma once

#include "lib/types.h"

/*
 * Минимальный блокирующий TCP-клиент — ОДНО соединение единовременно, БЕЗ
 * ретрансмиссии по таймеру, БЕЗ congestion control, приём только "по
 * порядку" (внеочередной/пропущенный сегмент молча отбрасывается — общий
 * таймаут вызывающей стороны в итоге просто оборвёт операцию). Осознанно
 * упрощённый уровень, подтверждённый для wget по HTTP: сеть QEMU SLIRP
 * практически никогда не переупорядочивает/не теряет пакеты, см. план.
 */

#define TCP_FLAG_FIN 0x01u
#define TCP_FLAG_SYN 0x02u
#define TCP_FLAG_RST 0x04u
#define TCP_FLAG_PSH 0x08u
#define TCP_FLAG_ACK 0x10u

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset_reserved; // старшие 4 бита — длина заголовка в 32-битных словах
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

// Активное открытие: SYN -> ждёт SYN-ACK (ограниченный таймаут, ~3000мс) ->
// ACK. 0 = ESTABLISHED, -1 = таймаут/ошибка. Заменяет любое предыдущее
// соединение (единственное на весь стек, см. план).
int tcp_connect(uint32_t remote_ip, uint16_t remote_port);

// Отправляет data одним или несколькими PSH|ACK-сегментами (без
// ретрансмиссии — см. план). 0 = успех, -1 = не ESTABLISHED/ошибка отправки.
int tcp_send(const void *data, uint16_t len);

// Опрашивает карту один раз. Если есть непотреблённый пришедший кусок
// данных — возвращает 1 и указатель/длину (валидны до следующего вызова
// tcp_ack_consumed()/tcp_recv_poll()); иначе 0.
int tcp_recv_poll(uint8_t **out_ptr, uint16_t *out_len);

// Подтверждает последний отданный tcp_recv_poll() кусок (шлёт ACK,
// освобождает буфер под приём следующего). Вызывать ровно один раз после
// каждого успешного tcp_recv_poll().
void tcp_ack_consumed(void);

// 1, если удалённая сторона прислала FIN (данных больше не будет —
// вызывающей стороне пора завершать чтение и звать tcp_close()).
int tcp_is_remote_closed(void);

// FIN -> best-effort (ограниченный таймаут) ожидание закрытия с той
// стороны, затем локальное закрытие в любом случае (клиент, которому
// сокет больше не нужен, не обязан строго дожидаться graceful close).
void tcp_close(void);

// Вызывается из ip_receive() на TCP-сегменты, адресованные текущему
// соединению; остальное молча отбрасывается.
void tcp_receive(uint32_t src_ip, const void *payload, uint16_t len);
