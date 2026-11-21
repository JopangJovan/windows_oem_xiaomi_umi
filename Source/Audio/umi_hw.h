/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <ntddk.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char      u8, uint8_t;
typedef unsigned short     u16, uint16_t;
typedef unsigned int       u32, uint32_t;
typedef unsigned long long u64, uint64_t;
typedef signed short       s16;
typedef signed int         s32;
typedef signed long long   s64;

#define UMI_BIT(n) (1u << (n))
#define UMI_TAG    'AImU'

/* Logging to \SystemRoot\Temp\umi-audio.log + DbgPrintEx */
void umi_log_init(void);
void umi_log_close(void);
void umi_log(const char *fmt, ...);

/* Timing */
static __inline void umi_spin_us(u32 us)
{
    KeStallExecutionProcessor(us);
}

static __inline void umi_sleep_ms(u32 ms)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

static __inline u64 umi_now_us(void)
{
    return (u64)KeQueryInterruptTime() / 10ULL;
}

static __inline void umi_dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static __inline u32 umi_rd32(volatile void *base, u32 off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)((const volatile u8 *)base + off));
}

static __inline void umi_wr32(volatile void *base, u32 off, u32 val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)((volatile u8 *)base + off), val);
}

static __inline void *umi_map_io(u64 phys, SIZE_T size, BOOLEAN write_combine)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)phys;
    return MmMapIoSpace(pa, size, write_combine ? MmWriteCombined : MmNonCached);
}

static __inline void umi_unmap_io(void *va, SIZE_T size)
{
    if (va) MmUnmapIoSpace(va, size);
}

/* File loader from \SystemRoot\System32\drivers\umi-audio\<name> */
u8 *umi_load_fw(const char *name, u32 *out_size);
void umi_free_fw(u8 *buf);

/* APR answer & event callback */
typedef struct {
    u32 opcode;
    u32 token;
    u32 length;
    u32 words[64];
} UMI_APR_ANSWER;

typedef BOOLEAN (*UMI_APR_EVENT_FN)(u8 service, u16 port, u32 opcode, u32 token,
                                    const u32 *payload, u32 length);

/* ADSP / GLINK / APR */
BOOLEAN umi_adsp_start(void);
void    umi_adsp_stop(void);
void    umi_adsp_set_events(UMI_APR_EVENT_FN fn);
void    umi_adsp_pump(void);
BOOLEAN umi_adsp_apr(u8 service, u16 port, u32 token, u32 opcode,
                     const void *payload, u32 length, u32 timeout_ms,
                     UMI_APR_ANSWER *answer);

/* CS35L41 Stereo Speakers */
BOOLEAN umi_speaker_prepare(void);
BOOLEAN umi_speaker_start(void);
BOOLEAN umi_speaker_volume(u32 attenuation_eighths_db, u32 step_ms);
BOOLEAN umi_speaker_healthy(void);
void    umi_speaker_stop(void);
void    umi_speaker_shutdown(void);
void    umi_speaker_quiesce(void);

/* Sound (SMMU 0x1801 + MI2S2 + AFE 0x1004 + ADM + ASM + WaveRT streaming) */
BOOLEAN umi_sound_open(void);
void    umi_sound_close(void);
BOOLEAN umi_sound_start_playback(u8 *cyclic_buf, u32 cyclic_size);
void    umi_sound_stop_playback(void);
u64     umi_sound_get_position(void);
void    umi_sound_set_volume(s32 db_16_16, BOOLEAN muted);

#ifdef __cplusplus
}
#endif
