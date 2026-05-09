#include "tables.h"

#define ACPI_10_TABLE_GUID {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x0, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}

VOID ScanSystemTables(BootInfo *bi) {
    bi->RsdpAddress = bi->SmbiosAddress = 0;
    
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_GUID Acpi2Guid = ACPI_20_TABLE_GUID;
        EFI_GUID Acpi1Guid = ACPI_10_TABLE_GUID;
        EFI_GUID SmbiosGuid = SMBIOS_TABLE_GUID;
        EFI_GUID Smbios3Guid = SMBIOS3_TABLE_GUID;
        
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi2Guid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Acpi1Guid) == 0)
            bi->RsdpAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
            
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &SmbiosGuid) == 0 ||
            CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &Smbios3Guid) == 0)
            bi->SmbiosAddress = (uint64_t)gST->ConfigurationTable[i].VendorTable;
    }
}