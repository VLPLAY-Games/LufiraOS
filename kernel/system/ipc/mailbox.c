// mailbox.c — см. подробный разбор архитектуры в mailbox.h.

#include "mailbox.h"
#include "system/process/process.h"
#include "lib/string.h"

static inline uint64_t mailbox_lock(void) {
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    asm volatile("cli");
    return flags;
}

static inline void mailbox_unlock(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

void mailbox_init(ipc_mailbox_t *mb) {
    mb->head = mb->tail = mb->count = 0;
    mb->waiter = NULL;
}

int mailbox_send(uint32_t dest_pid, uint32_t sender_pid, const void *data, uint32_t len) {
    if (len > MAX_IPC_MSG_PAYLOAD) return -1;

    process_t *dest = process_find_by_pid(dest_pid);
    if (!dest) return -1;

    ipc_mailbox_t *mb = &dest->mailbox;
    uint64_t flags = mailbox_lock();

    if (mb->count >= MAX_IPC_MAILBOX_SLOTS) {
        mailbox_unlock(flags);
        return -2;
    }

    ipc_msg_t *slot = &mb->slots[mb->tail];
    slot->sender_pid = sender_pid;
    slot->len = len;
    if (len > 0) memcpy(slot->payload, data, len);
    mb->tail = (mb->tail + 1) % MAX_IPC_MAILBOX_SLOTS;
    mb->count++;

    process_t *waiter = mb->waiter;
    mb->waiter = NULL;
    mailbox_unlock(flags);

    // Будим ВНЕ критической секции (тот же порядок, что у pipe_wake()) —
    // сама запись waiter->state не нуждается в cli/sti: schedule()
    // планировщика просматривает process_list независимо, лишний тик до
    // того, как PROCESS_READY станет видно, не ломает корректность, только
    // задержка на деле не больше одного тика.
    if (waiter) waiter->state = PROCESS_READY;

    return 0;
}

int mailbox_recv(void *out, int blocking) {
    if (!current_process || !out) return -1;
    ipc_mailbox_t *mb = &current_process->mailbox;

    for (;;) {
        uint64_t flags = mailbox_lock();
        if (mb->count > 0) {
            ipc_msg_t *slot = &mb->slots[mb->head];
            memcpy(out, slot, sizeof(ipc_msg_t));
            mb->head = (mb->head + 1) % MAX_IPC_MAILBOX_SLOTS;
            mb->count--;
            mailbox_unlock(flags);
            return 0;
        }
        mailbox_unlock(flags);

        if (!blocking) return -1;

        // Тот же приём, что и pipe_read()/console_input_read(): уступаем
        // CPU, пока mailbox_send() (другой процесс ИЛИ прямо IRQ-обработчик
        // ввода, см. mailbox.h) не положит что-то и не разбудит нас обратно.
        current_process->mailbox.waiter = current_process;
        current_process->state = PROCESS_BLOCKED;
        schedule();
    }
}
