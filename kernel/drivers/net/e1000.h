#pragma once

#include "lib/types.h"

/*
 * Драйвер Intel 8254x ("e1000", 82540EM и близкие 82544/82545/82546),
 * PCI vendor 0x8086, MMIO BAR0 (в отличие от RTL8139 с I/O BAR0),
 * дескрипторные кольца TX/RX (16-байтные "legacy" дескрипторы). QEMU
 * `-device e1000` эмулирует 82540EM (0x100E).
 *
 * Прерывания не используются (нет APIC/MSI/PCI capability list): IMS
 * всегда 0, e1000_poll() читает бит DD (Descriptor Done) дескриптора
 * напрямую, не через ICR/INTx.
 *
 * Интерфейс общий с rtl8139.h/e1000e.h — net.c подключает любую карту
 * через один nic_driver_t, остальной стек не знает, какая активна.
 */

void e1000_init(void);
int e1000_found(void);
const uint8_t *e1000_get_mac(void);
int e1000_send(const void *frame, uint16_t len);
void e1000_poll(void);
