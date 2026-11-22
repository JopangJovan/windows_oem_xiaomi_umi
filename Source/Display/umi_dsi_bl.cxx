/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Xiaomi Mi 10 (umi) SM8250 MDSS DSI0 AMOLED Brightness for KMDOD (umi-display.sys)
 *
 * Implements DXGK_BRIGHTNESS_INTERFACE (GUID_DEVINTERFACE_BRIGHTNESS v1)
 * using MIPI DCS 0x51 over DSI0 (0x0AE94000, DSI 6G v2.4 +0x4 shift, FROM_FRAME_BUFFER=1).
 */

#include "bdd.hxx"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define UMI_BIT(n)              (1u << (n))
#define DSI0_PHYS_BASE          0x0AE94000ULL
#define DSI0_SIZE               0x00000400UL

#define REG_DSI_6G_HW_VERSION   0x000u
#define REG_DSI_CTRL            0x004u
#define REG_DSI_STATUS0         0x008u
#define REG_DSI_CMD_DMA_CTRL    0x03Cu
#define REG_DSI_DMA_BASE        0x048u
#define REG_DSI_DMA_LEN         0x04Cu
#define REG_DSI_TRIG_DMA        0x090u
#define REG_DSI_INTR_CTRL       0x110u

#define DSI_STATUS0_DMA_BUSY    UMI_BIT(1)
#define DSI_STATUS0_MDP_BUSY    UMI_BIT(2)
#define DSI_CMD_DMA_FROM_FB     UMI_BIT(28)
#define DSI_CMD_DMA_LOW_POWER   UMI_BIT(26)
#define DSI_INTR_CMD_DMA_DONE   UMI_BIT(0)

#define UMI_BL_MAX_LEVEL        2047u

/* {FDE5BBA4-B3F9-46FB-BDAA-0728CE3100B4} */
static const GUID UMI_GUID_DEVINTERFACE_BRIGHTNESS = {
    0xFDE5BBA4, 0xB3F9, 0x46FB,
    { 0xBD, 0xAA, 0x07, 0x28, 0xCE, 0x31, 0x00, 0xB4 }
};

static volatile u8 *g_Dsi0 = NULL;
static u8          *g_DmaBuf = NULL;
static u32          g_DmaPhys = 0;
static UCHAR        g_CurrentPct = 70;
static KMUTEX       g_BlLock;
static BOOLEAN      g_BlInited = FALSE;

static __inline u32 dsi_rd32(u32 off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(g_Dsi0 + off));
}

static __inline void dsi_wr32(u32 off, u32 val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(g_Dsi0 + off), val);
}

static BOOLEAN UmiDsiSendDcs51(u32 level)
{
    if (!g_Dsi0 || !g_DmaBuf) return FALSE;
    if (level > UMI_BL_MAX_LEVEL) level = UMI_BL_MAX_LEVEL;

    KeWaitForSingleObject(&g_BlLock, Executive, KernelMode, FALSE, NULL);

    for (u32 i = 0; i < 500; i++) {
        if ((dsi_rd32(REG_DSI_STATUS0) & (DSI_STATUS0_DMA_BUSY | DSI_STATUS0_MDP_BUSY)) == 0)
            break;
        KeStallExecutionProcessor(100);
    }

    g_DmaBuf[0] = 0x03;
    g_DmaBuf[1] = 0x00;
    g_DmaBuf[2] = 0x39;
    g_DmaBuf[3] = 0xC0;
    g_DmaBuf[4] = 0x51;
    g_DmaBuf[5] = (u8)((level >> 8) & 0x07u);
    g_DmaBuf[6] = (u8)(level & 0xFFu);
    g_DmaBuf[7] = 0x00;
    __asm__ volatile("dsb sy" ::: "memory");

    u32 old_ctrl = dsi_rd32(REG_DSI_CMD_DMA_CTRL);
    u32 new_ctrl = (old_ctrl | DSI_CMD_DMA_FROM_FB) & ~DSI_CMD_DMA_LOW_POWER;
    dsi_wr32(REG_DSI_CMD_DMA_CTRL, new_ctrl);

    u32 intr = dsi_rd32(REG_DSI_INTR_CTRL);
    dsi_wr32(REG_DSI_INTR_CTRL, (intr & 0xAAAAAAAAu) | DSI_INTR_CMD_DMA_DONE);

    dsi_wr32(REG_DSI_DMA_BASE, g_DmaPhys);
    dsi_wr32(REG_DSI_DMA_LEN, 8u);
    dsi_wr32(REG_DSI_TRIG_DMA, 1u);
    __asm__ volatile("dsb sy" ::: "memory");

    BOOLEAN done = FALSE;
    for (u32 i = 0; i < 500; i++) {
        if (dsi_rd32(REG_DSI_INTR_CTRL) & DSI_INTR_CMD_DMA_DONE) {
            done = TRUE;
            break;
        }
        KeStallExecutionProcessor(100);
    }

    if (done) {
        u32 intr2 = dsi_rd32(REG_DSI_INTR_CTRL);
        dsi_wr32(REG_DSI_INTR_CTRL, (intr2 & 0xAAAAAAAAu) | DSI_INTR_CMD_DMA_DONE);
    }
    dsi_wr32(REG_DSI_CMD_DMA_CTRL, old_ctrl);

    KeReleaseMutex(&g_BlLock, FALSE);
    return done;
}

extern "C" VOID UmiBlInit(VOID)
{
    if (g_BlInited) return;
    KeInitializeMutex(&g_BlLock, 0);

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)DSI0_PHYS_BASE;
    g_Dsi0 = (volatile u8 *)MmMapIoSpace(pa, DSI0_SIZE, MmNonCached);

    PHYSICAL_ADDRESS low, high, bound;
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFULL;
    bound.QuadPart = 0;
    g_DmaBuf = (u8 *)MmAllocateContiguousMemorySpecifyCache(64, low, high, bound, MmNonCached);
    if (g_DmaBuf) {
        g_DmaPhys = (u32)MmGetPhysicalAddress(g_DmaBuf).QuadPart;
    }

    g_BlInited = TRUE;
    g_CurrentPct = 70;
    UmiDsiSendDcs51((70u * UMI_BL_MAX_LEVEL) / 100u);
}

extern "C" VOID UmiBlCleanup(VOID)
{
    if (!g_BlInited) return;
    if (g_DmaBuf) { MmFreeContiguousMemory(g_DmaBuf); g_DmaBuf = NULL; }
    if (g_Dsi0)   { MmUnmapIoSpace((PVOID)g_Dsi0, DSI0_SIZE); g_Dsi0 = NULL; }
    g_BlInited = FALSE;
}

static VOID UmiBlRefNoOp(_In_ PVOID Context)   { UNREFERENCED_PARAMETER(Context); }
static VOID UmiBlDerefNoOp(_In_ PVOID Context) { UNREFERENCED_PARAMETER(Context); }

static NTSTATUS UmiBlGetPossible(
    _In_ PVOID Context,
    _In_ ULONG BufferSize,
    _Out_ PUCHAR LevelCount,
    _Out_writes_bytes_to_(BufferSize, *LevelCount) PUCHAR BrightnessLevels)
{
    UNREFERENCED_PARAMETER(Context);
    if (!LevelCount || !BrightnessLevels) return STATUS_INVALID_PARAMETER;
    if (BufferSize < 101) {
        *LevelCount = 101;
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UCHAR i = 0; i <= 100; i++) {
        BrightnessLevels[i] = i;
    }
    *LevelCount = 101;
    return STATUS_SUCCESS;
}

static NTSTATUS UmiBlSet(_In_ PVOID Context, _In_ UCHAR Brightness)
{
    UNREFERENCED_PARAMETER(Context);
    if (Brightness > 100) Brightness = 100;
    u32 level = (Brightness == 0) ? 10u : (((u32)Brightness * UMI_BL_MAX_LEVEL) / 100u);
    if (level < 10u) level = 10u;
    g_CurrentPct = Brightness;
    UmiDsiSendDcs51(level);
    return STATUS_SUCCESS;
}

static NTSTATUS UmiBlGet(_In_ PVOID Context, _Out_ PUCHAR Brightness)
{
    UNREFERENCED_PARAMETER(Context);
    if (!Brightness) return STATUS_INVALID_PARAMETER;
    *Brightness = g_CurrentPct;
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS UmiDdiQueryInterface(
    _In_ CONST PVOID MiniportDeviceContext,
    _In_ PQUERY_INTERFACE QueryInterface)
{
    if (!QueryInterface || !QueryInterface->InterfaceType || !QueryInterface->Interface)
        return STATUS_INVALID_PARAMETER;

    if (RtlEqualMemory(QueryInterface->InterfaceType, &UMI_GUID_DEVINTERFACE_BRIGHTNESS, sizeof(GUID))) {
        if (QueryInterface->Version < DXGK_BRIGHTNESS_INTERFACE_VERSION_1 ||
            QueryInterface->Size < sizeof(DXGK_BRIGHTNESS_INTERFACE)) {
            return STATUS_NOT_SUPPORTED;
        }
        PDXGK_BRIGHTNESS_INTERFACE iface = (PDXGK_BRIGHTNESS_INTERFACE)QueryInterface->Interface;
        iface->Size = sizeof(DXGK_BRIGHTNESS_INTERFACE);
        iface->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
        iface->Context = MiniportDeviceContext;
        iface->InterfaceReference = UmiBlRefNoOp;
        iface->InterfaceDereference = UmiBlDerefNoOp;
        iface->GetPossibleBrightness = UmiBlGetPossible;
        iface->SetBrightness = UmiBlSet;
        iface->GetBrightness = UmiBlGet;
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}
