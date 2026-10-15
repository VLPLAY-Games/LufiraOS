#pragma once

#include "lib/types.h"

/*
 * Драйвер сетевой карты Intel 8257x/8258x ("e1000e") — более новое
 * PCIe-поколение той же регистровой модели, что и классический 8254x
 * (см. e1000.h): тот же MMIO BAR0, те же смещения CTRL/STATUS/EERD/RCTL/
 * TCTL/RDBAL.../TDBAL... и тот же формат legacy-дескрипторов — поэтому файл
 * структурно почти идентичен e1000.c, отличается только PCI vendor/device
 * (другой список device id — 82571-82583/ICH8-10) и собственными
 * статическими символами (e1000e_*). QEMU `-device e1000e` эмулирует
 * 82574L (device id 0x10D3) — на нём этот драйвер и проверялся.
 *
 * Прерывания не используются — тот же принцип, что и у e1000.c/RTL8139
 * (IMS=0, опрос через DD-бит дескриптора из net_poll()).
 */

void e1000e_init(void);
int e1000e_found(void);
const uint8_t *e1000e_get_mac(void);
int e1000e_send(const void *frame, uint16_t len);
void e1000e_poll(void);
