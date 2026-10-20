// driver.c — см. подробный разбор архитектуры в driver.h.

#include "driver.h"
#include "system/process/process.h"
#include "system/cpu/irq.h"

typedef struct {
    int in_use;
    uint32_t owner_pid;
    pci_device_t dev; // копия — не отдаём наружу указатель на внутренний pci_devices[]
} driver_claim_t;

typedef struct {
    int registered;
    uint32_t owner_pid;
    volatile uint32_t pending; // число непринятых срабатываний
    process_t *waiter;         // заблокирован в driver_irq_wait()
} user_irq_t;

static driver_claim_t g_claims[MAX_DRIVER_CLAIMS];
static user_irq_t g_user_irq[MAX_USER_IRQ];

// Тот же приём, что mailbox_lock()/mailbox_unlock() (mailbox.c) —
// driver_irq_notify() вызывается прямо из IRQ-обработчика (irq.c), поэтому
// мутация pending/waiter защищена cli/sti, не просто "однопоточностью".
static inline uint64_t driver_lock(void) {
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    asm volatile("cli");
    return flags;
}

static inline void driver_unlock(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

void driver_init(void) {
    for (int i = 0; i < MAX_DRIVER_CLAIMS; i++) g_claims[i].in_use = 0;
    for (int i = 0; i < MAX_USER_IRQ; i++) {
        g_user_irq[i].registered = 0;
        g_user_irq[i].pending = 0;
        g_user_irq[i].waiter = NULL;
    }
}

static int find_claim_slot(uint8_t bus, uint8_t device, uint8_t function) {
    for (int i = 0; i < MAX_DRIVER_CLAIMS; i++) {
        if (g_claims[i].in_use && g_claims[i].dev.bus == bus &&
            g_claims[i].dev.device == device && g_claims[i].dev.function == function)
            return i;
    }
    return -1;
}

int driver_owns_device(uint32_t pid, uint8_t bus, uint8_t device, uint8_t function) {
    int slot = find_claim_slot(bus, device, function);
    return slot >= 0 && g_claims[slot].owner_pid == pid;
}

int driver_is_registered(uint32_t pid) {
    for (int i = 0; i < MAX_DRIVER_CLAIMS; i++)
        if (g_claims[i].in_use && g_claims[i].owner_pid == pid) return 1;
    return 0;
}

int driver_claim_device(uint32_t pid, uint16_t vendor_id, uint16_t device_id,
                         pci_device_t *out) {
    if (!out) return -1;

    uint32_t count = pci_get_device_count();
    for (uint32_t i = 0; i < count; i++) {
        const pci_device_t *dev = pci_get_device(i);
        if (!dev || dev->vendor_id != vendor_id || dev->device_id != device_id) continue;
        // Уже кем-то заявлено — ищем следующее совпадение (на случай двух
        // одинаковых карт), а не отказываем сразу.
        if (find_claim_slot(dev->bus, dev->device, dev->function) >= 0) continue;

        int slot = -1;
        for (int s = 0; s < MAX_DRIVER_CLAIMS; s++) {
            if (!g_claims[s].in_use) { slot = s; break; }
        }
        if (slot < 0) return -2;

        g_claims[slot].in_use = 1;
        g_claims[slot].owner_pid = pid;
        g_claims[slot].dev = *dev;
        *out = *dev;
        return 0;
    }
    return -1;
}

int driver_mmio_allowed(uint32_t pid, uint64_t phys, uint64_t length) {
    if (length == 0) return 0;

    for (int i = 0; i < MAX_DRIVER_CLAIMS; i++) {
        if (!g_claims[i].in_use || g_claims[i].owner_pid != pid) continue;
        for (uint8_t b = 0; b < 6; b++) {
            pci_bar_t bar;
            if (pci_get_bar(&g_claims[i].dev, b, &bar) != 0) continue;
            if (bar.is_io || bar.size == 0) continue;
            if (phys >= bar.address && phys + length <= bar.address + bar.size) return 1;
        }
    }
    return 0;
}

int driver_io_port_allowed(uint32_t pid, uint16_t port, uint8_t width) {
    if (width == 0) return 0;

    for (int i = 0; i < MAX_DRIVER_CLAIMS; i++) {
        if (!g_claims[i].in_use || g_claims[i].owner_pid != pid) continue;
        for (uint8_t b = 0; b < 6; b++) {
            pci_bar_t bar;
            if (pci_get_bar(&g_claims[i].dev, b, &bar) != 0) continue;
            if (!bar.is_io || bar.size == 0) continue;
            if ((uint32_t)port >= bar.address && (uint32_t)port + width <= bar.address + bar.size) return 1;
        }
    }
    return 0;
}

int driver_release_device(uint32_t pid, uint8_t bus, uint8_t device, uint8_t function) {
    int slot = find_claim_slot(bus, device, function);
    if (slot < 0 || g_claims[slot].owner_pid != pid) return -1;
    g_claims[slot].in_use = 0;
    return 0;
}

void driver_release_process(uint32_t pid) {
    for (int i = 0; i < MAX_DRIVER_CLAIMS; i++)
        if (g_claims[i].in_use && g_claims[i].owner_pid == pid) g_claims[i].in_use = 0;

    uint64_t flags = driver_lock();
    for (int i = 0; i < MAX_USER_IRQ; i++) {
        if (g_user_irq[i].registered && g_user_irq[i].owner_pid == pid) {
            g_user_irq[i].registered = 0;
            g_user_irq[i].pending = 0;
            // Процесс и так уничтожается — будить некого, просто не оставляем
            // висячий указатель на умерший process_t.
            g_user_irq[i].waiter = NULL;
        }
    }
    driver_unlock(flags);
}

int driver_irq_register(uint32_t pid, uint8_t irq) {
    if (irq >= MAX_USER_IRQ) return -1;

    uint64_t flags = driver_lock();
    if (g_user_irq[irq].registered && g_user_irq[irq].owner_pid != pid) {
        driver_unlock(flags);
        return -2;
    }
    g_user_irq[irq].registered = 1;
    g_user_irq[irq].owner_pid = pid;
    g_user_irq[irq].pending = 0;
    g_user_irq[irq].waiter = NULL;
    driver_unlock(flags);

    irq_enable(irq); // снимает маску в PIC — irq_init() включает только 0/1/12
    return 0;
}

void driver_irq_notify(uint8_t irq) {
    if (irq >= MAX_USER_IRQ) return;

    uint64_t flags = driver_lock();
    if (!g_user_irq[irq].registered) {
        driver_unlock(flags);
        return;
    }
    g_user_irq[irq].pending++;
    process_t *waiter = g_user_irq[irq].waiter;
    g_user_irq[irq].waiter = NULL;
    driver_unlock(flags);

    // Будим ВНЕ критической секции — тот же порядок, что mailbox_send().
    if (waiter) waiter->state = PROCESS_READY;
}

int driver_irq_wait(uint32_t pid, uint8_t irq) {
    if (irq >= MAX_USER_IRQ) return -1;

    for (;;) {
        uint64_t flags = driver_lock();
        if (!g_user_irq[irq].registered || g_user_irq[irq].owner_pid != pid) {
            driver_unlock(flags);
            return -1;
        }
        if (g_user_irq[irq].pending > 0) {
            g_user_irq[irq].pending--;
            driver_unlock(flags);
            return 0;
        }

        // Настоящая блокировка без опроса — тот же приём и то же обоснование
        // не звать driver_unlock() перед schedule(), что в mailbox_recv()
        // (mailbox.c): schedule() рассчитан на вызов при IF=0, а IF
        // восстанавливается из СОБСТВЕННОГО сохранённого кадра процесса при
        // возврате в него.
        g_user_irq[irq].waiter = current_process;
        current_process->state = PROCESS_BLOCKED;
        schedule();
    }
}
