#include "ac97.h"

#include "drivers/pci/pci.h"
#include "system/mm/pmm.h"
#include "drivers/console/console.h"

/* ======================================================================== */
/* Port I/O                                                                  */
/* ======================================================================== */

static inline void ac97_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline void ac97_out16(uint16_t port, uint16_t value)
{
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline void ac97_out32(uint16_t port, uint32_t value)
{
    __asm__ volatile (
        "outl %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t ac97_in8(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static inline uint16_t ac97_in16(uint16_t port)
{
    uint16_t value;

    __asm__ volatile (
        "inw %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static inline uint32_t ac97_in32(uint16_t port)
{
    uint32_t value;

    __asm__ volatile (
        "inl %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

/* ======================================================================== */
/* Driver state                                                              */
/* ======================================================================== */

static int ac97_available = 0;

static uint16_t ac97_nam_base = 0;
static uint16_t ac97_nabm_base = 0;

static uint8_t ac97_volume = 100;
static uint32_t ac97_sample_rate = 48000;

static ac97_codec_info_t ac97_codec;

/*
 * One physical page for BDL itself.
 */
static uint64_t ac97_bdl_phys = 0;

/*
 * 32 independent physical DMA pages.
 *
 * This avoids requiring physically contiguous memory.
 */
static uint64_t ac97_dma_phys[AC97_BDL_ENTRIES];

/*
 * Because LufiraOS currently identity maps low physical memory,
 * physical == virtual for these pages.
 */
static ac97_bdl_entry_t* ac97_bdl = NULL;

/* ======================================================================== */
/* Small helpers                                                             */
/* ======================================================================== */

static void ac97_memzero(void* ptr, uint32_t size)
{
    uint8_t* p = (uint8_t*)ptr;

    for (uint32_t i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void ac97_memcpy(
    void* dst,
    const void* src,
    uint32_t size
)
{
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    for (uint32_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

/*
 * Very small busy delay.
 *
 * Used only during hardware initialization.
 */
static void ac97_delay(void)
{
    for (volatile uint32_t i = 0; i < 100000; i++) {
        __asm__ volatile ("pause");
    }
}

/* ======================================================================== */
/* Codec access                                                              */
/* ======================================================================== */

uint16_t ac97_read_codec(uint8_t reg)
{
    if (!ac97_available && ac97_nam_base == 0) {
        return 0xFFFF;
    }

    return ac97_in16(
        (uint16_t)(ac97_nam_base + reg)
    );
}

void ac97_write_codec(uint8_t reg, uint16_t value)
{
    if (ac97_nam_base == 0) {
        return;
    }

    ac97_out16(
        (uint16_t)(ac97_nam_base + reg),
        value
    );
}

/* ======================================================================== */
/* PCI detection                                                             */
/* ======================================================================== */

static int ac97_find_controller(void)
{
    /*
     * Search by PCI class:
     *
     * class    04 = Multimedia
     * subclass 01 = Audio device
     */
    const pci_device_t* dev =
        pci_find_class(0x04, 0x01);

    if (dev == NULL) {
        printf("[AC97] Controller not found\n");
        return 0;
    }

    printf(
        "[AC97] Controller found at %u:%u.%u\n",
        dev->bus,
        dev->device,
        dev->function
    );

    printf(
        "[AC97] vendor=%04X device=%04X\n",
        dev->vendor_id,
        dev->device_id
    );

    pci_bar_t bar0;
    pci_bar_t bar1;

    if (pci_get_bar(dev, 0, &bar0) != 0) {
        printf("[AC97] Failed to read BAR0\n");
        return 0;
    }

    if (pci_get_bar(dev, 1, &bar1) != 0) {
        printf("[AC97] Failed to read BAR1\n");
        return 0;
    }

    if (!bar0.is_io || !bar1.is_io) {
        printf("[AC97] Expected I/O BARs\n");
        return 0;
    }

    if (bar0.address > 0xFFFF || bar1.address > 0xFFFF) {
        printf("[AC97] Invalid I/O BAR address\n");
        return 0;
    }

    /*
     * For Intel ICH AC'97:
     *
     * BAR0 = Native Audio Mixer
     * BAR1 = Native Audio Bus Master
     */
    ac97_nam_base = (uint16_t)bar0.address;
    ac97_nabm_base = (uint16_t)bar1.address;

    printf(
        "[AC97] NAM  I/O base = %04X\n",
        ac97_nam_base
    );

    printf(
        "[AC97] NABM I/O base = %04X\n",
        ac97_nabm_base
    );

    /*
     * AC'97 controller needs I/O + bus mastering.
     */
    pci_enable_io(dev);
    pci_enable_bus_master(dev);

    /*
     * Disable PCI interrupts for now.
     *
     * We poll DMA status.
     */
    pci_disable_interrupts(dev);

    return 1;
}

/* ======================================================================== */
/* Controller reset                                                          */
/* ======================================================================== */

static int ac97_controller_reset(void)
{
    printf("[AC97] Controller reset...\n");

    /*
     * Cold reset.
     */
    uint32_t control =
        ac97_in32(
            (uint16_t)(ac97_nabm_base + AC97_BM_GLOB_CNT)
        );

    control |= AC97_GLOB_COLD_RESET;

    ac97_out32(
        (uint16_t)(ac97_nabm_base + AC97_BM_GLOB_CNT),
        control
    );

    ac97_delay();

    /*
     * Bring the controller out of reset.
     *
     * Bit 1 = cold reset released.
     */
    ac97_out32(
        (uint16_t)(ac97_nabm_base + AC97_BM_GLOB_CNT),
        AC97_GLOB_COLD_RESET
    );

    ac97_delay();

    /*
     * Poll for primary codec ready.
     */
    for (uint32_t i = 0; i < 1000; i++) {

        uint32_t status =
            ac97_in32(
                (uint16_t)(ac97_nabm_base + AC97_BM_GLOB_STA)
            );

        if (status & AC97_GLOB_PCR) {
            printf("[AC97] Primary codec ready\n");
            return 1;
        }

        ac97_delay();
    }

    printf("[AC97] Codec did not become ready\n");

    return 0;
}

/* ======================================================================== */
/* Codec initialization                                                      */
/* ======================================================================== */

static int ac97_codec_init(void)
{
    printf("[AC97] Resetting codec...\n");

    /*
     * Writing anything to codec register 0 resets
     * the codec mixer registers.
     */
    ac97_write_codec(AC97_REG_RESET, 0);

    ac97_delay();

    /*
     * Read codec information.
     */
    ac97_codec.vendor_id1 =
        ac97_read_codec(AC97_REG_VENDOR_ID1);

    ac97_codec.vendor_id2 =
        ac97_read_codec(AC97_REG_VENDOR_ID2);

    ac97_codec.ext_audio_id =
        ac97_read_codec(AC97_REG_EXT_AUDIO_ID);

    ac97_codec.ext_audio_ctrl =
        ac97_read_codec(AC97_REG_EXT_AUDIO_CTRL);

    printf(
        "[AC97] Codec vendor = %04X:%04X\n",
        ac97_codec.vendor_id1,
        ac97_codec.vendor_id2
    );

    printf(
        "[AC97] Extended Audio ID = %04X\n",
        ac97_codec.ext_audio_id
    );

    /*
     * Enable Variable Rate Audio.
     *
     * This allows us to program the front DAC sample rate.
     */
    ac97_codec.ext_audio_ctrl |= AC97_EACS_VRA;

    ac97_write_codec(
        AC97_REG_EXT_AUDIO_CTRL,
        ac97_codec.ext_audio_ctrl
    );

    ac97_delay();

    /*
     * Default 48 kHz.
     */
    ac97_write_codec(
        AC97_REG_PCM_FRONT_RATE,
        48000
    );

    /*
     * Maximum volume.
     *
     * AC'97 uses attenuation:
     *   0 = maximum
     *   31 = minimum
     */
    ac97_write_codec(
        AC97_REG_MASTER_VOLUME,
        0
    );

    ac97_write_codec(
        AC97_REG_PCM_OUT_VOLUME,
        0
    );

    /*
     * Make sure DAC is powered.
     */
    uint16_t power =
        ac97_read_codec(AC97_REG_POWERDOWN);

    power &= ~(AC97_POWER_DAC);

    ac97_write_codec(
        AC97_REG_POWERDOWN,
        power
    );

    printf("[AC97] Codec initialized\n");

    return 1;
}

/* ======================================================================== */
/* DMA memory                                                                */
/* ======================================================================== */

static int ac97_alloc_dma(void)
{
    printf("[AC97] Allocating DMA buffers...\n");

    /*
     * BDL itself.
     */
    ac97_bdl_phys = pmm_alloc_page();

    if (ac97_bdl_phys == 0) {
        printf("[AC97] Failed to allocate BDL page\n");
        return 0;
    }

    /*
     * AC'97 uses 32-bit physical addresses in BDL.
     */
    if (ac97_bdl_phys > 0xFFFFFFFFULL) {
        printf("[AC97] BDL above 4GB\n");
        return 0;
    }

    ac97_bdl =
        (ac97_bdl_entry_t*)(uintptr_t)ac97_bdl_phys;

    ac97_memzero(
        ac97_bdl,
        AC97_BDL_ENTRIES * sizeof(ac97_bdl_entry_t)
    );

    /*
     * 32 audio pages.
     */
    for (uint32_t i = 0; i < AC97_BDL_ENTRIES; i++) {

        ac97_dma_phys[i] =
            pmm_alloc_page();

        if (ac97_dma_phys[i] == 0) {
            printf(
                "[AC97] Failed to allocate DMA page %u\n",
                i
            );

            return 0;
        }

        if (ac97_dma_phys[i] > 0xFFFFFFFFULL) {
            printf(
                "[AC97] DMA page %u above 4GB\n",
                i
            );

            return 0;
        }

        /*
         * Clear audio memory.
         */
        ac97_memzero(
            (void*)(uintptr_t)ac97_dma_phys[i],
            AC97_DMA_PAGE_SIZE
        );

        /*
         * AC'97 samples are 16-bit.
         *
         * Stereo:
         *
         * 4096 bytes / 2 bytes = 2048 samples
         *
         * 2048 samples = 1024 stereo frames.
         */
        ac97_bdl[i].buffer =
            (uint32_t)ac97_dma_phys[i];

        ac97_bdl[i].samples =
            AC97_DMA_PAGE_SIZE / 2;

        ac97_bdl[i].flags = 0;
    }

    printf(
        "[AC97] DMA buffers ready (%u pages)\n",
        AC97_BDL_ENTRIES
    );

    return 1;
}

/* ======================================================================== */
/* Volume                                                                    */
/* ======================================================================== */

void ac97_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }

    ac97_volume = volume;

    /*
     * AC'97 attenuation range:
     *
     * 100% -> 0 dB attenuation
     *   0% -> mute
     *
     * 5 bits, where 31 is maximum attenuation.
     */
    uint8_t attenuation =
        (uint8_t)(31 - ((uint32_t)volume * 31) / 100);

    uint16_t value =
        ((uint16_t)attenuation << 8) |
        attenuation;

    if (volume == 0) {
        value |= AC97_VOLUME_MUTE;
    }

    ac97_write_codec(
        AC97_REG_MASTER_VOLUME,
        value
    );

    ac97_write_codec(
        AC97_REG_PCM_OUT_VOLUME,
        value
    );
}

uint8_t ac97_get_volume(void)
{
    return ac97_volume;
}

/* ======================================================================== */
/* Sample rate                                                                */
/* ======================================================================== */

void ac97_set_sample_rate(uint32_t rate)
{
    if (rate < 8000) {
        rate = 8000;
    }

    if (rate > 96000) {
        rate = 96000;
    }

    ac97_sample_rate = rate;

    ac97_write_codec(
        AC97_REG_PCM_FRONT_RATE,
        (uint16_t)rate
    );
}

uint32_t ac97_get_sample_rate(void)
{
    return ac97_sample_rate;
}

/* ======================================================================== */
/* PCM OUT DMA                                                               */
/* ======================================================================== */

static void ac97_reset_pcm_out(void)
{
    uint16_t port =
        (uint16_t)(ac97_nabm_base + AC97_PO_CR);

    /*
     * Stop DMA.
     */
    uint8_t control =
        ac97_in8(port);

    control &= ~AC97_CR_RPBM;

    ac97_out8(
        port,
        control
    );

    /*
     * Reset PCM OUT channel.
     */
    ac97_out8(
        port,
        AC97_CR_RR
    );

    /*
     * Wait until reset bit clears.
     */
    for (uint32_t i = 0; i < 100000; i++) {

        if (!(ac97_in8(port) & AC97_CR_RR)) {
            break;
        }

        __asm__ volatile ("pause");
    }
}

static void ac97_clear_pcm_status(void)
{
    ac97_out16(
        (uint16_t)(ac97_nabm_base + AC97_PO_SR),
        AC97_SR_W1C_MASK
    );
}

/* ======================================================================== */
/* PCM playback                                                              */
/* ======================================================================== */

int ac97_play_pcm_stereo(
    const int16_t* samples,
    uint32_t frames
)
{
    if (!ac97_available) {
        return 0;
    }

    if (samples == NULL || frames == 0) {
        return 0;
    }

    /*
     * Maximum:
     *
     * 32 pages * 1024 stereo frames
     */
    uint32_t max_frames =
        AC97_BDL_ENTRIES *
        (AC97_DMA_PAGE_SIZE / 4);

    if (frames > max_frames) {
        frames = max_frames;
    }

    /*
     * Stop previous playback.
     */
    ac97_reset_pcm_out();

    ac97_clear_pcm_status();

    /*
     * Copy PCM into DMA pages.
     */
    uint32_t copied_frames = 0;

    for (uint32_t page = 0;
         page < AC97_BDL_ENTRIES && copied_frames < frames;
         page++) {

        uint32_t page_frames =
            AC97_DMA_PAGE_SIZE / 4;

        uint32_t remaining =
            frames - copied_frames;

        if (page_frames > remaining) {
            page_frames = remaining;
        }

        /*
         * Stereo = 2 int16_t per frame.
         */
        uint32_t bytes =
            page_frames * 4;

        ac97_memcpy(
            (void*)(uintptr_t)ac97_dma_phys[page],
            samples + copied_frames * 2,
            bytes
        );

        /*
         * AC'97 BDL count is number of 16-bit samples.
         */
        ac97_bdl[page].buffer =
            (uint32_t)ac97_dma_phys[page];

        ac97_bdl[page].samples =
            (uint16_t)(page_frames * 2);

        ac97_bdl[page].flags = 0;

        copied_frames += page_frames;
    }

    uint32_t entries =
        (copied_frames +
         (AC97_DMA_PAGE_SIZE / 4) - 1) /
        (AC97_DMA_PAGE_SIZE / 4);

    if (entries == 0) {
        return 0;
    }

    /*
     * Last descriptor:
     *
     * BUP = buffer underrun / last buffer,
     * meaning stop after this entry.
     */
    ac97_bdl[entries - 1].flags =
        AC97_BDL_BUP |
        AC97_BDL_IOC;

    /*
     * Program BDL physical address.
     */
    ac97_out32(
        (uint16_t)(ac97_nabm_base + AC97_PO_BDBAR),
        (uint32_t)ac97_bdl_phys
    );

    /*
     * Last Valid Index.
     */
    ac97_out8(
        (uint16_t)(ac97_nabm_base + AC97_PO_LVI),
        (uint8_t)(entries - 1)
    );

    /*
     * Start DMA.
     */
    ac97_out8(
        (uint16_t)(ac97_nabm_base + AC97_PO_CR),
        AC97_CR_RPBM
    );

    /*
     * Wait until DMA channel stops.
     *
     * This is deliberately polling for now.
     */
    for (;;) {

        uint16_t status =
            ac97_in16(
                (uint16_t)(ac97_nabm_base + AC97_PO_SR)
            );

        if (status & AC97_SR_DCH) {
            break;
        }

        __asm__ volatile ("pause");
    }

    /*
     * Stop channel and clear status.
     */
    ac97_out8(
        (uint16_t)(ac97_nabm_base + AC97_PO_CR),
        0
    );

    ac97_clear_pcm_status();

    return 1;
}

/* ======================================================================== */
/* Tone generation                                                           */
/* ======================================================================== */

static uint32_t ac97_sine_phase = 0;

/*
 * Very cheap integer approximation of a sine waveform.
 *
 * No libm required.
 */
static int16_t ac97_wave(uint32_t phase)
{
    uint16_t p =
        (uint16_t)(phase >> 16);

    int32_t value;

    if (p < 16384) {
        value = p;
    }
    else if (p < 32768) {
        value = 32768 - p;
    }
    else if (p < 49152) {
        value = -(int32_t)(p - 32768);
    }
    else {
        value = -(int32_t)(65536 - p);
    }

    /*
     * Triangle wave.
     */
    value *= 2;

    if (value > 32767) {
        value = 32767;
    }

    if (value < -32768) {
        value = -32768;
    }

    return (int16_t)value;
}

int ac97_play_tone(
    uint32_t frequency,
    uint32_t duration_ms
)
{
    if (!ac97_available) {
        return 0;
    }

    if (frequency == 0 || duration_ms == 0) {
        return 0;
    }

    /*
     * Maximum one DMA round.
     */
    uint32_t frames =
        (ac97_sample_rate * duration_ms) / 1000;

    if (frames == 0) {
        frames = 1;
    }

    uint32_t max_frames =
        AC97_BDL_ENTRIES *
        (AC97_DMA_PAGE_SIZE / 4);

    /*
     * Very long tones are split into several playbacks.
     */
    while (frames > 0) {

        uint32_t chunk = frames;

        if (chunk > max_frames) {
            chunk = max_frames;
        }

        /*
         * Generate the complete chunk.
         *
         * We use the first DMA page as temporary source,
         * then ac97_play_pcm_stereo() copies it into DMA
         * pages. For chunks > page size we instead generate
         * directly into the DMA pages.
         */

        uint32_t produced = 0;

        while (produced < chunk) {

            uint32_t page =
                produced /
                (AC97_DMA_PAGE_SIZE / 4);

            uint32_t in_page =
                produced %
                (AC97_DMA_PAGE_SIZE / 4);

            uint32_t available =
                (AC97_DMA_PAGE_SIZE / 4) - in_page;

            uint32_t left =
                chunk - produced;

            uint32_t count =
                (available < left)
                ? available
                : left;

            int16_t* dst =
                (int16_t*)(uintptr_t)
                ac97_dma_phys[page];

            for (uint32_t i = 0; i < count; i++) {

                int16_t sample =
                    ac97_wave(ac97_sine_phase);

                dst[(in_page + i) * 2 + 0] =
                    sample;

                dst[(in_page + i) * 2 + 1] =
                    sample;

                /*
                 * phase increment:
                 *
                 * frequency / sample_rate
                 */
                uint64_t step =
                    ((uint64_t)frequency << 32) /
                    ac97_sample_rate;

                ac97_sine_phase +=
                    (uint32_t)step;
            }

            produced += count;
        }

        /*
         * Manually prepare BDL for generated pages.
         */
        uint32_t entries =
            (chunk +
             (AC97_DMA_PAGE_SIZE / 4) - 1) /
            (AC97_DMA_PAGE_SIZE / 4);

        ac97_reset_pcm_out();
        ac97_clear_pcm_status();

        for (uint32_t i = 0; i < entries; i++) {

            uint32_t page_frames =
                AC97_DMA_PAGE_SIZE / 4;

            uint32_t remaining =
                chunk -
                i * page_frames;

            if (remaining < page_frames) {
                page_frames = remaining;
            }

            ac97_bdl[i].buffer =
                (uint32_t)ac97_dma_phys[i];

            ac97_bdl[i].samples =
                (uint16_t)(page_frames * 2);

            ac97_bdl[i].flags = 0;
        }

        ac97_bdl[entries - 1].flags =
            AC97_BDL_BUP |
            AC97_BDL_IOC;

        ac97_out32(
            (uint16_t)(ac97_nabm_base + AC97_PO_BDBAR),
            (uint32_t)ac97_bdl_phys
        );

        ac97_out8(
            (uint16_t)(ac97_nabm_base + AC97_PO_LVI),
            (uint8_t)(entries - 1)
        );

        ac97_out8(
            (uint16_t)(ac97_nabm_base + AC97_PO_CR),
            AC97_CR_RPBM
        );

        /*
         * Blocking playback.
         */
        for (;;) {

            uint16_t status =
                ac97_in16(
                    (uint16_t)
                    (ac97_nabm_base + AC97_PO_SR)
                );

            if (status & AC97_SR_DCH) {
                break;
            }

            __asm__ volatile ("pause");
        }

        ac97_out8(
            (uint16_t)(ac97_nabm_base + AC97_PO_CR),
            0
        );

        ac97_clear_pcm_status();

        frames -= chunk;
    }

    return 1;
}

/* ======================================================================== */
/* Stop                                                                      */
/* ======================================================================== */

void ac97_stop(void)
{
    if (!ac97_available) {
        return;
    }

    ac97_out8(
        (uint16_t)(ac97_nabm_base + AC97_PO_CR),
        0
    );

    ac97_clear_pcm_status();
}

/* ======================================================================== */
/* Initialization                                                             */
/* ======================================================================== */

int ac97_init(void)
{
    printf("[AC97] Initializing AC'97 audio...\n");

    if (!ac97_find_controller()) {
        return 0;
    }

    if (!ac97_controller_reset()) {
        return 0;
    }

    /*
     * Codec access is now available.
     */
    ac97_available = 1;

    if (!ac97_codec_init()) {
        ac97_available = 0;
        return 0;
    }

    if (!ac97_alloc_dma()) {
        ac97_available = 0;
        return 0;
    }

    /*
     * Default configuration.
     */
    ac97_set_sample_rate(48000);
    ac97_set_volume(80);

    printf("[AC97] Sample rate: %u Hz\n", ac97_sample_rate);
    printf("[AC97] Volume: %u%%\n", ac97_volume);
    printf("[AC97] PCM OUT DMA: READY\n");
    printf("[AC97] AC'97 audio READY\n");

    return 1;
}

int ac97_is_available(void)
{
    return ac97_available;
}

const ac97_codec_info_t* ac97_get_codec_info(void)
{
    return &ac97_codec;
}