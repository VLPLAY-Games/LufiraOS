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

#define USB_CLASS_MSD            0x08 // Mass Storage
#define USB_MSD_SUBCLASS_SCSI    0x06 // SCSI transparent command set
#define USB_MSD_PROTOCOL_BOT     0x50 // Bulk-Only Transport

/* =========================================================
 * USB Mass Storage — Bulk-Only Transport (CBW/CSW) поверх пары bulk
 * endpoint'ов; сами SCSI-команды (opcode'ы) HCD-независимы так же, как и
 * остальной этот заголовок.
 * ========================================================= */

#define USB_BOT_CBW_SIGNATURE 0x43425355u // "USBC"
#define USB_BOT_CSW_SIGNATURE 0x53425355u // "USBS"

#define USB_BOT_FLAG_DATA_IN  0x80
#define USB_BOT_FLAG_DATA_OUT 0x00

#define USB_BOT_STATUS_OK          0
#define USB_BOT_STATUS_FAIL        1
#define USB_BOT_STATUS_PHASE_ERROR 2

typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;   // бит 7: 1 = Data-In, 0 = Data-Out
    uint8_t  bCBWLUN;      // биты 0-3
    uint8_t  bCBWCBLength; // биты 0-4 — длина CBWCB
    uint8_t  CBWCB[16];
} usb_bot_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus; // USB_BOT_STATUS_*
} usb_bot_csw_t;

/* SCSI command opcodes — только то, что нужно для базового блочного
 * чтения/записи (без файловой системы поверх, см. план). */
#define SCSI_CMD_TEST_UNIT_READY 0x00
#define SCSI_CMD_INQUIRY         0x12
#define SCSI_CMD_READ_CAPACITY10 0x25
#define SCSI_CMD_READ10          0x28
#define SCSI_CMD_WRITE10         0x2A

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

/* =========================================================
 * Дескриптор конфигурации (9 байт) — первый ответ содержит только
 * заголовок; wTotalLength сообщает полный размер конфигурации со всеми
 * вложенными interface/HID/endpoint-дескрипторами, которые идут следом
 * (их нужно запрашивать отдельно, повторным GET_DESCRIPTOR на всю длину).
 * ========================================================= */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress; // бит 7: 1 = IN
    uint8_t  bmAttributes;     // биты 1:0: 11 = interrupt
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;
