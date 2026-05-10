#pragma once

#include "lib/types.h"

int disk_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer);
int disk_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer);
