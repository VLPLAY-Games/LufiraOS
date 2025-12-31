#include <stdint.h>
#include <stdarg.h>
#include "drivers/keyboard.h"
#include "drivers/console.h"
#include "shell/shell.h"

// --- Структуры данных ---
typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
} BootInfo;

// --- Точка входа ядра ---
__attribute__((section(".text.prologue")))
void _start(BootInfo* bi) {
    initialize_console(bi);
    clear_screen();
    keyboard_init();
    
    current_y = 1;
    
    current_color = 0xAAAAAA;
    printf("LufiraOS Kernel v1.0 Boot Sequence:\n");
    printf("-----------------------------------\n\n");
    
    current_color = 0xFFFFFF;
    printf("Detected Resolution: %d x %d\n", bi->HorizontalResolution, bi->VerticalResolution);
    printf("Framebuffer Address: 0x%x\n", bi->FrameBufferBase);
    printf("Characters Grid: %d x %d\n\n", screen_width_chars, screen_height_chars);
    
    current_color = 0x55FF55;
    printf("Keyboard initialized: OK\n");
    printf("System status: OK.\n");
    
    current_color = 0xFFFFFF;
    show_prompt();
    
    while (1) {
        // Проверяем клавиатуру
        keyboard_handler();
        __asm__ volatile ("pause");
    }
}