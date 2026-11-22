/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <ntddk.h>
#include <stdarg.h>

typedef unsigned char      u8, uint8_t;
typedef unsigned short     u16, uint16_t;
typedef unsigned int       u32, uint32_t;
typedef unsigned long long u64, uint64_t;
typedef signed short       s16;
typedef signed int         s32;
typedef signed long long   s64;

#define UMI_BIT(n) (1u << (n))
#define UMI_TAG    'SImU'

void umi_log_init(void);
void umi_log_close(void);
void umi_log(const char *fmt, ...);

static __inline void umi_spin_us(u32 us) { KeStallExecutionProcessor(us); }
static __inline void umi_sleep_ms(u32 ms) {
    LARGE_INTEGER iv; iv.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &iv);
}
static __inline u64 umi_now_us(void) { return (u64)KeQueryInterruptTime() / 10ULL; }
static __inline void umi_dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static __inline u32 umi_rd32(volatile void *base, u32 off) {
    return READ_REGISTER_ULONG((volatile ULONG *)((const volatile u8 *)base + off));
}
static __inline void umi_wr32(volatile void *base, u32 off, u32 val) {
    WRITE_REGISTER_ULONG((volatile ULONG *)((volatile u8 *)base + off), val);
}
static __inline void *umi_map_io(u64 phys, SIZE_T size, BOOLEAN wc) {
    PHYSICAL_ADDRESS pa; pa.QuadPart = (LONGLONG)phys;
    return MmMapIoSpace(pa, size, wc ? MmWriteCombined : MmNonCached);
}
static __inline void umi_unmap_io(void *va, SIZE_T size) {
    if (va) MmUnmapIoSpace(va, size);
}

u8  *umi_load_fw(const char *name, u32 *out_size);
void umi_free_fw(u8 *buf);

typedef struct { u32 opcode, token, length; u32 words[64]; } UMI_APR_ANSWER;
typedef BOOLEAN (*UMI_APR_EVENT_FN)(u8 service, u16 port, u32 opcode, u32 token, const u32 *payload, u32 length);

typedef void (*UMI_QRTR_RX_FN)(u32 type, u32 snode, u32 sport, u32 dport, const u8 *payload, u32 len);
typedef void (*UMI_FRPC_RX_FN)(const u8 *payload, u32 len);

BOOLEAN umi_slpi_start(void);
void    umi_slpi_stop(void);
void    umi_slpi_pump(void);
void    umi_slpi_set_qrtr_rx(UMI_QRTR_RX_FN fn);
void    umi_slpi_set_fastrpc_rx(UMI_FRPC_RX_FN fn);
BOOLEAN umi_qrtr_send(u32 type, u32 src_port, u32 dst_node, u32 dst_port, const void *payload, u32 len);
BOOLEAN umi_fastrpc_ready(void);
BOOLEAN umi_fastrpc_send(const void *data, u32 len);

void    umi_sns_init(void);
void    umi_sns_publish_tftp(void);
void    umi_sns_on_qrtr(u32 type, u32 snode, u32 sport, u32 dport, const u8 *payload, u32 len);
void    umi_sns_tick(void);

void    umi_fastrpc_on_rx(const u8 *data, u32 len);
void    umi_fastrpc_try_start(void);
void    umi_fastrpc_stop(void);

NTSTATUS umi_hid_init(PVOID WdfDevicePtr);
void     umi_hid_stop(void);
void     umi_hid_submit_sensor(u32 kind, s32 v0, s32 v1, s32 v2);
