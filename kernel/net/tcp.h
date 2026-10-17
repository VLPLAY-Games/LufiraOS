#pragma once

#include "lib/types.h"

/*
 * Минимальный блокирующий TCP-клиент: одно соединение единовременно, без
 * congestion control. Приём строго по порядку (внеочередной сегмент
 * отбрасывается, нет буфера пересборки), сразу ACK'ается. Ретрансмиссия
 * исходящих данных — stop-and-wait по таймеру (tcp_retransmit_if_needed()).
 * Приёмное окно (tcp_rx_window()) настоящее: собеседник не шлёт больше,
 * чем есть места.
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

// Активное открытие: SYN -> ждёт SYN-ACK (~3000мс) -> ACK. 0 = ESTABLISHED,
// -1 = таймаут/ошибка. Заменяет предыдущее соединение (единственное на
// весь стек).
int tcp_connect(uint32_t remote_ip, uint16_t remote_port);

// Отправляет data PSH|ACK-сегментами (stop-and-wait, ретрансмиссия по
// таймеру на каждый — tcp_retransmit_if_needed()). 0 = успех, -1 = ошибка.
int tcp_send(const void *data, uint16_t len);

// Опрашивает карту один раз: если есть непотреблённые байты — 1 и
// указатель/длину куска (до ~1460 байт, валиден до tcp_ack_consumed()),
// иначе 0. Куски не совпадают с границами сегментов — это поток байт.
//
// Очередь (~8КБ) копится независимо от вызывающего; объявляемое окно
// отражает свободное место, так что отправитель сам приостанавливается
// вместо переполнения очереди.
int tcp_recv_poll(uint8_t **out_ptr, uint16_t *out_len);

// Освобождает последний отданный tcp_recv_poll() кусок и шлёт ACK с
// обновлённым окном. Вызывать один раз на каждый успешный tcp_recv_poll() —
// иначе очередь не сдвигается и окно не растёт.
void tcp_ack_consumed(void);

// 1, если удалённая сторона прислала FIN (данных больше не будет —
// вызывающей стороне пора завершать чтение и звать tcp_close()).
int tcp_is_remote_closed(void);

// FIN -> best-effort ожидание закрытия с той стороны, затем локальное
// закрытие в любом случае.
void tcp_close(void);

// Вызывается из ip_receive() на TCP-сегменты, адресованные текущему
// соединению; остальное молча отбрасывается.
void tcp_receive(uint32_t src_ip, const void *payload, uint16_t len);
