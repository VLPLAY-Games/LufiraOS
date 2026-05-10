#pragma once

#include "lib/types.h"
#include "lib/stddef.h"

// ACPI Root System Description Pointer
typedef struct __attribute__((packed)) {
    char     Signature[8];       // "RSD PTR "
    uint8_t  Checksum;
    char     OEMID[6];
    uint8_t  Revision;
    uint32_t RsdtAddress;        // 32-bit
} ACPI_RSDP_Rev1;

typedef struct __attribute__((packed)) {
    ACPI_RSDP_Rev1 rev1;
    uint32_t Length;
    uint64_t XsdtAddress;        // 64-bit
    uint8_t  ExtendedChecksum;
    uint8_t  Reserved[3];
} ACPI_RSDP_Rev2;

// System Description Table Header (common for RSDT/XSDT/FADT etc.)
typedef struct __attribute__((packed)) {
    char     Signature[4];
    uint32_t Length;
    uint8_t  Revision;
    uint8_t  Checksum;
    char     OEMID[6];
    char     OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
} ACPI_SDTHeader;

// FADT (Fixed ACPI Description Table)
typedef struct __attribute__((packed)) {
    ACPI_SDTHeader header;
    uint32_t FirmwareCtrl;
    uint32_t Dsdt;
    uint8_t  Reserved;
    uint8_t  Preferred_PM_Profile;
    uint16_t SCI_Int;
    uint32_t SMI_CMD;
    uint8_t  ACPI_ENABLE;
    uint8_t  ACPI_DISABLE;
    uint8_t  S4BIOS_REQ;
    uint8_t  PSTATE_CNT;
    uint32_t PM1a_EVT_BLK;
    uint32_t PM1b_EVT_BLK;
    uint32_t PM1a_CNT_BLK;
    uint32_t PM1b_CNT_BLK;
    uint32_t PM2_CNT_BLK;
    uint32_t PM_TMR_BLK;
    uint32_t GPE0_BLK;
    uint32_t GPE1_BLK;
    uint8_t  PM1_EVT_LEN;
    uint8_t  PM1_CNT_LEN;
    uint8_t  PM2_CNT_LEN;
    uint8_t  PM_TMR_LEN;
    uint8_t  GPE0_BLK_LEN;
    uint8_t  GPE1_BLK_LEN;
    uint8_t  GPE1_BASE;
    uint8_t  CST_CNT;
    uint16_t P_LVL2_LAT;
    uint16_t P_LVL3_LAT;
    uint16_t FlushSize;
    uint16_t FlushStride;
    uint8_t  DUTY_OFFSET;
    uint8_t  DUTY_WIDTH;
    uint8_t  DAY_ALRM;
    uint8_t  MON_ALRM;
    uint8_t  CENTURY;
    // ... есть ещё поля, но для shutdown они не нужны
} __attribute__((packed)) ACPI_FADT;

// SLP_TYPa values
#define ACPI_SLEEP_S5       0x00    // S5 (soft off) - обычно 0, но может быть иначе
#define ACPI_SLP_EN         0x2000  // SLP_EN bit

// Инициализация ACPI
int acpi_init(uint64_t rsdp_address);

// Shutdown
void acpi_shutdown(void);