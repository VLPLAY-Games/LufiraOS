#pragma once

#include "lib/types.h"

/*
 * HCD-независимые константы протокола USB (device requests, типы
 * дескрипторов, setup-пакет). Специфика конкретного хост-контроллера
 * (регистры UHCI, Frame List, Queue Head/TD) живёт в uhci.h — это
 * разделение специально сделано так, чтобы будущий драйвер OHCI мог
 * переиспользовать этот заголовок, не завися от UHCI.
 */

/* =========================================================
 * bmRequestType
 * ========================================================= */

#define USB_REQ_DIR_OUT          0x00
#define USB_REQ_DIR_IN           0x80

#define USB_REQ_TYPE_STANDARD    0x00
#define USB_REQ_TYPE_CLASS       0x20
#define USB_REQ_TYPE_VENDOR      0x40

#define USB_REQ_RECIP_DEVICE     0x00
#define USB_REQ_RECIP_INTERFACE  0x01
#define USB_REQ_RECIP_ENDPOINT   0x02

/* =========================================================
 * Стандартные device requests (bRequest)
 * ========================================================= */

#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B

/* Class-specific requests HID (используются boot-протоколом) */
#define USB_HID_REQ_GET_REPORT   0x01
#define USB_HID_REQ_SET_IDLE     0x0A
#define USB_HID_REQ_SET_PROTOCOL 0x0B

/* =========================================================
 * Типы дескрипторов
 * ========================================================= */

#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HID            0x21
#define USB_DESC_HID_REPORT     0x22

/* =========================================================
 * Классы устройств
 * ========================================================= */

#define USB_CLASS_HID            0x03
#define USB_HID_SUBCLASS_BOOT    0x01
#define USB_HID_PROTOCOL_KEYBOARD 0x01
#define USB_HID_PROTOCOL_MOUSE    0x02

/* =========================================================
 * Setup-пакет control-передачи (8 байт, как того требует спецификация)
 * ========================================================= */

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet_t;

/* =========================================================
 * Дескриптор устройства (18 байт)
 * ========================================================= */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;
