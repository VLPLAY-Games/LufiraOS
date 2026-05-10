#include "syscall.h"
#include "drivers/console/console.h"
#include "system/process/process.h"
#include "system/timer/pit.h"
#include "system/cpu/gdt.h"

typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

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
    return 0;
}

static uint64_t sys_exit(uint64_t exit_code, uint64_t unused1, uint64_t unused2,
                         uint64_t unused3, uint64_t unused4) {
    (void)exit_code;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    
    printf("\n[SYSCALL] Process %u exit(%u)\n", 
           current_process->pid, (uint32_t)exit_code);
    
    process_exit();
    while (1) __asm__("hlt");
}

static uint64_t sys_getpid(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    return current_process ? current_process->pid : 0;
}

static uint64_t sys_yield(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    schedule();
    return 0;
}

static uint64_t sys_gettick(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    return pit_get_ticks();
}

static syscall_fn_t syscall_table[256] = {
    [SYS_WRITE]   = sys_write,
    [SYS_READ]    = sys_read,
    [SYS_EXIT]    = sys_exit,
    [SYS_GETPID]  = sys_getpid,
    [SYS_YIELD]   = sys_yield,
    [SYS_GETTICK] = sys_gettick,
};

void syscall_init(void) {
    // Настраиваем STAR MSR (0xC0000081)
    // Биты 47:32 - селектор для kernel CS (0x08)
    // Биты 63:48 - селектор для user CS (0x28 | 3 = 0x2B)
    // На самом деле, при возврате через sysretq:
    //   CS = (STAR[63:48] + 16) | 3 = (0x28 + 0x10) | 3 = 0x3B
    //   SS = (STAR[63:48] + 8) | 3  = (0x28 + 0x08) | 3 = 0x33
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32) | 
                    ((uint64_t)GDT_USER_CODE << 48);
    asm volatile("wrmsr" : : "c"(0xC0000081), "a"((uint32_t)star), 
                 "d"((uint32_t)(star >> 32)));
    
    // Настраиваем LSTAR MSR (0xC0000082) - адрес обработчика syscall
    extern void syscall_entry(void);
    uint64_t handler = (uint64_t)&syscall_entry;
    asm volatile("wrmsr" : : "c"(0xC0000082), "a"((uint32_t)handler),
                 "d"((uint32_t)(handler >> 32)));
    
    // Настраиваем FMASK MSR (0xC0000084) - маска для RFLAGS
    // Очищаем IF (бит 9) при входе в syscall
    uint64_t fmask = 0x200;
    asm volatile("wrmsr" : : "c"(0xC0000084), "a"((uint32_t)fmask),
                 "d"((uint32_t)(fmask >> 32)));
    
    // Включаем SCE в EFER MSR
    uint64_t efer;
    uint32_t efer_low, efer_high;
    asm volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(0xC0000080));
    efer = ((uint64_t)efer_high << 32) | efer_low;
    efer |= 1;  // SCE bit
    efer_low = (uint32_t)efer;
    efer_high = (uint32_t)(efer >> 32);
    asm volatile("wrmsr" : : "c"(0xC0000080), "a"(efer_low), "d"(efer_high));
    
    printf("[SYSCALL] System calls initialized (Ring 0/3 ready)\n");
}

uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    if (syscall_num >= 256 || !syscall_table[syscall_num]) {
        printf("[SYSCALL] Unknown syscall: %u\n", (uint32_t)syscall_num);
        return -1;
    }
    
    return syscall_table[syscall_num](arg1, arg2, arg3, arg4, arg5);
}