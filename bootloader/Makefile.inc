# Правила для загрузчика

# Исходные файлы загрузчика
BOOTLOADER_SRC := $(BOOTLOADER_DIR)/bootloader.c
BOOTLOADER_OBJ := $(BUILD_DIR)/bootloader.o
BOOTLOADER_SO := $(BUILD_DIR)/bootloader.so

# Сборка загрузчика
$(BOOTLOADER_OBJ): $(BOOTLOADER_SRC)
	$(CC) $(BOOTLOADER_CFLAGS) -c -o $@ $<

$(BOOTLOADER_SO): $(BOOTLOADER_OBJ)
	$(LD) $(BOOTLOADER_LDFLAGS) -o $@ $< -lgnuefi -lefi

$(BUILD_DIR)/BOOTX64.EFI: $(BOOTLOADER_SO)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
	-j .dynsym -j .rel -j .rela -j .reloc \
	--target=efi-app-$(ARCH) $< $@