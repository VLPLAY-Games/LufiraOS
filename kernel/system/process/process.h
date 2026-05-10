#pragma once

#include "lib/types.h"
#include "system/cpu/tss.h"

// Состояния процесса
typedef enum {
    PROCESS_READY = 0,
    PROCESS_RUNNING = 1,
    PROCESS_BLOCKED = 2,
    PROCESS_TERMINATED = 3
} process_state_t;

// Контекст процесса (сохраняется при переключении)
typedef struct __attribute__((packed)) {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rsp;    // Stack pointer
    uint64_t rip;    // Instruction pointer
    uint64_t rflags; // CPU flags
    uint64_t cr3;    // Page table pointer
} process_context_t;

// Структура процесса
typedef struct process {
    uint32_t pid;                   // Process ID
    char name[32];                  // Process name
    process_state_t state;          // Current state
    process_context_t context;      // Saved context
    uint64_t stack_base;            // Base of kernel stack
    uint64_t stack_size;            // Size of kernel stack
    uint64_t page_table;            // CR3 value for this process (физический адрес PML4)
    struct process *next;           // Next process in list
} process_t;

// Глобальные функции
void process_init(void);
process_t* process_create(const char *name, void (*entry)(void));
void process_exit(void);
void schedule(void);
void switch_to_process(process_t *next);

// Ассемблерная функция переключения контекста
extern void context_switch(process_context_t *old_context, 
                          process_context_t *new_context);

// Текущий процесс
extern process_t *current_process;