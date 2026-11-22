/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Xiaomi Mi 10 (umi) QCA6390 Bluetooth UART HCI Bus Driver (umi-bt.sys)
 *
 * Drives SM8250 QUP0 SE6 (GENI UART6 @ 0x00998000, GIC SPI 607) + TLMM NORTH
 * GPIOs 16..19 (UART6) & GPIO 21 (BT_EN), downloads QCA6390 firmware
 * (htbtfw20.tlv + htnv20.bin), and exposes a child PDO (MS_BTHX_BTHMINI)
 * for Windows inbox bthmini.sys / bthport.sys.
 */

#include <ntddk.h>
#include <wdf.h>
#include <bthxddi.h>
#include <stdarg.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define UMI_BT_TAG 'TBmU'
#define BIT(n)     (1u << (n))

/* ---- SM8250 Hardware Bases ---- */
#define GCC_PHYS_BASE        0x00100000ULL
#define GCC_SIZE             0x000A0000UL

#define TLMM_NORTH_PHYS_BASE 0x0F900000ULL
#define TLMM_NORTH_SIZE      0x00020000UL

#define UART6_PHYS_BASE      0x00998000ULL
#define UART6_SIZE           0x00004000UL

/* GCC QUP0 wrap0 + SE6 clock registers (from Linux gcc-sm8250.c) */
#define GCC_APCS_CLK_BRANCH_ENA_VOTE_1  0x52008u
#define GCC_QUPV3_WRAP0_M_AHB_BIT       BIT(6)
#define GCC_QUPV3_WRAP0_S_AHB_BIT       BIT(7)
#define GCC_QUPV3_WRAP0_CORE_BIT        BIT(8)
#define GCC_QUPV3_WRAP0_CORE_2X_BIT     BIT(9)
#define GCC_QUPV3_WRAP0_S6_BIT          BIT(16)

#define GCC_QUPV3_WRAP0_M_AHB_CBCR      0x17004u
#define GCC_QUPV3_WRAP0_S_AHB_CBCR      0x17008u
#define GCC_QUPV3_WRAP0_CORE_CBCR       0x23000u
#define GCC_QUPV3_WRAP0_CORE_2X_CBCR    0x23008u
#define GCC_QUPV3_WRAP0_S6_CBCR         0x1772Cu
#define GCC_QUPV3_WRAP0_S6_CMD_RCGR     0x17730u

/* GENI UART registers (from Linux geni-se.h & qcom_geni_serial.c) */
#define GENI_FORCE_DEFAULT_REG          0x020u
#define GENI_OUTPUT_CTRL                0x024u
#define SE_GENI_CGC_CTRL                0x028u
#define SE_GENI_STATUS                  0x040u
#define GENI_SER_M_CLK_CFG              0x048u
#define GENI_SER_S_CLK_CFG              0x04Cu
#define GENI_FW_REVISION_RO             0x068u
#define SE_GENI_CLK_SEL                 0x07Cu
#define SE_UART_LOOPBACK_CFG            0x22Cu
#define SE_GENI_DMA_MODE_EN             0x258u
#define SE_UART_TX_TRANS_CFG            0x25Cu
#define SE_GENI_TX_PACKING_CFG0         0x260u
#define SE_GENI_TX_PACKING_CFG1         0x264u
#define SE_UART_TX_WORD_LEN             0x268u
#define SE_UART_TX_STOP_BIT_LEN         0x26Cu
#define SE_UART_TX_TRANS_LEN            0x270u
#define SE_UART_RX_TRANS_CFG            0x280u
#define SE_GENI_RX_PACKING_CFG0         0x284u
#define SE_GENI_RX_PACKING_CFG1         0x288u
#define SE_UART_RX_WORD_LEN             0x28Cu
#define SE_UART_RX_STALE_CNT            0x294u
#define SE_UART_TX_PARITY_CFG           0x2A4u
#define SE_UART_RX_PARITY_CFG           0x2A8u
#define SE_UART_MANUAL_RFR              0x2ACu
#define SE_GENI_M_CMD0                  0x600u
#define SE_GENI_M_CMD_CTRL_REG          0x604u
#define SE_GENI_M_IRQ_STATUS            0x610u
#define SE_GENI_M_IRQ_EN                0x614u
#define SE_GENI_M_IRQ_CLEAR             0x618u
#define SE_GENI_S_CMD0                  0x630u
#define SE_GENI_S_CMD_CTRL_REG          0x634u
#define SE_GENI_S_IRQ_STATUS            0x640u
#define SE_GENI_S_IRQ_EN                0x644u
#define SE_GENI_S_IRQ_CLEAR             0x648u
#define SE_GENI_TX_FIFOn                0x700u
#define SE_GENI_RX_FIFOn                0x780u
#define SE_GENI_TX_FIFO_STATUS          0x800u
#define SE_GENI_RX_FIFO_STATUS          0x804u
#define SE_GENI_TX_WATERMARK_REG        0x80Cu
#define SE_GENI_RX_WATERMARK_REG        0x810u
#define SE_GENI_RX_RFR_WATERMARK_REG    0x814u
#define SE_GSI_EVENT_EN                 0xE18u
#define SE_IRQ_EN                       0xE1Cu
#define SE_HW_PARAM_0                   0xE24u
#define SE_HW_PARAM_1                   0xE28u
#define SE_DMA_GENERAL_CFG              0xE30u

#define M_GENI_CMD_ACTIVE               BIT(0)
#define S_GENI_CMD_ACTIVE               BIT(12)
#define M_CMD_DONE_EN                   BIT(0)
#define M_CMD_CANCEL_EN                 BIT(4)
#define M_CMD_ABORT_EN                  BIT(5)
#define S_CMD_CANCEL_EN                 BIT(4)
#define S_CMD_ABORT_EN                  BIT(5)
#define S_RX_FIFO_WATERMARK_EN          BIT(26)
#define S_RX_FIFO_LAST_EN               BIT(27)
#define GENI_M_IRQ_EN                   BIT(2)
#define GENI_S_IRQ_EN                   BIT(3)
#define RX_LAST                         BIT(31)
#define RX_LAST_BYTE_VALID_MSK          (7u << 28)
#define RX_LAST_BYTE_VALID_SHFT         28
#define RX_FIFO_WC_MSK                  0x01FFFFFFu

/* QCA6390 EDL / HCI constants */
#define QCA_BAUDRATE_115200             0x00
#define QCA_BAUDRATE_2000000            0x0D
#define QCA_BAUDRATE_3200000            0x11

#define EDL_PATCH_CMD_OPCODE            0xFC00
#define EDL_PATCH_VER_REQ_CMD           0x19
#define EDL_PATCH_TLV_REQ_CMD           0x1E
#define MAX_SIZE_PER_TLV_SEGMENT        243

#define EDL_TAG_ID_BD_ADDR              2
#define EDL_TAG_ID_HCI                  17
#define EDL_TAG_ID_DEEP_SLEEP           27

#define QCA_SKIP_EVT_NONE               0
#define QCA_SKIP_EVT_VSE                1
#define QCA_SKIP_EVT_CC                 2
#define QCA_SKIP_EVT_VSE_CC             3

/* ---- Simple File Logger (\SystemRoot\Temp\umi-bt.log) ---- */
static HANDLE g_LogHandle = NULL;

static void bt_log_init(void)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&path, L"\\SystemRoot\\Temp\\umi-bt.log");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    ZwCreateFile(&g_LogHandle, FILE_GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                 FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0);
}

static void bt_log_close(void)
{
    if (g_LogHandle) {
        ZwClose(g_LogHandle);
        g_LogHandle = NULL;
    }
}

static void bt_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int len = 0;

    /* Minimal formatting via vsnprintf-like RtlStringCbVPrintfA? Use hand-rolled safe formatter */
    va_start(ap, fmt);
    const char *p = fmt;
    while (*p && len < (int)sizeof(buf) - 3) {
        if (*p == '%' && p[1]) {
            p++;
            if (*p == 's') {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && len < (int)sizeof(buf) - 3) buf[len++] = *s++;
            } else if (*p == 'u' || *p == 'd') {
                int is_signed = (*p == 'd');
                long long v = is_signed ? (long long)va_arg(ap, int) : (long long)va_arg(ap, unsigned int);
                if (is_signed && v < 0) { buf[len++] = '-'; v = -v; }
                char tmp[24]; int n = 0;
                do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 24);
                while (n-- > 0 && len < (int)sizeof(buf) - 3) buf[len++] = tmp[n];
            } else if (*p == 'x' || *p == 'X') {
                unsigned int v = va_arg(ap, unsigned int);
                const char *hex = (*p == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                char tmp[16]; int n = 0;
                do { tmp[n++] = hex[v & 0xF]; v >>= 4; } while (v && n < 16);
                while (n-- > 0 && len < (int)sizeof(buf) - 3) buf[len++] = tmp[n];
            } else {
                buf[len++] = *p;
            }
            p++;
        } else {
            buf[len++] = *p++;
        }
    }
    va_end(ap);
    buf[len++] = '\r';
    buf[len++] = '\n';
    buf[len] = '\0';

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "umi-bt: %s", buf);
    if (g_LogHandle && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        IO_STATUS_BLOCK iosb;
        ZwWriteFile(g_LogHandle, NULL, NULL, NULL, &iosb, buf, (ULONG)len, NULL, NULL);
    }
}

/* ---- MMIO & Timing Helpers ---- */
static __inline u32 rd32(volatile u8 *base, u32 off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(base + off));
}

static __inline void wr32(volatile u8 *base, u32 off, u32 val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(base + off), val);
}

static __inline void sleep_ms(u32 ms)
{
    LARGE_INTEGER iv;
    iv.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &iv);
}

static __inline u64 now_ms(void)
{
    return (u64)KeQueryInterruptTime() / 10000ULL;
}

/* ---- Firmware File Loader ---- */
static u8 *load_fw_file(const WCHAR *full_path, u32 *out_size)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    FILE_STANDARD_INFORMATION fsi;
    NTSTATUS st;
    u8 *buf = NULL;

    *out_size = 0;
    RtlInitUnicodeString(&path, full_path);
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    st = ZwCreateFile(&h, FILE_GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                      FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                      FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) {
        bt_log("load_fw open failed st=0x%x", (u32)st);
        return NULL;
    }

    st = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (!NT_SUCCESS(st) || fsi.EndOfFile.QuadPart <= 0 || fsi.EndOfFile.QuadPart > 1024 * 1024) {
        bt_log("load_fw query failed st=0x%x", (u32)st);
        ZwClose(h);
        return NULL;
    }

    u32 sz = (u32)fsi.EndOfFile.QuadPart;
    buf = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, UMI_BT_TAG);
    if (!buf) {
        ZwClose(h);
        return NULL;
    }

    st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sz, NULL, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(st) || iosb.Information != sz) {
        ExFreePoolWithTag(buf, UMI_BT_TAG);
        return NULL;
    }

    *out_size = sz;
    return buf;
}

/* ---- H4 Packet Queue & Driver Context ---- */
#define RX_RING_SIZE   16384
#define MAX_HCI_PKT    2048

typedef struct _HCI_RX_PACKET {
    LIST_ENTRY Link;
    u8         Type;
    u32        Len;
    u8         Data[1];
} HCI_RX_PACKET;

typedef struct _FDO_CONTEXT {
    WDFDEVICE       Device;
    WDFDEVICE       ChildPdo;
    WDFINTERRUPT    Interrupt;
    WDFTIMER        RxPollTimer;
    WDFWORKITEM     IbsAckWorkItem;
    WDFQUEUE        ReadEventQueue;
    WDFQUEUE        ReadAclQueue;
    WDFQUEUE        WriteQueue;

    KMUTEX          TxMutex;
    KEVENT          WakeAckEvent;
    volatile BOOLEAN ChipSleeping;
    u32             IbsWakeIndCount;

    volatile u8    *Gcc;
    volatile u8    *Tlmm;
    volatile u8    *Uart;
    BOOLEAN         UartMappedFromCrs;

    u32             TxFifoDepth;
    u32             RxFifoDepth;
    BOOLEAN         IrqRunning;
    BOOLEAN         HwReady;

    /* Raw byte ring written by ISR, consumed by DPC */
    WDFSPINLOCK     RingLock;
    u8              RxRing[RX_RING_SIZE];
    u32             RxHead;
    u32             RxTail;

    /* H4 Framer state (DPC-only) */
    u32             FrameState; /* 0=type, 1=hdr, 2=payload */
    u8              FrameType;
    u32             FrameHdrLen;
    u32             FrameExpected;
    u32             FramePos;
    u8              FrameBuf[MAX_HCI_PKT];

    /* Completed RX H4 packets waiting for IOCTL_BTHX_READ_HCI (split by Event vs ACL!) */
    WDFSPINLOCK     PktLock;
    LIST_ENTRY      RxEventList;
    u32             RxEventCount;
    LIST_ENTRY      RxAclList;
    u32             RxAclCount;

    /* Stats */
    u32             TxCount;
    u32             RxCount;
    u32             IsrCount;
    u32             PollDrainCount;
    u32             RxBytesTotal;
    u32             LastSIrq;
    u8              FirstRxBytes[16];
    u32             FirstRxLen;
    u32             IoctlLogCount;
} FDO_CONTEXT, *PFDO_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FDO_CONTEXT, GetFdoContext)

typedef struct _PDO_CONTEXT {
    PFDO_CONTEXT Fdo;
} PDO_CONTEXT, *PPDO_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PDO_CONTEXT, GetPdoContext)

/* ---- GCC Clock Setup for QUP0 SE6 (UART6) ---- */
static BOOLEAN gcc_enable_vote_bit(volatile u8 *gcc, u32 vote_bit, u32 cbcr_off)
{
    u32 v = rd32(gcc, GCC_APCS_CLK_BRANCH_ENA_VOTE_1);
    wr32(gcc, GCC_APCS_CLK_BRANCH_ENA_VOTE_1, v | vote_bit);
    for (u32 i = 0; i < 1000; i++) {
        if ((rd32(gcc, cbcr_off) & BIT(31)) == 0) return TRUE;
        KeStallExecutionProcessor(10);
    }
    bt_log("gcc vote bit 0x%x cbcr 0x%x timeout (val=0x%x)", vote_bit, cbcr_off, rd32(gcc, cbcr_off));
    return FALSE;
}

static BOOLEAN gcc_set_uart6_rcg(volatile u8 *gcc, u32 src_sel, u32 hid_div, u32 m, u32 n)
{
    u32 cmd = GCC_QUPV3_WRAP0_S6_CMD_RCGR;
    u32 cfg = (src_sel << 8) | (hid_div & 0x1Fu);
    if (n != 0) {
        cfg |= (2u << 12); /* Dual-edge MND mode */
        wr32(gcc, cmd + 0x08, m & 0xFFFFu);
        wr32(gcc, cmd + 0x0C, (~(n - m)) & 0xFFFFu);
        wr32(gcc, cmd + 0x10, (~n) & 0xFFFFu);
    }
    wr32(gcc, cmd + 0x04, cfg);
    wr32(gcc, cmd, rd32(gcc, cmd) | 1u);
    for (u32 i = 0; i < 1000; i++) {
        if ((rd32(gcc, cmd) & 1u) == 0) return TRUE;
        KeStallExecutionProcessor(10);
    }
    bt_log("gcc_set_uart6_rcg update timeout cmd=0x%x", rd32(gcc, cmd));
    return FALSE;
}

/* ---- TLMM NORTH Tile GPIO Setup ---- */
static void tlmm_cfg_pin(volatile u8 *tlmm, u32 pin, u32 func, u32 pull, u32 drv, BOOLEAN out_en, BOOLEAN out_hi)
{
    u32 base = pin * 0x1000u;
    if (out_en) {
        u32 io = rd32(tlmm, base + 4);
        io = (io & ~BIT(1)) | (out_hi ? BIT(1) : 0u);
        wr32(tlmm, base + 4, io);
    }
    u32 cfg = (pull & 3u) | ((func & 0xFu) << 2) | ((drv & 7u) << 6) | (out_en ? BIT(9) : 0u);
    wr32(tlmm, base, cfg);
}

/* ---- GENI UART6 Operations ---- */
static void uart_set_baud(PFDO_CONTEXT fdo, u32 baud)
{
    /*
     * SM8250 QUPv3 >= v2.5 uses oversampling = 16.
     * 115200 baud: 7,372,800 Hz (GPLL0_EVEN 300M * 384 / 15625), clk_div = 4 -> 7372800 / 64 = 115200 exact.
     * 2000000 baud: 32,000,000 Hz (GPLL0_EVEN 300M * 8 / 75), clk_div = 1 -> 32000000 / 16 = 2000000 exact.
     */
    u32 clk_div;
    if (baud == 2000000) {
        gcc_set_uart6_rcg(fdo->Gcc, 6, 1, 8, 75);
        clk_div = 1;
    } else {
        gcc_set_uart6_rcg(fdo->Gcc, 6, 1, 384, 15625);
        clk_div = 4;
    }
    u32 ser_clk = 1u | (clk_div << 4);
    wr32(fdo->Uart, GENI_SER_M_CLK_CFG, ser_clk);
    wr32(fdo->Uart, GENI_SER_S_CLK_CFG, ser_clk);
    wr32(fdo->Uart, SE_GENI_CLK_SEL, 0);
}

static void uart_stop_rx(PFDO_CONTEXT fdo)
{
    wr32(fdo->Uart, SE_GENI_S_IRQ_EN, 0);
    if (rd32(fdo->Uart, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) {
        wr32(fdo->Uart, SE_GENI_S_CMD_CTRL_REG, BIT(2)); /* CANCEL */
        for (u32 i = 0; i < 1000; i++) {
            if (rd32(fdo->Uart, SE_GENI_S_IRQ_STATUS) & S_CMD_CANCEL_EN) break;
            KeStallExecutionProcessor(10);
        }
        if (rd32(fdo->Uart, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) {
            wr32(fdo->Uart, SE_GENI_S_CMD_CTRL_REG, BIT(1)); /* ABORT */
            for (u32 i = 0; i < 1000; i++) {
                if (rd32(fdo->Uart, SE_GENI_S_IRQ_STATUS) & S_CMD_ABORT_EN) break;
                KeStallExecutionProcessor(10);
            }
        }
    }
    /* Drain stale RX FIFO */
    for (u32 i = 0; i < 64; i++) {
        if ((rd32(fdo->Uart, SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MSK) == 0) break;
        (void)rd32(fdo->Uart, SE_GENI_RX_FIFOn);
    }
    wr32(fdo->Uart, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
}

static void uart_start_rx(PFDO_CONTEXT fdo)
{
    if (!(rd32(fdo->Uart, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE)) {
        wr32(fdo->Uart, SE_GENI_S_CMD0, (1u << 27)); /* UART_START_READ */
    }
}

static BOOLEAN uart_tx_bytes(PFDO_CONTEXT fdo, const u8 *buf, u32 len)
{
    u32 offset = 0;
    while (offset < len) {
        u32 chunk = len - offset;
        if (chunk > 48) chunk = 48; /* Keep within 64-byte FIFO */

        /* Wait for previous M_CMD to complete */
        u64 deadline = now_ms() + 1000;
        while (rd32(fdo->Uart, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) {
            if (now_ms() > deadline) {
                bt_log("uart_tx timeout waiting idle status=0x%x", rd32(fdo->Uart, SE_GENI_STATUS));
                return FALSE;
            }
            KeStallExecutionProcessor(20);
        }

        wr32(fdo->Uart, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
        wr32(fdo->Uart, SE_UART_TX_TRANS_LEN, chunk);
        wr32(fdo->Uart, SE_GENI_M_CMD0, (1u << 27)); /* UART_START_TX */

        u32 i = 0;
        while (i < chunk) {
            /* Wait if TX FIFO full */
            while ((rd32(fdo->Uart, SE_GENI_TX_FIFO_STATUS) & 0x0FFFFFFFu) >= fdo->TxFifoDepth) {
                if (now_ms() > deadline) return FALSE;
                KeStallExecutionProcessor(10);
            }
            u32 word = 0;
            u32 n = (chunk - i >= 4) ? 4 : (chunk - i);
            for (u32 b = 0; b < n; b++) {
                word |= ((u32)buf[offset + i + b]) << (b * 8);
            }
            wr32(fdo->Uart, SE_GENI_TX_FIFOn, word);
            i += n;
        }

        /* Wait for M_CMD_DONE */
        while (!(rd32(fdo->Uart, SE_GENI_M_IRQ_STATUS) & M_CMD_DONE_EN)) {
            if (now_ms() > deadline) {
                bt_log("uart_tx timeout waiting M_CMD_DONE m_irq=0x%x status=0x%x",
                       rd32(fdo->Uart, SE_GENI_M_IRQ_STATUS), rd32(fdo->Uart, SE_GENI_STATUS));
                return FALSE;
            }
            KeStallExecutionProcessor(20);
        }
        wr32(fdo->Uart, SE_GENI_M_IRQ_CLEAR, M_CMD_DONE_EN);
        offset += chunk;
    }
    return TRUE;
}

/* Polled RX helper used ONLY during PrepareHardware firmware download */
static u32 uart_poll_drain_to_ring(PFDO_CONTEXT fdo)
{
    u32 total = 0;
    u32 s_irq = rd32(fdo->Uart, SE_GENI_S_IRQ_STATUS);
    if (s_irq) wr32(fdo->Uart, SE_GENI_S_IRQ_CLEAR, s_irq);

    u32 st = rd32(fdo->Uart, SE_GENI_RX_FIFO_STATUS);
    u32 wc = st & RX_FIFO_WC_MSK;
    BOOLEAN rx_last = (st & RX_LAST) != 0;
    u32 last_valid = (st & RX_LAST_BYTE_VALID_MSK) >> RX_LAST_BYTE_VALID_SHFT;

    if (wc == 0) return 0;

    for (u32 w = 0; w < wc; w++) {
        u32 word = rd32(fdo->Uart, SE_GENI_RX_FIFOn);
        u32 nb = 4;
        if (rx_last && w == wc - 1 && last_valid > 0 && last_valid < 4) {
            nb = last_valid;
        }
        for (u32 b = 0; b < nb; b++) {
            u32 next = (fdo->RxHead + 1) % RX_RING_SIZE;
            if (next != fdo->RxTail) {
                fdo->RxRing[fdo->RxHead] = (u8)((word >> (b * 8)) & 0xFFu);
                fdo->RxHead = next;
                total++;
            }
        }
    }
    return total;
}

static BOOLEAN uart_poll_read_byte(PFDO_CONTEXT fdo, u8 *out, u32 timeout_ms)
{
    u64 deadline = now_ms() + timeout_ms;
    while (fdo->RxTail == fdo->RxHead) {
        uart_poll_drain_to_ring(fdo);
        if (fdo->RxTail != fdo->RxHead) break;
        if (now_ms() > deadline) return FALSE;
        KeStallExecutionProcessor(50);
    }
    *out = fdo->RxRing[fdo->RxTail];
    fdo->RxTail = (fdo->RxTail + 1) % RX_RING_SIZE;
    return TRUE;
}

/* Read one complete H4 Event packet (type 0x04) during init */
static BOOLEAN qca_poll_read_event(PFDO_CONTEXT fdo, u8 *evt_code, u8 *payload, u32 max_len, u32 *out_len, u32 timeout_ms)
{
    u8 b = 0;
    u64 deadline = now_ms() + timeout_ms;
    while (now_ms() <= deadline) {
        if (!uart_poll_read_byte(fdo, &b, 50)) continue;
        if (b == 0x04) {
            u8 ec = 0, plen = 0;
            if (!uart_poll_read_byte(fdo, &ec, 500)) return FALSE;
            if (!uart_poll_read_byte(fdo, &plen, 500)) return FALSE;
            for (u32 i = 0; i < plen; i++) {
                u8 d = 0;
                if (!uart_poll_read_byte(fdo, &d, 500)) return FALSE;
                if (i < max_len) payload[i] = d;
            }
            *evt_code = ec;
            *out_len = plen;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN qca_send_hci_cmd(PFDO_CONTEXT fdo, u16 opcode, const u8 *params, u8 plen)
{
    u8 pkt[260];
    pkt[0] = 0x01; /* HciPacketCommand */
    pkt[1] = (u8)(opcode & 0xFFu);
    pkt[2] = (u8)((opcode >> 8) & 0xFFu);
    pkt[3] = plen;
    if (plen && params) RtlCopyMemory(pkt + 4, params, plen);
    return uart_tx_bytes(fdo, pkt, (u32)plen + 4);
}

/* ---- QCA6390 Firmware Download ---- */
static BOOLEAN qca_download_tlv_buf(PFDO_CONTEXT fdo, u8 *data, u32 size, u8 dnld_mode, const char *label)
{
    u32 offset = 0;
    u32 seg_idx = 0;
    u8 cmd[MAX_SIZE_PER_TLV_SEGMENT + 2];

    bt_log("qca_download %s: size=%u dnld_mode=%u", label, size, (u32)dnld_mode);

    while (offset < size) {
        u32 remain = size - offset;
        u32 seg = (remain > MAX_SIZE_PER_TLV_SEGMENT) ? MAX_SIZE_PER_TLV_SEGMENT : remain;
        BOOLEAN is_last = (remain <= MAX_SIZE_PER_TLV_SEGMENT);

        cmd[0] = EDL_PATCH_TLV_REQ_CMD;
        cmd[1] = (u8)seg;
        RtlCopyMemory(cmd + 2, data + offset, seg);

        if (!qca_send_hci_cmd(fdo, EDL_PATCH_CMD_OPCODE, cmd, (u8)(seg + 2))) {
            bt_log("qca_download %s tx failed at seg %u", label, seg_idx);
            return FALSE;
        }

        /*
         * For QCA6390:
         * - If dnld_mode == QCA_SKIP_EVT_NONE (0): controller sends Command Complete (0x0E) after every segment.
         * - If dnld_mode == QCA_SKIP_EVT_VSE_CC (3): controller skips CC/VSE on all intermediate segments,
         *   and sends a VSE/CC only on the last segment (or none; we drain/wait briefly on last).
         */
        if (dnld_mode == QCA_SKIP_EVT_NONE || is_last) {
            u8 ec = 0, rsp[64];
            u32 rlen = 0;
            if (!qca_poll_read_event(fdo, &ec, rsp, sizeof(rsp), &rlen, 2000)) {
                if (!is_last) {
                    bt_log("qca_download %s rsp timeout at seg %u", label, seg_idx);
                    return FALSE;
                }
                /* Some QCA6390 builds skip even the final CC when dnld_mode==3; verify via HCI Reset next */
                bt_log("qca_download %s final evt skipped (dnld_mode=%u)", label, (u32)dnld_mode);
            }
        } else {
            /* Brief pacing so controller RAM buffer never overflows */
            KeStallExecutionProcessor(100);
        }

        offset += seg;
        seg_idx++;
    }

    bt_log("qca_download %s complete (%u segments)", label, seg_idx);
    return TRUE;
}

static void qca_patch_nvm(u8 *data, u32 size, u8 target_baud_code)
{
    if (size < 4) return;
    u32 type_len = *(u32 *)data;
    u8 type = (u8)(type_len & 0xFFu);
    u32 length = type_len >> 8;
    u32 base_off = 4;

    if (type == 4 && size >= 8) {
        type_len = *(u32 *)(data + 4);
        type = (u8)(type_len & 0xFFu);
        length = type_len >> 8;
        base_off = 8;
    }
    if (type != 2 || base_off + length > size) return;

    u32 idx = 0;
    while (idx + 12 <= length) {
        u8 *hdr = data + base_off + idx;
        u16 tag_id  = (u16)(hdr[0] | ((u16)hdr[1] << 8));
        u16 tag_len = (u16)(hdr[2] | ((u16)hdr[3] << 8));
        if (idx + 12 + tag_len > length) break;
        u8 *val = hdr + 12;

        if (tag_id == EDL_TAG_ID_HCI && tag_len >= 3) {
            /* Disable IBS (bit 7 = 0) so plain H4 UART works cleanly, set target baudrate */
            val[0] &= (u8)~0x80u;
            val[1] = target_baud_code;
            bt_log("patched NVM tag 17 (HCI): b0=0x%x baud=0x%x", (u32)val[0], (u32)val[1]);
        } else if (tag_id == EDL_TAG_ID_DEEP_SLEEP && tag_len >= 1) {
            /* Disable deep sleep so controller stays responsive on UART */
            val[0] &= (u8)~0x01u;
            bt_log("patched NVM tag 27 (DeepSleep): b0=0x%x", (u32)val[0]);
        } else if (tag_id == EDL_TAG_ID_BD_ADDR && tag_len == 6) {
            /* Ensure a valid locally-administered BD_ADDR if stock NVM has placeholder */
            if (val[2] == 0 && val[3] == 0 && val[4] == 0 && val[5] == 0) {
                val[0] = 0x10; val[1] = 0x50; val[2] = 0x82;
                val[3] = 0xAD; val[4] = 0x5A; val[5] = 0x02;
                bt_log("patched NVM tag 2 (BD_ADDR) to 02:5A:AD:82:50:10");
            }
        }
        idx += 12 + tag_len;
    }
}

static BOOLEAN umi_bt_hw_and_qca_init(PFDO_CONTEXT fdo)
{
    /* 1. Enable QUP0 wrapper + SE6 clocks */
    gcc_enable_vote_bit(fdo->Gcc, GCC_QUPV3_WRAP0_M_AHB_BIT, GCC_QUPV3_WRAP0_M_AHB_CBCR);
    gcc_enable_vote_bit(fdo->Gcc, GCC_QUPV3_WRAP0_S_AHB_BIT, GCC_QUPV3_WRAP0_S_AHB_CBCR);
    gcc_enable_vote_bit(fdo->Gcc, GCC_QUPV3_WRAP0_CORE_BIT, GCC_QUPV3_WRAP0_CORE_CBCR);
    gcc_enable_vote_bit(fdo->Gcc, GCC_QUPV3_WRAP0_CORE_2X_BIT, GCC_QUPV3_WRAP0_CORE_2X_CBCR);

    gcc_set_uart6_rcg(fdo->Gcc, 6, 1, 384, 15625); /* 7,372,800 Hz */
    gcc_enable_vote_bit(fdo->Gcc, GCC_QUPV3_WRAP0_S6_BIT, GCC_QUPV3_WRAP0_S6_CBCR);

    /* 2. Configure TLMM NORTH GPIOs: 16=CTS, 17=RTS, 18=TX, 19=RX (func 1 = qup6), 21=BT_EN */
    tlmm_cfg_pin(fdo->Tlmm, 16, 1, 0, 0, FALSE, FALSE); /* CTS: no pull */
    tlmm_cfg_pin(fdo->Tlmm, 17, 1, 0, 0, FALSE, FALSE); /* RTS: no pull */
    tlmm_cfg_pin(fdo->Tlmm, 18, 1, 0, 0, FALSE, FALSE); /* TX: no pull */
    tlmm_cfg_pin(fdo->Tlmm, 19, 1, 3, 0, FALSE, FALSE); /* RX: pull-up */

    /* Pulse BT_EN LOW -> HIGH for clean QCA6390 BT reset */
    tlmm_cfg_pin(fdo->Tlmm, 21, 0, 0, 0, TRUE, FALSE);
    sleep_ms(30);
    tlmm_cfg_pin(fdo->Tlmm, 21, 0, 0, 0, TRUE, TRUE);
    sleep_ms(180);

    /* 3. Inspect and initialize GENI UART6 */
    u32 fw_rev = rd32(fdo->Uart, GENI_FW_REVISION_RO);
    u32 proto = (fw_rev >> 12) & 0xFu;
    u32 hw0 = rd32(fdo->Uart, SE_HW_PARAM_0);
    u32 hw1 = rd32(fdo->Uart, SE_HW_PARAM_1);
    fdo->TxFifoDepth = (hw0 >> 16) & 0x3Fu;
    fdo->RxFifoDepth = (hw1 >> 16) & 0x3Fu;
    if (fdo->TxFifoDepth == 0) fdo->TxFifoDepth = 16;
    if (fdo->RxFifoDepth == 0) fdo->RxFifoDepth = 16;

    bt_log("UART6 fw_rev=0x%x proto=%u tx_fifo=%u rx_fifo=%u",
           fw_rev, proto, fdo->TxFifoDepth, fdo->RxFifoDepth);
    if (proto != 2) {
        bt_log("WARNING: UART6 proto=%u (expected 2=UART)", proto);
    }

    uart_stop_rx(fdo);
    wr32(fdo->Uart, SE_GENI_M_IRQ_EN, 0);
    wr32(fdo->Uart, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);

    wr32(fdo->Uart, SE_GENI_CGC_CTRL, rd32(fdo->Uart, SE_GENI_CGC_CTRL) | 0x7Fu);
    wr32(fdo->Uart, SE_DMA_GENERAL_CFG, rd32(fdo->Uart, SE_DMA_GENERAL_CFG) | 0x0Fu);
    wr32(fdo->Uart, GENI_OUTPUT_CTRL, 0x7Fu);
    wr32(fdo->Uart, GENI_FORCE_DEFAULT_REG, 1u);

    wr32(fdo->Uart, SE_GENI_DMA_MODE_EN, 0u);
    wr32(fdo->Uart, SE_GSI_EVENT_EN, 0u);
    wr32(fdo->Uart, SE_IRQ_EN, GENI_M_IRQ_EN | GENI_S_IRQ_EN);

    /* 4 x 8-bit LSB-first packing */
    wr32(fdo->Uart, SE_GENI_TX_PACKING_CFG0, 0x0004380Eu);
    wr32(fdo->Uart, SE_GENI_TX_PACKING_CFG1, 0x000C3E0Eu);
    wr32(fdo->Uart, SE_GENI_RX_PACKING_CFG0, 0x0004380Eu);
    wr32(fdo->Uart, SE_GENI_RX_PACKING_CFG1, 0x000C3E0Eu);

    /* 8N1 + Hardware CTS/RTS flow control */
    wr32(fdo->Uart, SE_UART_LOOPBACK_CFG, 0u);
    wr32(fdo->Uart, SE_UART_TX_TRANS_CFG, 0u); /* CTS enabled (bit 1 = 0), no parity */
    wr32(fdo->Uart, SE_UART_RX_TRANS_CFG, 0u);
    wr32(fdo->Uart, SE_UART_TX_PARITY_CFG, 0u);
    wr32(fdo->Uart, SE_UART_RX_PARITY_CFG, 0u);
    wr32(fdo->Uart, SE_UART_TX_WORD_LEN, 8u);
    wr32(fdo->Uart, SE_UART_RX_WORD_LEN, 8u);
    wr32(fdo->Uart, SE_UART_TX_STOP_BIT_LEN, 0u);
    wr32(fdo->Uart, SE_UART_RX_STALE_CNT, 160u);
    wr32(fdo->Uart, SE_UART_MANUAL_RFR, 0u); /* Auto HW RFR */

    wr32(fdo->Uart, SE_GENI_RX_WATERMARK_REG, 2u);
    wr32(fdo->Uart, SE_GENI_RX_RFR_WATERMARK_REG,
         (fdo->RxFifoDepth > 2) ? (fdo->RxFifoDepth - 2) : 2);

    uart_set_baud(fdo, 115200);
    fdo->RxHead = fdo->RxTail = 0;
    uart_start_rx(fdo);
    sleep_ms(20);

    /* 4. QCA6390 EDL Read SoC Version (01 00 FC 01 19) */
    u8 ver_cmd = EDL_PATCH_VER_REQ_CMD;
    u8 ec = 0, rsp[64];
    u32 rlen = 0;
    BOOLEAN got_ver = FALSE;

    for (u32 attempt = 0; attempt < 3 && !got_ver; attempt++) {
        bt_log("Sending QCA EDL_PATCH_VER_REQ_CMD (attempt %u)", attempt + 1);
        if (!qca_send_hci_cmd(fdo, EDL_PATCH_CMD_OPCODE, &ver_cmd, 1)) {
            bt_log("Failed to TX EDL_PATCH_VER_REQ_CMD");
            return FALSE;
        }
        /* QCA6390 sends Command Complete (0x0E) + possibly Vendor Event (0xFF) */
        for (u32 ev = 0; ev < 2; ev++) {
            if (qca_poll_read_event(fdo, &ec, rsp, sizeof(rsp), &rlen, 1000)) {
                bt_log("EDL rsp evt=0x%x len=%u b0=0x%x b1=0x%x b2=0x%x b3=0x%x",
                       (u32)ec, rlen,
                       rlen > 0 ? rsp[0] : 0, rlen > 1 ? rsp[1] : 0,
                       rlen > 2 ? rsp[2] : 0, rlen > 3 ? rsp[3] : 0);
                if (ec == 0xFF || (ec == 0x0E && rlen >= 5)) {
                    got_ver = TRUE;
                }
            }
        }
    }

    if (!got_ver) {
        bt_log("ERROR: QCA6390 did not respond to EDL version query!");
        return FALSE;
    }

    /* 5. Switch QCA6390 & UART6 to 2,000,000 baud for fast firmware download */
    u8 baud_param = QCA_BAUDRATE_2000000;
    bt_log("Switching QCA6390 to 2,000,000 baud (0x%x)", (u32)baud_param);
    if (!qca_send_hci_cmd(fdo, 0xFC48, &baud_param, 1)) {
        bt_log("Failed to send baudrate change command");
        return FALSE;
    }
    sleep_ms(300);
    uart_stop_rx(fdo);
    uart_set_baud(fdo, 2000000);
    fdo->RxHead = fdo->RxTail = 0;
    uart_start_rx(fdo);
    sleep_ms(20);

    /* 6. Load & download htbtfw20.tlv */
    u32 fw_size = 0;
    u8 *fw_buf = load_fw_file(L"\\SystemRoot\\System32\\drivers\\umi-bt\\htbtfw20.tlv", &fw_size);
    if (!fw_buf) {
        bt_log("ERROR: Failed to load htbtfw20.tlv");
        return FALSE;
    }
    u8 patch_dnld_mode = (fw_size > 14) ? fw_buf[14] : QCA_SKIP_EVT_NONE;
    BOOLEAN ok = qca_download_tlv_buf(fdo, fw_buf, fw_size, patch_dnld_mode, "htbtfw20.tlv");
    ExFreePoolWithTag(fw_buf, UMI_BT_TAG);
    if (!ok) return FALSE;
    sleep_ms(50);

    /* 7. Load, patch & download htnv20.bin */
    u32 nvm_size = 0;
    u8 *nvm_buf = load_fw_file(L"\\SystemRoot\\System32\\drivers\\umi-bt\\htnv20.bin", &nvm_size);
    if (!nvm_buf) {
        bt_log("ERROR: Failed to load htnv20.bin");
        return FALSE;
    }
    qca_patch_nvm(nvm_buf, nvm_size, QCA_BAUDRATE_2000000);
    ok = qca_download_tlv_buf(fdo, nvm_buf, nvm_size, QCA_SKIP_EVT_NONE, "htnv20.bin");
    ExFreePoolWithTag(nvm_buf, UMI_BT_TAG);
    if (!ok) return FALSE;
    sleep_ms(50);

    /* 8. Send standard HCI Reset (0x0C03) to verify firmware is live */
    bt_log("Sending HCI Reset (0x0C03)...");
    if (!qca_send_hci_cmd(fdo, 0x0C03, NULL, 0)) return FALSE;
    if (!qca_poll_read_event(fdo, &ec, rsp, sizeof(rsp), &rlen, 2000)) {
        bt_log("ERROR: HCI Reset timed out!");
        return FALSE;
    }
    bt_log("HCI Reset response: evt=0x%x len=%u status=0x%x",
           (u32)ec, rlen, rlen >= 4 ? rsp[3] : 0xFFu);

    fdo->HwReady = TRUE;
    return TRUE;
}

/* ---- Helper to extract METHOD_NEITHER / Buffered IOCTL buffers safely ---- */
static void get_bthx_buffers(WDFREQUEST Request, PVOID *in_buf, size_t *in_sz,
                             PVOID *out_buf, size_t *out_sz)
{
    WDFMEMORY mem = NULL;
    *in_buf = NULL;  *in_sz = 0;
    *out_buf = NULL; *out_sz = 0;

    if (NT_SUCCESS(WdfRequestRetrieveInputMemory(Request, &mem)) && mem) {
        *in_buf = WdfMemoryGetBuffer(mem, in_sz);
    }
    mem = NULL;
    if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &mem)) && mem) {
        *out_buf = WdfMemoryGetBuffer(mem, out_sz);
    }

    /* Fallback for METHOD_NEITHER kernel IRPs from BthMini */
    if (!*in_buf || !*out_buf) {
        PIRP irp = WdfRequestWdmGetIrp(Request);
        if (irp) {
            PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
            if (!*in_buf && sp->Parameters.DeviceIoControl.InputBufferLength > 0) {
                PVOID p = sp->Parameters.DeviceIoControl.Type3InputBuffer;
                if (!p) p = irp->AssociatedIrp.SystemBuffer;
                if (p) {
                    *in_buf = p;
                    *in_sz = sp->Parameters.DeviceIoControl.InputBufferLength;
                }
            }
            if (!*out_buf && sp->Parameters.DeviceIoControl.OutputBufferLength > 0) {
                PVOID p = irp->UserBuffer;
                if (!p) p = irp->AssociatedIrp.SystemBuffer;
                if (p) {
                    *out_buf = p;
                    *out_sz = sp->Parameters.DeviceIoControl.OutputBufferLength;
                }
            }
        }
    }
}

/* ---- RX Packet Delivery to BthMini ---- */
static void umi_bt_try_complete_read_type(PFDO_CONTEXT fdo, u8 pkt_type)
{
    WDFQUEUE queue = (pkt_type == HciPacketEvent) ? fdo->ReadEventQueue : fdo->ReadAclQueue;
    PLIST_ENTRY list = (pkt_type == HciPacketEvent) ? &fdo->RxEventList : &fdo->RxAclList;
    u32 *count = (pkt_type == HciPacketEvent) ? &fdo->RxEventCount : &fdo->RxAclCount;

    for (;;) {
        WDFREQUEST req = NULL;
        HCI_RX_PACKET *pkt = NULL;

        WdfSpinLockAcquire(fdo->PktLock);
        if (!IsListEmpty(list)) {
            NTSTATUS st = WdfIoQueueRetrieveNextRequest(queue, &req);
            if (NT_SUCCESS(st) && req != NULL) {
                PLIST_ENTRY entry = RemoveHeadList(list);
                pkt = CONTAINING_RECORD(entry, HCI_RX_PACKET, Link);
                (*count)--;
            }
        }
        WdfSpinLockRelease(fdo->PktLock);

        if (!req || !pkt) break;

        PVOID in_buf = NULL, out_buf = NULL;
        size_t in_sz = 0, out_sz = 0;
        get_bthx_buffers(req, &in_buf, &in_sz, &out_buf, &out_sz);

        size_t needed = FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + pkt->Len;
        if (out_buf && needed <= out_sz) {
            PBTHX_HCI_READ_WRITE_CONTEXT ctx = (PBTHX_HCI_READ_WRITE_CONTEXT)out_buf;
            ctx->Type = pkt->Type;
            ctx->DataLen = pkt->Len;
            RtlCopyMemory(ctx->Data, pkt->Data, pkt->Len);
            WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, needed);
        } else {
            WdfRequestCompleteWithInformation(req, STATUS_BUFFER_TOO_SMALL, needed);
        }

        ExFreePoolWithTag(pkt, UMI_BT_TAG);
    }
}

static void umi_bt_enqueue_rx_packet(PFDO_CONTEXT fdo, u8 type, const u8 *data, u32 len)
{
    if (type != HciPacketEvent && type != HciPacketAclData) return;

    HCI_RX_PACKET *pkt = (HCI_RX_PACKET *)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, FIELD_OFFSET(HCI_RX_PACKET, Data) + len, UMI_BT_TAG);
    if (!pkt) return;

    pkt->Type = type;
    pkt->Len = len;
    RtlCopyMemory(pkt->Data, data, len);

    WdfSpinLockAcquire(fdo->PktLock);
    if (type == HciPacketEvent) {
        if (fdo->RxEventCount < 256) {
            InsertTailList(&fdo->RxEventList, &pkt->Link);
            fdo->RxEventCount++;
            fdo->RxCount++;
            pkt = NULL;
        }
    } else {
        if (fdo->RxAclCount < 256) {
            InsertTailList(&fdo->RxAclList, &pkt->Link);
            fdo->RxAclCount++;
            fdo->RxCount++;
            pkt = NULL;
        }
    }
    WdfSpinLockRelease(fdo->PktLock);

    if (pkt) ExFreePoolWithTag(pkt, UMI_BT_TAG);
}

/* Qualcomm In-Band Sleep (IBS) protocol bytes */
#define HCI_IBS_WAKE_ACK  0xFCu
#define HCI_IBS_WAKE_IND  0xFDu
#define HCI_IBS_SLEEP_IND 0xFEu

static VOID EvtIbsAckWorkItem(WDFWORKITEM WorkItem)
{
    PFDO_CONTEXT fdo = GetFdoContext(WdfWorkItemGetParentObject(WorkItem));
    if (!fdo->Uart || !fdo->HwReady) return;
    u8 ack = HCI_IBS_WAKE_ACK;
    KeWaitForSingleObject(&fdo->TxMutex, Executive, KernelMode, FALSE, NULL);
    uart_tx_bytes(fdo, &ack, 1);
    KeReleaseMutex(&fdo->TxMutex, FALSE);
}

static void umi_bt_feed_h4_byte(PFDO_CONTEXT fdo, u8 b)
{
    if (fdo->FrameState == 0) {
        if (b == HCI_IBS_WAKE_IND) {
            /* QCA6390 wants to transmit: reply 0xFC (WAKE_ACK) immediately! */
            fdo->IbsWakeIndCount++;
            if (fdo->IbsAckWorkItem) WdfWorkItemEnqueue(fdo->IbsAckWorkItem);
            return;
        } else if (b == HCI_IBS_SLEEP_IND) {
            fdo->ChipSleeping = TRUE;
            return;
        } else if (b == HCI_IBS_WAKE_ACK) {
            fdo->ChipSleeping = FALSE;
            KeSetEvent(&fdo->WakeAckEvent, IO_NO_INCREMENT, FALSE);
            return;
        } else if (b == HciPacketEvent) {
            fdo->FrameType = b;
            fdo->FrameHdrLen = 2;
            fdo->FramePos = 0;
            fdo->FrameState = 1;
        } else if (b == HciPacketAclData) {
            fdo->FrameType = b;
            fdo->FrameHdrLen = 4;
            fdo->FramePos = 0;
            fdo->FrameState = 1;
        } else if (b == 0x03) { /* SCO */
            fdo->FrameType = b;
            fdo->FrameHdrLen = 3;
            fdo->FramePos = 0;
            fdo->FrameState = 1;
        }
        return;
    }

    if (fdo->FramePos < MAX_HCI_PKT) {
        fdo->FrameBuf[fdo->FramePos++] = b;
    }

    if (fdo->FrameState == 1) {
        if (fdo->FramePos == fdo->FrameHdrLen) {
            u32 plen = 0;
            if (fdo->FrameType == HciPacketEvent) {
                plen = fdo->FrameBuf[1];
            } else if (fdo->FrameType == HciPacketAclData) {
                plen = (u32)fdo->FrameBuf[2] | ((u32)fdo->FrameBuf[3] << 8);
            } else {
                plen = fdo->FrameBuf[2];
            }
            fdo->FrameExpected = fdo->FrameHdrLen + plen;
            if (fdo->FrameExpected > MAX_HCI_PKT) {
                fdo->FrameState = 0;
                return;
            }
            if (plen == 0) {
                umi_bt_enqueue_rx_packet(fdo, fdo->FrameType, fdo->FrameBuf, fdo->FramePos);
                fdo->FrameState = 0;
            } else {
                fdo->FrameState = 2;
            }
        }
    } else if (fdo->FrameState == 2) {
        if (fdo->FramePos >= fdo->FrameExpected) {
            umi_bt_enqueue_rx_packet(fdo, fdo->FrameType, fdo->FrameBuf, fdo->FrameExpected);
            fdo->FrameState = 0;
        }
    }
}

static void umi_bt_drain_ring_to_framer(PFDO_CONTEXT fdo)
{
    WdfSpinLockAcquire(fdo->RingLock);
    for (;;) {
        u8 b = 0;
        BOOLEAN has = FALSE;
        WdfInterruptAcquireLock(fdo->Interrupt);
        if (fdo->RxTail != fdo->RxHead) {
            b = fdo->RxRing[fdo->RxTail];
            fdo->RxTail = (fdo->RxTail + 1) % RX_RING_SIZE;
            has = TRUE;
        }
        WdfInterruptReleaseLock(fdo->Interrupt);
        if (!has) break;
        umi_bt_feed_h4_byte(fdo, b);
    }
    WdfSpinLockRelease(fdo->RingLock);
    umi_bt_try_complete_read_type(fdo, HciPacketEvent);
    umi_bt_try_complete_read_type(fdo, HciPacketAclData);
}

/* Drain UART6 RX FIFO into RxRing (caller MUST hold Interrupt lock) */
static u32 umi_bt_hw_drain_fifo_locked(PFDO_CONTEXT fdo)
{
    u32 added = 0;
    u32 st = rd32(fdo->Uart, SE_GENI_RX_FIFO_STATUS);
    u32 wc = st & RX_FIFO_WC_MSK;
    BOOLEAN rx_last = (st & RX_LAST) != 0;
    u32 last_valid = (st & RX_LAST_BYTE_VALID_MSK) >> RX_LAST_BYTE_VALID_SHFT;

    for (u32 w = 0; w < wc; w++) {
        u32 word = rd32(fdo->Uart, SE_GENI_RX_FIFOn);
        u32 nb = 4;
        if (rx_last && w == wc - 1 && last_valid > 0 && last_valid < 4) {
            nb = last_valid;
        }
        for (u32 b = 0; b < nb; b++) {
            u8 val = (u8)((word >> (b * 8)) & 0xFFu);
            if (fdo->FirstRxLen < 16) {
                fdo->FirstRxBytes[fdo->FirstRxLen++] = val;
            }
            fdo->RxBytesTotal++;
            u32 next = (fdo->RxHead + 1) % RX_RING_SIZE;
            if (next != fdo->RxTail) {
                fdo->RxRing[fdo->RxHead] = val;
                fdo->RxHead = next;
                added++;
            }
        }
    }
    return added;
}

/* ---- UART6 ISR, DPC & 1ms Fallback Poll Timer ---- */
static BOOLEAN EvtInterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageID)
{
    UNREFERENCED_PARAMETER(MessageID);
    PFDO_CONTEXT fdo = GetFdoContext(WdfInterruptGetDevice(Interrupt));
    if (!fdo->Uart || !fdo->IrqRunning) return FALSE;

    /* Only handle enabled S_IRQ bits; never touch M_IRQ (used by polled TX) */
    u32 s_en  = rd32(fdo->Uart, SE_GENI_S_IRQ_EN);
    u32 s_raw = rd32(fdo->Uart, SE_GENI_S_IRQ_STATUS);
    u32 s_irq = s_raw & s_en;
    if (s_irq == 0) return FALSE;

    fdo->IsrCount++;
    fdo->LastSIrq = s_raw;

    /* Drain FIFO BEFORE clearing level-triggered RX watermark/last IRQ! */
    u32 added = umi_bt_hw_drain_fifo_locked(fdo);
    wr32(fdo->Uart, SE_GENI_S_IRQ_CLEAR, s_irq);

    if (added > 0) {
        WdfInterruptQueueDpcForIsr(Interrupt);
    }
    return TRUE;
}

static VOID EvtInterruptDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);
    PFDO_CONTEXT fdo = GetFdoContext(WdfInterruptGetDevice(Interrupt));
    umi_bt_drain_ring_to_framer(fdo);
}

static VOID EvtRxPollTimer(WDFTIMER Timer)
{
    PFDO_CONTEXT fdo = GetFdoContext(WdfTimerGetParentObject(Timer));
    if (!fdo->Uart || !fdo->IrqRunning) return;

    u32 added = 0;
    WdfInterruptAcquireLock(fdo->Interrupt);
    u32 s_irq = rd32(fdo->Uart, SE_GENI_S_IRQ_STATUS);
    if (s_irq) wr32(fdo->Uart, SE_GENI_S_IRQ_CLEAR, s_irq);
    added = umi_bt_hw_drain_fifo_locked(fdo);
    if (added) fdo->PollDrainCount += added;
    WdfInterruptReleaseLock(fdo->Interrupt);

    if (added > 0) {
        umi_bt_drain_ring_to_framer(fdo);
    }
}

/* ---- BTHX IOCTL Handlers ---- */
static VOID EvtWriteHci(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength,
                        size_t InputBufferLength, ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(IoControlCode);

    PFDO_CONTEXT fdo = GetFdoContext(WdfIoQueueGetDevice(Queue));
    PVOID in_buf = NULL, out_buf = NULL;
    size_t in_sz = 0, out_sz = 0;

    get_bthx_buffers(Request, &in_buf, &in_sz, &out_buf, &out_sz);
    if (!in_buf || in_sz < FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    PBTHX_HCI_READ_WRITE_CONTEXT hci = (PBTHX_HCI_READ_WRITE_CONTEXT)in_buf;
    if (FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + hci->DataLen > in_sz) {
        WdfRequestComplete(Request, STATUS_INVALID_BUFFER_SIZE);
        return;
    }

    if (fdo->TxCount < 15) {
        u16 op = (hci->Type == HciPacketCommand && hci->DataLen >= 2)
                 ? (u16)(hci->Data[0] | ((u16)hci->Data[1] << 8)) : 0;
        bt_log("WRITE_HCI #%u type=0x%x len=%u op=0x%x (isr=%u poll=%u rxb=%u rxp=%u sirq=0x%x)",
               fdo->TxCount, (u32)hci->Type, hci->DataLen, (u32)op,
               fdo->IsrCount, fdo->PollDrainCount, fdo->RxBytesTotal, fdo->RxCount, fdo->LastSIrq);
        if (fdo->FirstRxLen > 0) {
            bt_log("  first_rx(%u): %x %x %x %x %x %x %x %x",
                   fdo->FirstRxLen,
                   fdo->FirstRxBytes[0], fdo->FirstRxBytes[1], fdo->FirstRxBytes[2], fdo->FirstRxBytes[3],
                   fdo->FirstRxBytes[4], fdo->FirstRxBytes[5], fdo->FirstRxBytes[6], fdo->FirstRxBytes[7]);
        }
    }

    /* Send H4 type + payload as a single contiguous buffer */
    u32 total = 1 + hci->DataLen;
    u8 stack_buf[300];
    u8 *tx_buf = (total <= sizeof(stack_buf)) ? stack_buf
                 : (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, total, UMI_BT_TAG);
    if (!tx_buf) {
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    tx_buf[0] = hci->Type;
    if (hci->DataLen > 0) RtlCopyMemory(tx_buf + 1, hci->Data, hci->DataLen);

    KeWaitForSingleObject(&fdo->TxMutex, Executive, KernelMode, FALSE, NULL);
    if (fdo->ChipSleeping) {
        u8 wake = HCI_IBS_WAKE_IND;
        LARGE_INTEGER timeout;
        timeout.QuadPart = -30LL * 10000LL; /* 30ms max wait for 0xFC WAKE_ACK */
        for (u32 tries = 0; tries < 2; tries++) {
            KeClearEvent(&fdo->WakeAckEvent);
            uart_tx_bytes(fdo, &wake, 1);
            if (KeWaitForSingleObject(&fdo->WakeAckEvent, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS) {
                break;
            }
        }
        fdo->ChipSleeping = FALSE;
    }
    BOOLEAN ok = uart_tx_bytes(fdo, tx_buf, total);
    KeReleaseMutex(&fdo->TxMutex, FALSE);

    if (tx_buf != stack_buf) ExFreePoolWithTag(tx_buf, UMI_BT_TAG);
    fdo->TxCount++;

    if (!ok) {
        WdfRequestComplete(Request, STATUS_IO_DEVICE_ERROR);
        return;
    }

    ULONG_PTR info = 0;
    if (out_buf && out_sz >= sizeof(ULONG)) {
        *(PULONG)out_buf = hci->DataLen;
        info = sizeof(ULONG);
    }
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, info);
}

static VOID EvtPdoIoctl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength,
                        size_t InputBufferLength, ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PPDO_CONTEXT pdo = GetPdoContext(WdfIoQueueGetDevice(Queue));
    PFDO_CONTEXT fdo = pdo->Fdo;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    PVOID in_buf = NULL, out_buf = NULL;
    size_t in_sz = 0, out_sz = 0;

    get_bthx_buffers(Request, &in_buf, &in_sz, &out_buf, &out_sz);

    if (fdo->IoctlLogCount < 20) {
        fdo->IoctlLogCount++;
        bt_log("PDO IOCTL 0x%x in=%u out=%u", IoControlCode, (u32)in_sz, (u32)out_sz);
    }

    WDF_REQUEST_FORWARD_OPTIONS fwd;
    WDF_REQUEST_FORWARD_OPTIONS_INIT(&fwd);

    switch (IoControlCode) {
    case IOCTL_BTHX_GET_VERSION:
        if (out_buf && out_sz >= sizeof(BTHX_VERSION)) {
            ((PBTHX_VERSION)out_buf)->Version = BTHX_DDI_VERSION_1;
            info = sizeof(BTHX_VERSION);
        } else {
            st = STATUS_INVALID_PARAMETER;
        }
        WdfRequestCompleteWithInformation(Request, st, info);
        break;

    case IOCTL_BTHX_SET_VERSION:
        if (in_buf && in_sz >= sizeof(BTHX_VERSION)) {
            if (((PBTHX_VERSION)in_buf)->Version != BTHX_DDI_VERSION_1) {
                st = STATUS_NOT_SUPPORTED;
            }
        } else {
            st = STATUS_INVALID_PARAMETER;
        }
        WdfRequestComplete(Request, st);
        break;

    case IOCTL_BTHX_QUERY_CAPABILITIES:
        if (out_buf && out_sz >= sizeof(BTHX_CAPABILITIES)) {
            PBTHX_CAPABILITIES cap = (PBTHX_CAPABILITIES)out_buf;
            RtlZeroMemory(cap, sizeof(*cap));
            /* BthMini.SYS strictly requires ScoSupport==ScoSupportHCIBypass (2) and MaxScoChannels==1 */
            cap->MaxAclTransferInSize = 1025;
            cap->ScoSupport = ScoSupportHCIBypass;
            cap->MaxScoChannels = 1;
            cap->IsDeviceIdleCapable = FALSE;
            cap->IsDeviceWakeCapable = FALSE;
            info = sizeof(BTHX_CAPABILITIES);
        } else {
            st = STATUS_INVALID_PARAMETER;
        }
        WdfRequestCompleteWithInformation(Request, st, info);
        break;

    case IOCTL_BTHX_WRITE_HCI:
        st = WdfRequestForwardToParentDeviceIoQueue(Request, fdo->WriteQueue, &fwd);
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(Request, st);
        }
        break;

    case IOCTL_BTHX_READ_HCI: {
        if (!in_buf || in_sz < sizeof(BTHX_HCI_PACKET_TYPE) ||
            !out_buf || out_sz < sizeof(BTHX_HCI_READ_WRITE_CONTEXT)) {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }
        BTHX_HCI_PACKET_TYPE ptype = *(BTHX_HCI_PACKET_TYPE *)in_buf;
        if (ptype == HciPacketEvent) {
            st = WdfRequestForwardToParentDeviceIoQueue(Request, fdo->ReadEventQueue, &fwd);
            if (!NT_SUCCESS(st)) WdfRequestComplete(Request, st);
            else umi_bt_try_complete_read_type(fdo, HciPacketEvent);
        } else if (ptype == HciPacketAclData) {
            st = WdfRequestForwardToParentDeviceIoQueue(Request, fdo->ReadAclQueue, &fwd);
            if (!NT_SUCCESS(st)) WdfRequestComplete(Request, st);
            else umi_bt_try_complete_read_type(fdo, HciPacketAclData);
        } else {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        }
        break;
    }

    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }
}

/* ---- Static Child PDO Creation (MS_BTHX_BTHMINI) ---- */
static NTSTATUS umi_bt_create_child_pdo(PFDO_CONTEXT fdo)
{
    NTSTATUS st;
    PWDFDEVICE_INIT init = WdfPdoInitAllocate(fdo->Device);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;

    DECLARE_CONST_UNICODE_STRING(dev_id, L"UMI_BT\\QCA6390_H4");
    DECLARE_CONST_UNICODE_STRING(inst_id, L"0");
    DECLARE_CONST_UNICODE_STRING(compat_id, L"MS_BTHX_BTHMINI");
    DECLARE_CONST_UNICODE_STRING(desc, L"Xiaomi Mi 10 QCA6390 Bluetooth Radio");
    DECLARE_CONST_UNICODE_STRING(loc, L"SM8250 QUP0 SE6 UART");

    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_BLUETOOTH);
    st = WdfPdoInitAssignDeviceID(init, &dev_id);
    if (NT_SUCCESS(st)) st = WdfPdoInitAddHardwareID(init, &dev_id);
    if (NT_SUCCESS(st)) st = WdfPdoInitAddCompatibleID(init, &compat_id);
    if (NT_SUCCESS(st)) st = WdfPdoInitAssignInstanceID(init, &inst_id);
    if (NT_SUCCESS(st)) st = WdfPdoInitAddDeviceText(init, &desc, &loc, 0x0409);
    if (NT_SUCCESS(st)) {
        WdfPdoInitSetDefaultLocale(init, 0x0409);
        WdfPdoInitAllowForwardingRequestToParent(init);
    }

    if (!NT_SUCCESS(st)) {
        WdfDeviceInitFree(init);
        return st;
    }

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, PDO_CONTEXT);

    WDFDEVICE pdo = NULL;
    st = WdfDeviceCreate(&init, &attrs, &pdo);
    if (!NT_SUCCESS(st)) return st;

    GetPdoContext(pdo)->Fdo = fdo;

    /* Non-power-managed queue required by BthMini! Handle BOTH DeviceControl & InternalDeviceControl */
    WDF_IO_QUEUE_CONFIG qcfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.PowerManaged = WdfFalse;
    qcfg.EvtIoDeviceControl         = EvtPdoIoctl;
    qcfg.EvtIoInternalDeviceControl = EvtPdoIoctl;
    st = WdfIoQueueCreate(pdo, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(st)) return st;

    WDF_DEVICE_PNP_CAPABILITIES pnp;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnp);
    pnp.Removable = WdfFalse;
    pnp.SurpriseRemovalOK = WdfTrue;
    pnp.UniqueID = WdfTrue;
    WdfDeviceSetPnpCapabilities(pdo, &pnp);

    WDF_DEVICE_POWER_CAPABILITIES pwr;
    WDF_DEVICE_POWER_CAPABILITIES_INIT(&pwr);
    pwr.DeviceD1 = WdfFalse;
    pwr.DeviceD2 = WdfFalse;
    pwr.WakeFromD0 = WdfFalse;
    pwr.WakeFromD1 = WdfFalse;
    pwr.WakeFromD2 = WdfFalse;
    pwr.WakeFromD3 = WdfFalse;
    pwr.DeviceWake = PowerDeviceUnspecified;
    pwr.DeviceState[PowerSystemWorking]   = PowerDeviceD0;
    pwr.DeviceState[PowerSystemSleeping1] = PowerDeviceD0;
    pwr.DeviceState[PowerSystemSleeping2] = PowerDeviceD0;
    pwr.DeviceState[PowerSystemSleeping3] = PowerDeviceD0;
    pwr.DeviceState[PowerSystemHibernate] = PowerDeviceD3;
    pwr.DeviceState[PowerSystemShutdown]  = PowerDeviceD3;
    WdfDeviceSetPowerCapabilities(pdo, &pwr);

    st = WdfFdoAddStaticChild(fdo->Device, pdo);
    if (NT_SUCCESS(st)) {
        fdo->ChildPdo = pdo;
        bt_log("Child PDO MS_BTHX_BTHMINI created successfully!");
    }
    return st;
}

/* ---- FDO PnP / Power Callbacks ---- */
static NTSTATUS EvtDevicePrepareHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw, WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesRaw);
    PFDO_CONTEXT fdo = GetFdoContext(Device);
    PHYSICAL_ADDRESS pa;

    bt_log_init();
    bt_log("=== umi-bt.sys v1.0.0.7 PrepareHardware ===");

    pa.QuadPart = (LONGLONG)GCC_PHYS_BASE;
    fdo->Gcc = (volatile u8 *)MmMapIoSpace(pa, GCC_SIZE, MmNonCached);
    pa.QuadPart = (LONGLONG)TLMM_NORTH_PHYS_BASE;
    fdo->Tlmm = (volatile u8 *)MmMapIoSpace(pa, TLMM_NORTH_SIZE, MmNonCached);

    /* Find UART6 MMIO from translated resources, or fallback to fixed base */
    ULONG count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (ULONG i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc->Type == CmResourceTypeMemory && !fdo->Uart) {
            fdo->Uart = (volatile u8 *)MmMapIoSpace(desc->u.Memory.Start, desc->u.Memory.Length, MmNonCached);
            fdo->UartMappedFromCrs = TRUE;
        }
    }
    if (!fdo->Uart) {
        pa.QuadPart = (LONGLONG)UART6_PHYS_BASE;
        fdo->Uart = (volatile u8 *)MmMapIoSpace(pa, UART6_SIZE, MmNonCached);
    }

    if (!fdo->Gcc || !fdo->Tlmm || !fdo->Uart) {
        bt_log("ERROR: MmMapIoSpace failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!umi_bt_hw_and_qca_init(fdo)) {
        bt_log("ERROR: umi_bt_hw_and_qca_init failed");
        return STATUS_DEVICE_NOT_READY;
    }

    if (!fdo->ChildPdo) {
        NTSTATUS st = umi_bt_create_child_pdo(fdo);
        if (!NT_SUCCESS(st)) {
            bt_log("ERROR: create_child_pdo failed st=0x%x", (u32)st);
            return st;
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    PFDO_CONTEXT fdo = GetFdoContext(Device);
    if (fdo->Uart && fdo->HwReady) {
        fdo->RxHead = fdo->RxTail = 0;
        fdo->FrameState = 0;
        fdo->ChipSleeping = TRUE; /* Re-sync Qualcomm IBS wake on first TX after D0Entry */
        fdo->IrqRunning = TRUE;
        wr32(fdo->Uart, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
        wr32(fdo->Uart, SE_GENI_S_IRQ_EN, S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN);
        uart_start_rx(fdo);
        WdfTimerStart(fdo->RxPollTimer, WDF_REL_TIMEOUT_IN_MS(1));
        bt_log("D0Entry: UART6 RX IRQ + 1ms poll timer enabled (prev=%u)", (u32)PreviousState);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    PFDO_CONTEXT fdo = GetFdoContext(Device);
    fdo->IrqRunning = FALSE;
    if (fdo->RxPollTimer) {
        WdfTimerStop(fdo->RxPollTimer, TRUE);
    }
    if (fdo->Uart) {
        wr32(fdo->Uart, SE_GENI_S_IRQ_EN, 0);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PFDO_CONTEXT fdo = GetFdoContext(Device);
    fdo->IrqRunning = FALSE;
    if (fdo->Uart) {
        uart_stop_rx(fdo);
        MmUnmapIoSpace((PVOID)fdo->Uart, UART6_SIZE);
        fdo->Uart = NULL;
    }
    if (fdo->Tlmm) {
        MmUnmapIoSpace((PVOID)fdo->Tlmm, TLMM_NORTH_SIZE);
        fdo->Tlmm = NULL;
    }
    if (fdo->Gcc) {
        MmUnmapIoSpace((PVOID)fdo->Gcc, GCC_SIZE);
        fdo->Gcc = NULL;
    }
    bt_log("ReleaseHardware complete (tx=%u rx=%u)", fdo->TxCount, fdo->RxCount);
    bt_log_close();
    return STATUS_SUCCESS;
}

static NTSTATUS EvtDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);
    NTSTATUS st;

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    pnp.EvtDeviceD0Entry         = EvtDeviceD0Entry;
    pnp.EvtDeviceD0Exit          = EvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, FDO_CONTEXT);

    WDFDEVICE device = NULL;
    st = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(st)) return st;

    PFDO_CONTEXT fdo = GetFdoContext(device);
    fdo->Device = device;
    InitializeListHead(&fdo->RxEventList);
    InitializeListHead(&fdo->RxAclList);
    KeInitializeMutex(&fdo->TxMutex, 0);
    KeInitializeEvent(&fdo->WakeAckEvent, NotificationEvent, FALSE);

    st = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &fdo->RingLock);
    if (!NT_SUCCESS(st)) return st;
    st = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &fdo->PktLock);
    if (!NT_SUCCESS(st)) return st;

    /* Separate manual queues for pending IOCTL_BTHX_READ_HCI (Event vs ACL) */
    WDF_IO_QUEUE_CONFIG rcfg;
    WDF_IO_QUEUE_CONFIG_INIT(&rcfg, WdfIoQueueDispatchManual);
    rcfg.PowerManaged = WdfFalse;
    st = WdfIoQueueCreate(device, &rcfg, WDF_NO_OBJECT_ATTRIBUTES, &fdo->ReadEventQueue);
    if (!NT_SUCCESS(st)) return st;
    st = WdfIoQueueCreate(device, &rcfg, WDF_NO_OBJECT_ATTRIBUTES, &fdo->ReadAclQueue);
    if (!NT_SUCCESS(st)) return st;

    /* Sequential passive-level queue for IOCTL_BTHX_WRITE_HCI */
    WDF_IO_QUEUE_CONFIG wcfg;
    WDF_IO_QUEUE_CONFIG_INIT(&wcfg, WdfIoQueueDispatchSequential);
    wcfg.PowerManaged = WdfFalse;
    wcfg.EvtIoDeviceControl         = EvtWriteHci;
    wcfg.EvtIoInternalDeviceControl = EvtWriteHci;
    WDF_OBJECT_ATTRIBUTES wattrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&wattrs);
    wattrs.ExecutionLevel = WdfExecutionLevelPassive;
    st = WdfIoQueueCreate(device, &wcfg, &wattrs, &fdo->WriteQueue);
    if (!NT_SUCCESS(st)) return st;

    /* Connect UART6 interrupt */
    WDF_INTERRUPT_CONFIG icfg;
    WDF_INTERRUPT_CONFIG_INIT(&icfg, EvtInterruptIsr, EvtInterruptDpc);
    st = WdfInterruptCreate(device, &icfg, WDF_NO_OBJECT_ATTRIBUTES, &fdo->Interrupt);
    if (!NT_SUCCESS(st)) return st;

    /* 1ms periodic RX poll fallback timer */
    WDF_TIMER_CONFIG tcfg;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&tcfg, EvtRxPollTimer, 1);
    WDF_OBJECT_ATTRIBUTES tattrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&tattrs);
    tattrs.ParentObject = device;
    st = WdfTimerCreate(&tcfg, &tattrs, &fdo->RxPollTimer);
    if (!NT_SUCCESS(st)) return st;

    /* Work item for replying 0xFC (IBS WAKE_ACK) at PASSIVE_LEVEL */
    WDF_WORKITEM_CONFIG wicfg;
    WDF_WORKITEM_CONFIG_INIT(&wicfg, EvtIbsAckWorkItem);
    WDF_OBJECT_ATTRIBUTES wiattrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&wiattrs);
    wiattrs.ParentObject = device;
    st = WdfWorkItemCreate(&wicfg, &wiattrs, &fdo->IbsAckWorkItem);
    if (!NT_SUCCESS(st)) return st;

    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDriverDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
