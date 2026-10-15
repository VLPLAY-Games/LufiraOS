#pragma once

#include "lib/types.h"

/*
 * Драйвер сетевой карты Intel 8254x ("e1000") — классическое семейство
 * (82540EM и близкие: 82544/82545/82546), PCI vendor 0x8086, MMIO BAR0
 * (в отличие от RTL8139 — там I/O BAR0), дескрипторные кольца TX/RX
 * (16-байтные "legacy" дескрипторы, а не один большой кольцевой буфер).
 * QEMU `-device e1000` эмулирует именно 82540EM (device id 0x100E) — то,
 * на чём этот драйвер в первую очередь проверялся.
 *
 * Как и RTL8139/xHCI/AC'97/HPET — прерывания не используются (нет ни
 * APIC/MSI, ни разбора PCI capability list в этом ядре): IMS всегда 0,
 * e1000_poll() читает готовность дескрипторов напрямую через их собственный
 * бит DD (Descriptor Done), не через ICR/аппаратную линию INTx.
 *
 * Тот же интерфейс, что у rtl8139.h/e1000e.h — net.c подключает любую из
 * этих карт через один и тот же nic_driver_t (см. net.h), остальной стек
 * (eth.c и выше) не знает, какая именно карта активна.
 */

void e1000_init(void);
int e1000_found(void);
const uint8_t *e1000_get_mac(void);
int e1000_send(const void *frame, uint16_t len);
void e1000_poll(void);
