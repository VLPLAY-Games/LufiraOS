#pragma once

#include "lib/types.h"

/*
 * Минимальный UDP — только то, что нужно dns.c (единственный сегодняшний
 * потребитель): отправить один датаграм и дождаться ОДНОГО ответного на
 * заданный локальный порт. Тот же стиль упрощения, что и у tcp.c/icmp.c
 * (см. их шапки) — один "слот" ожидания одновременно, без очереди и без
 * demultiplexing по нескольким одновременным портам/сокетам сразу.
 */

#define UDP_HEADER_LEN 8

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;   // network order, заголовок+данные
    uint16_t checksum; // network order
} udp_header_t;

// Отправляет один датаграм. 0 = успех, -1 = ошибка (слишком длинный/ARP/
// отправка, см. реализацию).
int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *data, uint16_t len);

// Начинает ждать датаграм НА local_port — вызывать ПЕРЕД udp_send() своего
// запроса (как dns_resolve() и делает), иначе ранний ответ, пришедший до
// udp_listen(), будет молча потерян udp_receive() ниже.
void udp_listen(uint16_t local_port);
void udp_stop_listen(void);

// Опрашивает карту один раз (как tcp_recv_poll()/rtl8139_poll()). 1 и
// заполняет out_* (валидны до следующего udp_recv_poll()/udp_listen()),
// если на прослушиваемый порт пришёл датаграм с прошлого вызова; иначе 0.
int udp_recv_poll(uint32_t *out_src_ip, uint16_t *out_src_port, uint8_t **out_ptr, uint16_t *out_len);

// Вызывается из ip_receive() на UDP-датаграммы.
void udp_receive(uint32_t src_ip, const void *payload, uint16_t len);
