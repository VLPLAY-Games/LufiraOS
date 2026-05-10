#include "syscall.h"
#include "drivers/console/console.h"
#include "system/process/process.h"
#include "system/timer/pit.h"

// Таблица системных вызовов
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// Реализации системных вызовов
static uint64_t sys_write(uint64_t fd, uint64_t buffer, uint64_t length, 
                          uint64_t unused1, uint64_t unused2) {
    (void)fd;
    (void)unused1;
    (void)unused2;
    
    if (buffer == 0 || length == 0) return 0;
    
    const char *str = (const char *)buffer;
    uint64_t printed = 0;
    
    for (uint64_t i = 0; i < length; i++) {
        if (str[i] == '\0') break;
        put_char(str[i]);
        printed++;
    }
    
    return printed;
}

static uint64_t sys_read(uint64_t fd, uint64_t buffer, uint64_t length,
                         uint64_t unused1, uint64_t unused2) {
    (void)fd;
    (void)buffer;
    (void)length;
    (void)unused1;
    (void)unused2;
    
    // Пока не реализовано
    return 0;
}

static uint64_t sys_exit(uint64_t exit_code, uint64_t unused1, uint64_t unused2,
                         uint64_t unused3, uint64_t unused4) {
    (void)exit_code;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    
    printf("\n[SYSCALL] Process %u called exit(%u)\n", 
           current_process->pid, (uint32_t)exit_code);
    
    process_exit();
    
    // Никогда не возвращаемся
    while (1) __asm__("hlt");
}

static uint64_t sys_getpid(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    
    return current_process ? current_process->pid : 0;
}

static uint64_t sys_yield(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    
    schedule();
    return 0;
}

static uint64_t sys_gettick(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    
    return pit_get_ticks();
}

// Таблица системных вызовов
static syscall_fn_t syscall_table[256] = {
    [SYS_WRITE]   = sys_write,
    [SYS_READ]    = sys_read,
    [SYS_EXIT]    = sys_exit,
    [SYS_GETPID]  = sys_getpid,
    [SYS_YIELD]   = sys_yield,
    [SYS_GETTICK] = sys_gettick,
};

// Инициализация syscall (настройка MSR)
void syscall_init(void) {
    // Настраиваем STAR MSR (0xC0000081)
    // Биты 47:32 - селектор для kernel CS (0x08)
    // Биты 63:48 - селектор для user CS (будет 0x1B когда сделаем Ring 3)
    // Пока используем тот же селектор что и для ядра (0x08)
    uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x08 << 48);
    asm volatile("wrmsr" : : "c"(0xC0000081), "a"((uint32_t)star), 
                 "d"((uint32_t)(star >> 32)));
    
    // Настраиваем LSTAR MSR (0xC0000082) - адрес обработчика syscall
    extern void syscall_entry(void);
    uint64_t handler = (uint64_t)&syscall_entry;
    asm volatile("wrmsr" : : "c"(0xC0000082), "a"((uint32_t)handler),
                 "d"((uint32_t)(handler >> 32)));
    
    // Настраиваем FMASK MSR (0xC0000084) - маска для RFLAGS
    // Очищаем IF (бит 9) при входе в syscall
    uint64_t fmask = 0x200;  // Clear interrupt flag
    asm volatile("wrmsr" : : "c"(0xC0000084), "a"((uint32_t)fmask),
                 "d"((uint32_t)(fmask >> 32)));
    
    // Включаем SCE (System Call Extension) в EFER MSR
    uint64_t efer;
    asm volatile("rdmsr" : "=a"(*(uint32_t*)&efer), "=d"(*((uint32_t*)&efer + 1)) 
                 : "c"(0xC0000080));
    efer |= 1;  // SCE bit
    asm volatile("wrmsr" : : "c"(0xC0000080), "a"((uint32_t)efer),
                 "d"((uint32_t)(efer >> 32)));
    
    printf("[SYSCALL] System call interface initialized\n");
}

// Обработчик системного вызова (вызывается из ассемблера)
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    if (syscall_num >= 256 || !syscall_table[syscall_num]) {
        printf("[SYSCALL] Unknown syscall: %u\n", (uint32_t)syscall_num);
        return -1;
    }
    
    return syscall_table[syscall_num](arg1, arg2, arg3, arg4, arg5);
}