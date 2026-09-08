#pragma once

#include "lib/types.h"
#include "system/cpu/tss.h"

#define KERNEL_HEAP_START       0xFFFF900000000000ULL  // Куча ядра
#define KERNEL_STACK_AREA_START 0xFFFF880000000000ULL  // Область стеков
#define KERNEL_STACK_SIZE       (16 * 1024)            // 16KB на процесс
#define MAX_PROCESSES           32
#define USER_STACK_AREA_START 0x0000700000000000ULL
#define USER_STACK_SIZE       (16 * 1024)  // 16KB


typedef enum {
    PROCESS_READY = 0,
    PROCESS_RUNNING = 1,
    PROCESS_BLOCKED = 2,
    PROCESS_SLEEPING = 3,
    PROCESS_TERMINATED = 4
} process_state_t;

typedef struct __attribute__((packed)) {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cr3;
} process_context_t;

typedef struct process {
    uint32_t pid;
    uint32_t ppid;          // 0 = нет родителя (например, до fork()/wait())
    char name[32];
    process_state_t state;
    uint64_t wakeup_tick;
    int exit_code;          // валиден, когда state == PROCESS_TERMINATED
    process_context_t context;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t ring0_stack;
    uint64_t ring0_stack_pages;
    uint64_t page_table;
    struct process *next;
} process_t;

void process_init(void);
process_t* process_create(const char *name, void (*entry)(void));
void process_exit(int exit_code);
void schedule(void);
void switch_to_process(process_t *next);
void process_reap(void);
void process_sleep(uint64_t milliseconds);
void process_ps(void);
int process_kill(uint32_t pid);

// Готовит новое адресное пространство и пользовательский стек для exec(),
// не трогая текущий образ proc (см. elf_exec_replace()).
int process_prepare_exec(
    process_t *proc,
    uint64_t *new_pml4_out,
    uint64_t *new_stack_out
);

// Подтверждает exec(): переключает proc на уже подготовленные (и
// заполненные) адресное пространство и стек, оставляя тот же PID.
int process_commit_exec(
    process_t *proc,
    uint64_t new_pml4,
    uint64_t new_stack,
    const char *name
);

extern uint64_t kernel_cr3;

extern void context_switch(process_context_t *old_context, 
                          process_context_t *new_context);

extern process_t *current_process;
extern process_t *process_list;