/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Xiaomi Mi 10 (UMI) ADSP Real-Time Audio Engine:
 * SMMU 0x1801 + TLMM WEST MI2S2 + AFE 0x1004 + ADM + ASM + WaveRT Cyclic Buffer Feeder
 */
#include "umi_hw.h"

#define TLMM_WEST_PHYS      0x0F100000ULL
#define TLMM_SIZE           0x00100000u
#define SMMU_PHYS           0x15000000ULL
#define SMMU_SIZE           0x000D0000u
#define AUDIO_STREAM_SID    0x1801u
#define AUDIO_STREAM_BITS   (1ULL << 32)

#define GPIO_CFG(pin)       ((pin) * 0x1000u)
#define GPIO_IO(pin)        ((pin) * 0x1000u + 4u)
#define CFG_8MA             (3u << 6)
#define CFG_OUTPUT          (1u << 9)

#define APR_SVC_AFE         4u
#define APR_SVC_ASM         7u
#define APR_SVC_ADM         8u
#define APR_BASIC_RSP       0x000110E8u

#define AFE_PORT_TERT_MI2S  0x1004u
#define AFE_CMD_START       0x000100E5u
#define AFE_CMD_STOP        0x000100E6u
#define AFE_CMD_SET_PARAM2  0x000100EFu
#define AFE_SVC_SET_PARAM   0x000100F3u
#define AFE_MOD_I2S         0x0001020Cu
#define AFE_PARAM_I2S       0x0001020Du
#define AFE_MOD_CLOCK       0x0001028Fu
#define AFE_PARAM_CLOCK     0x00010290u

#define ADM_CMD_OPEN_V5     0x00010326u
#define ADM_RSP_OPEN_V5     0x00010329u
#define ADM_CMD_CLOSE_V5    0x00010327u
#define ADM_CMD_ROUTE_V5    0x00010325u

#define ASM_CMD_MAP         0x00010D92u
#define ASM_RSP_MAP         0x00010D93u
#define ASM_CMD_UNMAP       0x00010D94u
#define ASM_CMD_OPEN_WR_V3  0x00010DB3u
#define ASM_CMD_FMT_V2      0x00010D98u
#define ASM_CMD_RUN_V2      0x00010DAAu
#define ASM_CMD_WRITE_V2    0x00010DABu
#define ASM_EVT_WR_DONE_V2  0x00010D99u
#define ASM_CMD_CLOSE       0x00010BCDu

#define RATE                48000u
#define PERIODS             6u
#define PERIOD_FRAMES       480u /* 10 ms at 48 kHz */
#define PERIOD_BYTES        (PERIOD_FRAMES * 4u) /* 1920 bytes */
#define BUFFER_BYTES        (PERIODS * 4096u)    /* 24 KiB */
#define SESSION             1u
#define STREAM_PORT         ((SESSION << 8) | 1u)
#define WRITE_TOKEN         0x80000000u

static volatile u8 *g_tlmm_west;
static u32 g_pin_cfg[3], g_pin_io[3];
static BOOLEAN g_pins_on;

static u8 *g_dma_va;
static u64 g_dma_dsp_addr;
static u32 g_map_handle, g_copp;
static BOOLEAN g_hw_open, g_afe_on, g_mapped, g_copp_open, g_stream_open;
static volatile LONG g_returned_mask, g_written_done;

/* Playback thread state */
static KMUTEX g_play_lock;
static BOOLEAN g_play_lock_init;
static PETHREAD g_play_thread;
static volatile LONG g_play_stop;
static u8 *g_cyclic_buf;
static u32 g_cyclic_size;
static volatile LONG64 g_play_pos;
static volatile LONG g_vol_atten = 128; /* -16 dB default */
static volatile LONG g_vol_dirty = 0;

static BOOLEAN smmu_setup_audio_bypass(void)
{
    volatile u8 *smmu = (volatile u8 *)umi_map_io(SMMU_PHYS, SMMU_SIZE, FALSE);
    if (!smmu) return FALSE;
    u32 idr0 = umi_rd32(smmu, 0x020), idr1 = umi_rd32(smmu, 0x024);
    u32 matches = idr0 & 0xffu, num_cb = idr1 & 0xffu;
    u32 page_sz = (idr1 >> 31) ? 0x10000u : 0x1000u;
    u32 num_pages = 2u << ((idr1 >> 28) & 7u);
    BOOLEAN used_cb[128] = { FALSE };
    u32 free_idx = 0xFFFFFFFFu;
    BOOLEAN covered = FALSE;
    for (u32 n = 0; n < matches && n < 128u; n++) {
        u32 smr = umi_rd32(smmu, 0x800 + 4 * n), s2cr = umi_rd32(smmu, 0xc00 + 4 * n);
        if (smr & UMI_BIT(31)) {
            if ((s2cr & 0xffu) < 128u) used_cb[s2cr & 0xffu] = TRUE;
            u32 id = smr & 0x7fffu, mask = (smr >> 16) & 0x7fffu;
            if (((id ^ AUDIO_STREAM_SID) & ~mask) == 0) covered = TRUE;
        } else if (free_idx == 0xFFFFFFFFu) {
            free_idx = n;
        }
    }
    BOOLEAN ok = covered;
    if (!covered && free_idx != 0xFFFFFFFFu) {
        u32 free_cb = 0xFFFFFFFFu;
        for (u32 cb = 0; cb < num_cb && cb < 128u; cb++) {
            u32 cb_off = (num_pages + cb) * page_sz;
            if (cb_off + 0x1000u > SMMU_SIZE) break;
            if (!used_cb[cb] && (umi_rd32(smmu, cb_off) & 1u) == 0) { free_cb = cb; break; }
        }
        if (free_cb != 0xFFFFFFFFu) {
            u32 cb_off = (num_pages + free_cb) * page_sz;
            umi_wr32(smmu, cb_off, 0u);
            umi_wr32(smmu, page_sz + 0x800u + 4u * free_cb, 1u);
            umi_wr32(smmu, page_sz + 4u * free_cb, 0x00210000u);
            umi_wr32(smmu, 0xc00u + 4u * free_idx, free_cb);
            umi_wr32(smmu, 0x800u + 4u * free_idx, 0x80000000u | AUDIO_STREAM_SID);
            u32 rb_smr = umi_rd32(smmu, 0x800u + 4u * free_idx);
            u32 rb_s2  = umi_rd32(smmu, 0xc00u + 4u * free_idx);
            umi_log("sound: SMMU SID 0x1801 idx=%u cb=%u SMR=0x%x S2CR=0x%x\n", free_idx, free_cb, rb_smr, rb_s2);
            ok = (rb_smr == (0x80000000u | AUDIO_STREAM_SID)) && ((rb_s2 & 0x300ffu) == free_cb);
        }
    }
    umi_unmap_io((void *)smmu, SMMU_SIZE);
    return ok;
}

static BOOLEAN apr_cmd(const char *what, u8 svc, u16 port, u32 op, const void *pay, u32 len, u32 wanted, UMI_APR_ANSWER *out)
{
    UMI_APR_ANSWER local;
    if (!out) out = &local;
    if (!umi_adsp_apr(svc, port, 0, op, pay, len, 1000, out)) {
        umi_log("sound: %s (op 0x%x) TIMEOUT\n", what, op);
        return FALSE;
    }
    if (out->opcode == APR_BASIC_RSP && out->words[0] == op) {
        if (out->words[1] != 0) umi_log("sound: %s REFUSED err=%u\n", what, out->words[1]);
        return out->words[1] == 0 && wanted == 0;
    }
    return wanted != 0 && out->opcode == wanted;
}

static BOOLEAN asm_events(u8 svc, u16 port, u32 op, u32 tok, const u32 *pay, u32 len)
{
    UNREFERENCED_PARAMETER(port);
    UNREFERENCED_PARAMETER(pay);
    UNREFERENCED_PARAMETER(len);
    if (svc != APR_SVC_ASM) return FALSE;
    if ((op == ASM_EVT_WR_DONE_V2 || op == APR_BASIC_RSP) && (tok & WRITE_TOKEN)) {
        InterlockedIncrement(&g_written_done);
        InterlockedOr(&g_returned_mask, (LONG)(1u << (tok & 31u)));
        return TRUE;
    }
    return FALSE;
}

static BOOLEAN pins_enable(void)
{
    static const struct { u32 pin, cfg, io; } pins[3] = {
        { 133, (1u << 2) | CFG_8MA | CFG_OUTPUT, 2 },
        { 135, (1u << 2) | CFG_8MA | CFG_OUTPUT, 2 },
        { 137, (3u << 2) | CFG_8MA, 0 },
    };
    if (!g_tlmm_west) {
        g_tlmm_west = (volatile u8 *)umi_map_io(TLMM_WEST_PHYS, TLMM_SIZE, FALSE);
        if (!g_tlmm_west) return FALSE;
    }
    for (u32 i = 0; i < 3; i++) {
        g_pin_cfg[i] = umi_rd32(g_tlmm_west, GPIO_CFG(pins[i].pin));
        g_pin_io[i]  = umi_rd32(g_tlmm_west, GPIO_IO(pins[i].pin));
        umi_wr32(g_tlmm_west, GPIO_IO(pins[i].pin), pins[i].io);
        umi_wr32(g_tlmm_west, GPIO_CFG(pins[i].pin), pins[i].cfg);
    }
    g_pins_on = TRUE;
    return TRUE;
}

static void pins_disable(void)
{
    static const u32 pins[3] = { 133, 135, 137 };
    if (g_pins_on && g_tlmm_west) {
        for (u32 i = 0; i < 3; i++) {
            umi_wr32(g_tlmm_west, GPIO_CFG(pins[i]), g_pin_cfg[i]);
            umi_wr32(g_tlmm_west, GPIO_IO(pins[i]), g_pin_io[i] & 2u);
        }
        g_pins_on = FALSE;
    }
    if (g_tlmm_west) { umi_unmap_io((void *)g_tlmm_west, TLMM_SIZE); g_tlmm_west = NULL; }
}

static BOOLEAN afe_start(void)
{
    #pragma pack(push, 1)
    struct { u32 mod, param; u16 sz, rsv; } hdr;
    struct { u32 ver; u16 bits, ch_mode, stereo, ws_src; u32 rate; u16 fmt, rsv; } i2s = { 1, 16, 2, 1, 1, RATE, 0, 0 };
    struct { u32 ver, id, hz; u16 attr, root; u32 en; } clk = { 1, 0x104, RATE * 16 * 2, 1, 0, 1 };
    #pragma pack(pop)
    u8 pkt[64];

    RtlZeroMemory(pkt, sizeof(pkt));
    *(u16 *)pkt = AFE_PORT_TERT_MI2S;
    *(u16 *)(pkt + 2) = (u16)(sizeof(hdr) + sizeof(i2s));
    hdr.mod = AFE_MOD_I2S; hdr.param = AFE_PARAM_I2S; hdr.sz = sizeof(i2s); hdr.rsv = 0;
    RtlCopyMemory(pkt + 16, &hdr, sizeof(hdr));
    RtlCopyMemory(pkt + 16 + sizeof(hdr), &i2s, sizeof(i2s));
    if (!apr_cmd("AFE I2S cfg", APR_SVC_AFE, 0, AFE_CMD_SET_PARAM2, pkt, 16 + sizeof(hdr) + sizeof(i2s), 0, NULL))
        return FALSE;

    RtlZeroMemory(pkt, sizeof(pkt));
    *(u32 *)pkt = (u32)(sizeof(hdr) + sizeof(clk));
    hdr.mod = AFE_MOD_CLOCK; hdr.param = AFE_PARAM_CLOCK; hdr.sz = sizeof(clk); hdr.rsv = 0;
    RtlCopyMemory(pkt + 16, &hdr, sizeof(hdr));
    RtlCopyMemory(pkt + 16 + sizeof(hdr), &clk, sizeof(clk));
    if (!apr_cmd("AFE CLK on", APR_SVC_AFE, 0, AFE_SVC_SET_PARAM, pkt, 16 + sizeof(hdr) + sizeof(clk), 0, NULL))
        return FALSE;

    u16 st[2] = { AFE_PORT_TERT_MI2S, 0 };
    if (!apr_cmd("AFE START", APR_SVC_AFE, 0, AFE_CMD_START, st, sizeof(st), 0, NULL))
        return FALSE;
    g_afe_on = TRUE;
    return TRUE;
}

static BOOLEAN stream_open_hw(void)
{
    PHYSICAL_ADDRESS low = { 0 }, high, bound = { 0 };
    high.QuadPart = 0xFFFFFFFFLL;
    g_dma_va = (u8 *)MmAllocateContiguousMemorySpecifyCache(BUFFER_BYTES, low, high, bound, MmNonCached);
    if (!g_dma_va) return FALSE;
    RtlZeroMemory(g_dma_va, BUFFER_BYTES);
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(g_dma_va);
    g_dma_dsp_addr = (u64)pa.QuadPart | AUDIO_STREAM_BITS;

    #pragma pack(push, 1)
    struct { u16 pool, regions; u32 prop; u32 lo, hi, sz; } map = {
        3, 1, 0, (u32)g_dma_dsp_addr, (u32)(g_dma_dsp_addr >> 32), BUFFER_BYTES
    };
    struct { u16 flags, mode, ep1, ep2; u32 topo; u16 ch, bits; u32 rate; u8 chmap[8]; } open = {
        0, 1, AFE_PORT_TERT_MI2S, 0xffff, 0x00010312u, 2, 24, RATE, { 1, 2, 0, 0, 0, 0, 0, 0 }
    };
    struct { u32 mat, sess; u16 sid, copps; u16 copp[2]; } route = { 0, 1, SESSION, 1, { 0, 0 } };
    struct { u32 flags; u16 sink, bits; u32 topo, fmt; } wr = { 0, 0, 16, 0x00010c68u, 0x00010da5u };
    struct { u32 sz; u16 ch, bits; u32 rate; u16 is_sig, rsv; u8 chmap[8]; } fmt = {
        20, 2, 16, RATE, 1, 0, { 1, 2, 0, 0, 0, 0, 0, 0 }
    };
    #pragma pack(pop)
    u32 run[3] = { 0, 0, 0 };
    UMI_APR_ANSWER ans;

    if (!apr_cmd("ASM MAP", APR_SVC_ASM, 0, ASM_CMD_MAP, &map, sizeof(map), ASM_RSP_MAP, &ans)) return FALSE;
    g_mapped = TRUE; g_map_handle = ans.words[0];

    if (!apr_cmd("ADM OPEN", APR_SVC_ADM, AFE_PORT_TERT_MI2S, ADM_CMD_OPEN_V5, &open, sizeof(open), ADM_RSP_OPEN_V5, &ans) ||
        ans.words[0] != 0) return FALSE;
    g_copp_open = TRUE; g_copp = ans.words[1] & 0xffffu;
    route.copp[0] = (u16)g_copp;

    if (!apr_cmd("ASM OPEN", APR_SVC_ASM, STREAM_PORT, ASM_CMD_OPEN_WR_V3, &wr, sizeof(wr), 0, NULL)) return FALSE;
    g_stream_open = TRUE;

    return apr_cmd("ADM ROUTE", APR_SVC_ADM, 0, ADM_CMD_ROUTE_V5, &route, sizeof(route), 0, NULL) &&
           apr_cmd("ASM FMT", APR_SVC_ASM, STREAM_PORT, ASM_CMD_FMT_V2, &fmt, sizeof(fmt), 0, NULL) &&
           apr_cmd("ASM RUN", APR_SVC_ASM, STREAM_PORT, ASM_CMD_RUN_V2, run, sizeof(run), 0, NULL);
}

BOOLEAN umi_sound_open(void)
{
    if (!g_play_lock_init) {
        KeInitializeMutex(&g_play_lock, 0);
        g_play_lock_init = TRUE;
    }
    if (g_hw_open) return TRUE;
    umi_log("sound: opening ADSP + MI2S2 + CS35L41\n");
    if (!umi_adsp_start()) return FALSE;
    umi_adsp_set_events(asm_events);
    if (!smmu_setup_audio_bypass() || !pins_enable() || !afe_start() || !stream_open_hw()) {
        umi_sound_close();
        return FALSE;
    }
    if (!umi_speaker_prepare()) {
        umi_sound_close();
        return FALSE;
    }
    g_hw_open = TRUE;
    umi_log("sound: hardware ready for WaveRT playback\n");
    return TRUE;
}

static void fill_period_from_cyclic(u32 p, u32 *read_off)
{
    u8 *dst = g_dma_va + 4096 * p;
    if (!g_cyclic_buf || !g_cyclic_size) {
        RtlZeroMemory(dst, PERIOD_BYTES);
    } else {
        u32 off = *read_off;
        u32 first = g_cyclic_size - off;
        if (first >= PERIOD_BYTES) {
            RtlCopyMemory(dst, g_cyclic_buf + off, PERIOD_BYTES);
            off += PERIOD_BYTES;
            if (off >= g_cyclic_size) off = 0;
        } else {
            RtlCopyMemory(dst, g_cyclic_buf + off, first);
            RtlCopyMemory(dst + first, g_cyclic_buf, PERIOD_BYTES - first);
            off = PERIOD_BYTES - first;
        }
        *read_off = off;
        InterlockedExchangeAdd64(&g_play_pos, (LONG64)PERIOD_BYTES);
    }
    umi_dsb();
    u32 wr[8] = { (u32)(g_dma_dsp_addr + 4096 * p), (u32)((g_dma_dsp_addr + 4096 * p) >> 32),
                  g_map_handle, PERIOD_BYTES, p, 0, 0, 0 };
    umi_adsp_apr(APR_SVC_ASM, STREAM_PORT, WRITE_TOKEN | p, ASM_CMD_WRITE_V2, wr, sizeof(wr), 0, NULL);
}

static void PlaybackThread(PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    if (!umi_speaker_start()) {
        umi_log("sound: umi_speaker_start failed\n");
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
        return;
    }
    umi_speaker_volume((u32)InterlockedCompareExchange(&g_vol_atten, 0, 0), 2);
    InterlockedExchange(&g_vol_dirty, 0);

    InterlockedExchange(&g_returned_mask, 0);
    InterlockedExchange(&g_written_done, 0);
    u32 read_off = 0;
    u32 in_flight = 0;

    for (u32 p = 0; p < PERIODS; p++) {
        fill_period_from_cyclic(p, &read_off);
        in_flight++;
    }

    u32 health_ticks = 0;
    while (!InterlockedCompareExchange(&g_play_stop, 0, 0)) {
        umi_adsp_pump();
        LONG back = InterlockedExchange(&g_returned_mask, 0);
        if (back) {
            for (u32 p = 0; p < PERIODS; p++) {
                if (!(back & (1 << p))) continue;
                in_flight--;
                if (!InterlockedCompareExchange(&g_play_stop, 0, 0)) {
                    fill_period_from_cyclic(p, &read_off);
                    in_flight++;
                }
            }
        }
        if (InterlockedExchange(&g_vol_dirty, 0)) {
            umi_speaker_volume((u32)InterlockedCompareExchange(&g_vol_atten, 0, 0), 1);
        }
        if (++health_ticks >= 500) {
            health_ticks = 0;
            if (!umi_speaker_healthy()) break;
        }
        umi_sleep_ms(2);
    }

    /* Drain remaining in-flight periods briefly (up to 100 ms) */
    for (u32 w = 0; w < 50 && in_flight > 0; w++) {
        umi_adsp_pump();
        LONG back = InterlockedExchange(&g_returned_mask, 0);
        for (u32 p = 0; p < PERIODS; p++) if (back & (1 << p)) in_flight--;
        umi_sleep_ms(2);
    }

    umi_speaker_stop();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

BOOLEAN umi_sound_start_playback(u8 *cyclic_buf, u32 cyclic_size)
{
    if (!g_hw_open) return FALSE;
    KeWaitForSingleObject(&g_play_lock, Executive, KernelMode, FALSE, NULL);
    if (g_play_thread) {
        KeReleaseMutex(&g_play_lock, FALSE);
        return TRUE;
    }
    g_cyclic_buf = cyclic_buf;
    g_cyclic_size = cyclic_size;
    InterlockedExchange64(&g_play_pos, 0);
    InterlockedExchange(&g_play_stop, 0);
    HANDLE th = NULL;
    NTSTATUS st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, PlaybackThread, NULL);
    if (NT_SUCCESS(st)) {
        ObReferenceObjectByHandle(th, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&g_play_thread, NULL);
        ZwClose(th);
    }
    KeReleaseMutex(&g_play_lock, FALSE);
    return NT_SUCCESS(st);
}

void umi_sound_stop_playback(void)
{
    if (!g_play_lock_init) return;
    KeWaitForSingleObject(&g_play_lock, Executive, KernelMode, FALSE, NULL);
    if (g_play_thread) {
        InterlockedExchange(&g_play_stop, 1);
        KeWaitForSingleObject(g_play_thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_play_thread);
        g_play_thread = NULL;
    }
    g_cyclic_buf = NULL;
    g_cyclic_size = 0;
    KeReleaseMutex(&g_play_lock, FALSE);
}

u64 umi_sound_get_position(void)
{
    u64 pos = (u64)InterlockedCompareExchange64(&g_play_pos, 0, 0);
    u32 sz = g_cyclic_size;
    return sz ? (pos % sz) : 0;
}

void umi_sound_set_volume(s32 db_16_16, BOOLEAN muted)
{
    /* Windows volume is in 1/65536 dB (-96*65536 .. 0). CS35L41 attenuation is in 1/8 dB (0 .. 816). */
    u32 atten;
    if (muted || db_16_16 <= -96 * 65536) {
        atten = 816; /* -102 dB mute */
    } else if (db_16_16 >= 0) {
        atten = 24;  /* safe -3 dB ceiling below unity */
    } else {
        u32 neg_db_eighths = (u32)((-(s64)db_16_16) >> 13); /* divide by 8192 = 65536/8 */
        atten = neg_db_eighths + 24;
        if (atten > 816) atten = 816;
    }
    InterlockedExchange(&g_vol_atten, (LONG)atten);
    InterlockedExchange(&g_vol_dirty, 1);
}

void umi_sound_close(void)
{
    umi_sound_stop_playback();
    umi_speaker_shutdown();
    if (g_stream_open) { apr_cmd("ASM CLOSE", APR_SVC_ASM, STREAM_PORT, ASM_CMD_CLOSE, NULL, 0, 0, NULL); g_stream_open = FALSE; }
    if (g_copp_open)   { apr_cmd("ADM CLOSE", APR_SVC_ADM, (u16)g_copp, ADM_CMD_CLOSE_V5, NULL, 0, 0, NULL); g_copp_open = FALSE; }
    if (g_mapped)      { u32 h = g_map_handle; apr_cmd("ASM UNMAP", APR_SVC_ASM, 0, ASM_CMD_UNMAP, &h, sizeof(h), 0, NULL); g_mapped = FALSE; }
    if (g_afe_on)      { u16 st[2] = { AFE_PORT_TERT_MI2S, 0 }; apr_cmd("AFE STOP", APR_SVC_AFE, 0, AFE_CMD_STOP, st, sizeof(st), 0, NULL); g_afe_on = FALSE; }
    pins_disable();
    if (g_dma_va) { MmFreeContiguousMemorySpecifyCache(g_dma_va, BUFFER_BYTES, MmNonCached); g_dma_va = NULL; }
    umi_adsp_stop();
    g_hw_open = FALSE;
}
