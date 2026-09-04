#pragma once

#include "lib/types.h"

/*
 * Intel AC'97 / ICH controller
 *
 * NAM  - Native Audio Mixer / Codec registers
 * NABM - Native Audio Bus Master registers
 */

/* ======================================================================== */
/* Codec / NAM registers                                                    */
/* ======================================================================== */

#define AC97_REG_RESET              0x00
#define AC97_REG_MASTER_VOLUME      0x02
#define AC97_REG_HEADPHONE_VOLUME   0x04
#define AC97_REG_MASTER_TONE        0x08
#define AC97_REG_PCM_OUT_VOLUME     0x18
#define AC97_REG_RECORD_SELECT      0x1A
#define AC97_REG_RECORD_GAIN        0x1C
#define AC97_REG_POWERDOWN          0x26
#define AC97_REG_EXT_AUDIO_ID       0x28
#define AC97_REG_EXT_AUDIO_CTRL     0x2A
#define AC97_REG_PCM_FRONT_RATE     0x2C
#define AC97_REG_PCM_SURR_RATE      0x2E
#define AC97_REG_PCM_LFE_RATE       0x30
#define AC97_REG_PCM_ADC_RATE       0x32
#define AC97_REG_MIC_ADC_RATE       0x34

#define AC97_REG_VENDOR_ID1         0x7C
#define AC97_REG_VENDOR_ID2         0x7E

/* Extended audio control */
#define AC97_EACS_VRA               (1 << 0)

/* Powerdown */
#define AC97_POWER_ADC              (1 << 8)
#define AC97_POWER_DAC              (1 << 9)

/* Volume */
#define AC97_VOLUME_MUTE            (1 << 15)

/* ======================================================================== */
/* NABM registers                                                           */
/* ======================================================================== */

#define AC97_BM_PCM_IN             0x00
#define AC97_BM_PCM_OUT            0x10
#define AC97_BM_MIC_IN             0x20

#define AC97_BM_GLOB_CNT           0x2C
#define AC97_BM_GLOB_STA           0x30
#define AC97_BM_CODEC_SEMA         0x34

/* PCM OUT registers */
#define AC97_PO_BDBAR              0x10
#define AC97_PO_CIV                0x14
#define AC97_PO_LVI                0x15
#define AC97_PO_SR                 0x16
#define AC97_PO_PICB               0x18
#define AC97_PO_PIV                0x1A
#define AC97_PO_CR                 0x1B

/* Bus master status bits */
#define AC97_SR_DCH                0x0001
#define AC97_SR_CELV               0x0002
#define AC97_SR_LVBCI              0x0004
#define AC97_SR_BCIS               0x0008
#define AC97_SR_FIFOE              0x0010

#define AC97_SR_W1C_MASK           0x001C

/* Control */
#define AC97_CR_RPBM               0x01
#define AC97_CR_RR                 0x02
#define AC97_CR_LVBIE              0x04
#define AC97_CR_FEIE               0x08
#define AC97_CR_IOCE               0x10

/* Global control */
#define AC97_GLOB_GIE              (1 << 0)
#define AC97_GLOB_COLD_RESET       (1 << 1)
#define AC97_GLOB_WARM_RESET       (1 << 2)

/* Global status */
#define AC97_GLOB_PCR              (1 << 8)

/* ======================================================================== */
/* BDL                                                                       */
/* ======================================================================== */

#define AC97_BDL_ENTRIES           32
#define AC97_BDL_ENTRY_SIZE        8

#define AC97_BDL_IOC               (1 << 15)
#define AC97_BDL_BUP               (1 << 14)

/*
 * One DMA page = 4096 bytes
 *
 * Stereo 16-bit:
 * 4 bytes per frame
 * 4096 / 4 = 1024 stereo frames
 */
#define AC97_DMA_PAGE_SIZE         4096

typedef struct {
    uint32_t buffer;
    uint16_t samples;
    uint16_t flags;
} ac97_bdl_entry_t;

typedef struct {
    uint32_t frequency;
    uint32_t duration_ms;
} ac97_tone_t;

typedef struct {
    uint16_t vendor_id1;
    uint16_t vendor_id2;
    uint16_t ext_audio_id;
    uint16_t ext_audio_ctrl;
} ac97_codec_info_t;

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int ac97_init(void);

int ac97_is_available(void);

void ac97_set_volume(uint8_t volume);
uint8_t ac97_get_volume(void);

void ac97_set_sample_rate(uint32_t rate);
uint32_t ac97_get_sample_rate(void);

int ac97_play_pcm_stereo(
    const int16_t* samples,
    uint32_t frames
);

int ac97_play_tone(
    uint32_t frequency,
    uint32_t duration_ms
);

void ac97_stop(void);

uint16_t ac97_read_codec(uint8_t reg);
void ac97_write_codec(uint8_t reg, uint16_t value);

const ac97_codec_info_t* ac97_get_codec_info(void);