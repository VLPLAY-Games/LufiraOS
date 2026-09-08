#pragma once

#include "lib/types.h"

/*
 * Драйвер USB 1.1 UHCI (Universal Host Controller Interface) — PCI-класс
 * 0x0C (Serial Bus Controller), подкласс 0x03 (USB), prog-if 0x00.
 *
 * Фаза A: приведение контроллера в рабочее состояние (сброс, пустой Frame
 * List, детект/сброс портов).
 * Фаза B: control-передачи (setup+data+status TD) и полная энумерация
 * подключённых устройств — назначение адреса, чтение дескрипторов,
 * SET_CONFIGURATION, и для HID-устройств boot-протокола — поиск их
 * interrupt IN endpoint'а и SET_PROTOCOL(boot).
 * Периодические interrupt-передачи (собственно чтение отчётов клавиатуры/
 * мыши) появятся в следующих фазах — см. план разработки.
 */

// Найденное на порту root hub'а USB HID-устройство boot-протокола
// (клавиатура или мышь) — заполняется энумерацией в uhci_init().
// Используется следующими фазами для настройки periodic interrupt transfer.
typedef struct {
    int      valid;       // 0, если на этом порту нет подходящего HID-интерфейса
    uint8_t  address;     // USB-адрес устройства (назначен через SET_ADDRESS)
    uint8_t  protocol;    // USB_HID_PROTOCOL_KEYBOARD / USB_HID_PROTOCOL_MOUSE
    uint8_t  ep_addr;     // номер interrupt IN endpoint'а (0-15)
    uint16_t max_packet;  // максимальный размер пакета этого endpoint'а
    uint8_t  interval;    // bInterval из дескриптора endpoint'а (мс)
    int      low_speed;   // скорость порта, к которому подключено устройство
} uhci_hid_device_t;

// Инициализация: ищет контроллер через PCI, сбрасывает его, поднимает
// пустое расписание, обнаруживает и сбрасывает подключённые порты.
// Безопасно вызывать, даже если контроллер не найден (просто ничего не
// делает дальше).
void uhci_init(void);

// Вызывается из timer_irq_handler() раз в тик PIT (100 Гц). До тех пор,
// пока более поздние фазы не установят периодическое расписание
// (interrupt-QH клавиатуры/мыши), ничего не делает.
void usb_poll(void);

// Возвращает найденное HID-устройство для port_index-го порта root hub'а
// (0 или 1), либо NULL, если порт пуст или на нём не оказалось подходящего
// boot-протокольного HID-интерфейса (клавиатуры/мыши).
const uhci_hid_device_t *uhci_get_hid_device(int port_index);
