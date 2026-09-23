#pragma once

#include "lib/types.h"

/*
 * Драйвер USB 3.0-класса xHCI (eXtensible Host Controller Interface) —
 * PCI-класс 0x0C (Serial Bus Controller), подкласс 0x03 (USB), prog-if 0x30.
 * Заменяет прежний UHCI-драйвер (uhci.c/uhci.h, удалены).
 *
 * Область действия этой версии (см. план): контроллер + boot-протокол HID
 * (клавиатура/мышь), БЕЗ настоящей SuperSpeed-логики (нет U1/U2 link power
 * management, нет BOS-дескрипторов) — устройства энумерируются и работают
 * на той скорости, которую сами сообщают через PORTSC, как и большинство
 * full/high-speed USB2-устройств. USB Mass Storage — отдельная будущая фаза.
 *
 * Прерывания контроллера НЕ используются (как и раньше у UHCI) — Event Ring
 * опрашивается из usb_poll(), вызываемого раз в тик PIT (100 Гц) из
 * timer_irq_handler(). См. подробное обоснование в xhci.c.
 */

// Найденное HID-устройство boot-протокола (клавиатура или мышь).
// В отличие от uhci_hid_device_t (индексировался по НОМЕРУ ПОРТА root
// hub'а — у UHCI их было фиксированно 2), xhci_get_hid_device(index)
// индексирует МАССИВ НАЙДЕННЫХ УСТРОЙСТВ В ПОРЯДКЕ ОБНАРУЖЕНИЯ — у xHCI
// портов обычно 4+, и с этим API сегодня в дереве никто не работает
// (uhci_get_hid_device() тоже нигде не вызывался), так что это осознанная,
// а не случайная смена контракта.
typedef struct {
    int      valid;
    uint8_t  address;     // USB-адрес, назначенный контроллером (для справки)
    uint8_t  protocol;    // USB_HID_PROTOCOL_KEYBOARD / USB_HID_PROTOCOL_MOUSE
    uint8_t  ep_addr;     // номер interrupt IN endpoint'а (0-15)
    uint16_t max_packet;  // максимальный размер пакета этого endpoint'а
    uint8_t  interval;    // bInterval из дескриптора endpoint'а (мс)
    int      low_speed;   // 1, если PORTSC.Speed == Low
} xhci_hid_device_t;

// Инициализация: ищет контроллер через PCI, отображает его MMIO BAR,
// сбрасывает, поднимает Command/Event Ring, обнаруживает и энумерирует
// подключённые устройства, вооружает interrupt-передачи найденных
// клавиатур/мышей. Безопасно вызывать, даже если контроллер не найден.
void xhci_init(void);

// Вызывается из timer_irq_handler() раз в тик PIT (100 Гц) — опрашивает
// Event Ring на предмет завершённых interrupt-передач и перевооружает их.
void usb_poll(void);

// Возвращает index-е найденное (в порядке обнаружения) HID-устройство,
// либо NULL, если такого нет.
const xhci_hid_device_t *xhci_get_hid_device(int index);

// USB Mass Storage (Bulk-Only Transport + SCSI READ10/WRITE10) — только
// блочное чтение/запись, без монтирования файловой системы (см. план:
// VFS сегодня не поддерживает второй ФС вообще). block_size — настоящий
// размер блока устройства (обычно 512), см. xhci_msd_get_info().
int xhci_msd_device_count(void);
int xhci_msd_get_info(int index, uint32_t *out_max_lba, uint32_t *out_block_size);
int xhci_msd_read_block(int index, uint32_t lba, void *buf, uint32_t block_size);
int xhci_msd_write_block(int index, uint32_t lba, const void *buf, uint32_t block_size);
