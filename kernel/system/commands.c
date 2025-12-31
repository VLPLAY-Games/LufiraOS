#include "commands.h"
#include "../drivers/console.h"
#include "../shell/shell.h"

void command_help(void) {
    printf("\nAvailable commands:\n");
    printf("  help      - Show this help\n");
    printf("  clear     - Clear screen\n");
    printf("  reboot    - Reboot system\n");
    printf("  shutdown  - Shutdown system\n");
    printf("  version   - Show kernel version\n");
    printf("  echo      - Echo text back\n");
}

void command_clear(void) {
    clear_screen();
    show_prompt();
}

void command_reboot(void) {
    printf("\nRebooting system...\n");
    // Перезагрузка через 8042 контроллер
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    
    // Запасной метод через ACPI
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    
    // Если все еще работает
    printf("Reboot failed. Please restart manually.\n");
}

void command_shutdown(void) {
    printf("\nShutting down system...\n");
    
    // Попытка выключения через ACPI (QEMU и современные системы)
    // Для QEMU используем порт 0x604
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    
    // Запасной метод для QEMU (более старый)
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x3400), "Nd"((uint16_t)0x604));
    
    // Метод для Bochs и старых версий QEMU
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    
    // Метод через порт 0x64 (8042 контроллер) - для реального железа
    __asm__ volatile (
        "mov $0xFE, %%al\n"
        "out %%al, $0x64\n"
        : : : "eax"
    );
    
    // Если все еще работает
    printf("Shutdown command sent. System may require manual power off.\n");
}

void command_version(void) {
    printf("\nLufiraOS Kernel v0.1\n");
    printf("Built: %s %s\n", __DATE__, __TIME__);
    printf("Architecture: x86_64\n");
}