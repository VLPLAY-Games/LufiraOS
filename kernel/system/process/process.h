#pragma once

#include "lib/types.h"
#include "system/cpu/tss.h"

typedef enum {
    PROCESS_READY = 0,
    PROCESS_RUNNING = 1,
    PROCESS_BLOCKED = 2,
    PROCESS_TERMINATED = 3
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
    char name[32];
    process_state_t state;
    process_context_t context;
    uint64_t stack_base;       // User stack (Ring 3)
    uint64_t stack_size;
    uint64_t ring0_stack;      // Kernel stack (Ring 0)
    uint64_t page_table;
    struct process *next;
} process_t;

void process_init(void);
process_t* process_create(const char *name, void (*entry)(void));
void process_exit(void);
void schedule(void);
void switch_to_process(process_t *next);
void process_reap(void);

extern uint64_t kernel_cr3;  // CR3 ядра

extern void context_switch(process_context_t *old_context, 
                          process_context_t *new_context);

extern process_t *current_process;