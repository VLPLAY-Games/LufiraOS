# Правила для загрузчика

# Исходные файлы загрузчика (рекурсивно по всем подпапкам)
BOOTLOADER_SOURCES := $(shell find $(BOOTLOADER_DIR) -name '*.c')
BOOTLOADER_OBJECTS := $(patsubst $(BOOTLOADER_DIR)/%.c,$(BUILD_DIR)/boot/%.o,$(BOOTLOADER_SOURCES))
BOOTLOADER_SO := $(BUILD_DIR)/boot.so

$(BUILD_DIR)/boot/%.o: $(BOOTLOADER_DIR)/%.c
	@echo "  CC    $<"
	@mkdir -p $(dir $@)
	$(CC) $(BOOTLOADER_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/boot.so: $(BOOTLOADER_OBJECTS)
	@echo "  LD    $@"
	$(LD) $(BOOTLOADER_LDFLAGS) -o $@ $(BOOTLOADER_OBJECTS) -lefi -lgnuefi

$(BUILD_DIR)/BOOTX64.EFI: $(BUILD_DIR)/boot.so
	@echo "  OBJCOPY $@"
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
	-j .dynsym -j .rel -j .rela -j .reloc \
	--target=efi-app-$(ARCH) $< $@