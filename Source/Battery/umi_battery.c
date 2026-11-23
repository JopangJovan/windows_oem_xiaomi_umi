/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Xiaomi Mi 10 (UMI / SM8250) Native Windows 11 KMDF Battery Miniclass Driver
 *
 * Reads PM8150B Gen4 Fuel Gauge (SPMI slave 2, 0x4000/0x4100/0x4200) and
 * manages PM8150B SMB5 Charger replug/APSD/ICL (SPMI slave 2, 0x1000/0x1300/0x1500/0x1600)
 * via the SM8250 SPMI PMIC Arbiter v5. JEITA & voltage safety remain hardware-owned.
 */
#define INITGUID
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <batclass.h>

/* SM8250 SPMI PMIC Arbiter (v5) physical ranges */
#define SPMI_CORE_PHYS      0x0C440000ULL
#define SPMI_CORE_SIZE      0x2000UL
#define SPMI_OWNERS_PHYS    0x0C40A000ULL
#define SPMI_OWNERS_SIZE    0x1000UL
#define SPMI_OBS_PHYS       0x0E600000ULL
#define SPMI_OBS_SIZE       0x10000UL
#define SPMI_CHNLS_PHYS     0x0C600000ULL
#define SPMI_NO_APID        0xFFFFu

/* PM8150B (SPMI Slave ID 2) PPIDs = (sid << 8) | (periph >> 8) */
#define PPID_PM8150B_CHG    0x210u  /* 0x1000: SMB5 CHGR */
#define PPID_PM8150B_DCDC   0x211u  /* 0x1100: SMB5 DCDC / OTG */
#define PPID_PM8150B_USBIN  0x213u  /* 0x1300: SMB5 USBIN */
#define PPID_PM8150B_TYPEC  0x215u  /* 0x1500: SMB5 TYPEC */
#define PPID_PM8150B_MISC   0x216u  /* 0x1600: SMB5 MISC (WDOG/AICL) */
#define PPID_PM8150B_PDPHY  0x217u  /* 0x1700: SMB5 USB_PDPHY */
#define PPID_PM8150B_GPIO12 0x2CBu  /* 0xCB00: PM8150B GPIO12 (umi ext_5v_boost / otg_vbus_boost) */
#define PPID_PM8150B_FG_SOC 0x240u  /* 0x4000: FG SOC */
#define PPID_PM8150B_FG_BAT 0x241u  /* 0x4100: FG BATT_INFO */
#define PPID_PM8150B_FG_ADC 0x242u  /* 0x4200: FG RR_ADC */

/* FG register offsets */
#define REG_FG_MONOTONIC_SOC    0x09u /* 2 bytes */
#define REG_FG_BATT_VOLTAGE     0xA0u /* 2 bytes, 122.070 uV/LSB */
#define REG_FG_BATT_CURRENT     0xA2u /* 2 bytes signed, 488.281 uA/LSB */
#define REG_FG_ADC_BATT_TEMP    0x88u /* 2 bytes */

/* SMB5 CHGR (0x1000) offsets */
#define REG_CHG_STATUS_1        0x06u /* bits[2:0] charger_status */
#define REG_CHG_STATUS_2        0x07u /* bit 5 = VBATT_GTET_INHIBIT */
#define REG_CHG_ENABLE_CMD      0x42u /* bit 0 = CHARGING_ENABLE_CMD_BIT */

/* SMB5 DCDC / OTG (0x1100) offsets */
#define REG_DCDC_POWER_PATH     0x0Bu /* POWER_PATH_STATUS */
#define REG_DCDC_CMD_OTG        0x40u /* bit 0 = OTG_EN */
#define REG_DCDC_OTG_ILIM_CFG   0x52u /* bits[2:0]: 0=500mA, 1=1000mA, 2=1500mA */
#define REG_DCDC_OTG_CFG        0x53u /* bit 1 = OTG_EN_SRC_CFG_BIT (0=SW CMD_OTG, 1=HW) */

/* SMB5 USBIN (0x1300) offsets */
#define REG_USBIN_APSD_STATUS   0x07u /* bit 0 = APSD_DTC_STATUS_DONE */
#define REG_USBIN_APSD_RESULT   0x08u /* bits[6:0] charger type */
#define REG_USBIN_INT_RT_STS    0x10u /* bit 4 = USBIN_PLUGIN_RT_STS */
#define REG_USBIN_CMD_IL        0x40u /* bit 0 = USBIN_SUSPEND_BIT */
#define REG_USBIN_CMD_APSD      0x41u /* bit 0 = APSD_RERUN_BIT */
#define REG_USBIN_CMD_ICL_OVR   0x42u /* bit 0 = ICL_OVERRIDE_BIT */
#define REG_USBIN_TYPEC_CFG     0x58u /* bit 7 = BC1P2_START_ON_CC */
#define REG_USBIN_ADAPTER_ALLOW 0x60u /* 0=5V, 3=5V_OR_9V, 4=5V_9V_12V */
#define REG_USBIN_OPT1_CFG      0x62u /* bit 3 = BC1P2_SRC_DETECT_BIT */
#define REG_USBIN_LOAD_CFG      0x65u /* bit 4 = ICL_OVERRIDE_AFTER_APSD */
#define REG_USBIN_ICL_OPT       0x66u /* bit 0 = USBIN_MODE_CHG_BIT */
#define REG_USBIN_ICL_CFG       0x70u /* 50 mA / LSB */
#define REG_USBIN_AICL_OPT      0x80u /* bits 7,4,3,2 */

/* SMB5 TYPEC (0x1500) offsets */
#define REG_TYPEC_SNK_STATUS    0x06u /* TYPE_C_SNK_STATUS */
#define REG_TYPEC_SRC_STATUS    0x08u /* TYPE_C_SRC_STATUS (bit3=SRC_RD_OPEN, bit2=SRC_RD_RA_VCONN) */
#define REG_TYPEC_MISC_STATUS   0x0Bu /* bit6=SNK_SRC_MODE (1=SRC), bit1=CC_ORIENTATION, bit0=CC_ATTACHED */
#define REG_TYPEC_MODE_CFG      0x44u /* bit 4=EN_TRY_SNK, bit 2=EN_SRC_ONLY, bit 1=EN_SNK_ONLY, bit 0=DISABLE */
#define REG_TYPEC_VCONN_CTRL    0x46u /* bit2=ORIENTATION, bit1=EN_VALUE, bit0=EN_SRC (SW) */

/* SMB5 MISC (0x1600) offsets */
#define REG_MISC_WD_CFG         0x51u /* bits 7,6,1 */
#define REG_MISC_SNARL_WD_CFG   0x53u /* 0 = disable */
#define REG_MISC_AICL_RERUN     0x61u /* bits[1:0] */

/* PM8150B USB_PDPHY (0x1700) offsets */
#define REG_PDPHY_INT_RT_STS      0x10u
#define REG_PDPHY_INT_LATCHED_CLR 0x14u
#define REG_PDPHY_INT_EN_SET      0x15u
#define REG_PDPHY_INT_LATCHED_STS 0x18u
#define REG_PDPHY_MSG_CONFIG      0x40u /* bits[1:0]=rev (1=PD2.0), bit2=PR, bit3=DR */
#define REG_PDPHY_TX_SIZE         0x42u /* (hdr+data)-1 */
#define REG_PDPHY_TX_CONTROL      0x44u /* [6:5]=retries, [4:2]=frame, bit1=sig, bit0=send */
#define REG_PDPHY_EN_CONTROL      0x46u /* bit0=enable */
#define REG_PDPHY_RX_SIZE         0x48u /* real_size = reg + 1 */
#define REG_PDPHY_RX_STATUS       0x4Au /* [2:0]=frame type (0=SOP) */
#define REG_PDPHY_RX_ACK          0x4Bu /* bit0=RX_BUFFER_TOKEN */
#define REG_PDPHY_FRAME_FILTER    0x4Cu /* bit5=HARD_RESET, bit0=SOP */
#define REG_PDPHY_TX_BUFFER_HDR   0x60u /* 2 bytes */
#define REG_PDPHY_TX_BUFFER_DATA  0x62u /* up to 28 bytes */
#define REG_PDPHY_RX_BUFFER       0x80u /* up to 30 bytes */

#define PDPHY_IRQ_SIG_RX          (1u << 1)
#define PDPHY_IRQ_MSG_TX          (1u << 2)
#define PDPHY_IRQ_MSG_RX          (1u << 3)
#define PDPHY_IRQ_MSG_TX_FAIL     (1u << 4)
#define PDPHY_IRQ_MSG_TX_DISCARD  (1u << 5)

/* Xiaomi Mi 10 (BM4N) battery design specs */
#define UMI_DESIGN_MWH          18300u
#define UMI_ALERT_CRIT_MWH      915u   /* 5% */
#define UMI_ALERT_LOW_MWH       1830u  /* 10% */
#define UMI_POLL_MS             1000u

typedef struct _UMI_BATT_SAMPLE {
    BOOLEAN Valid;
    ULONG   SocPercent;
    ULONG   VoltageMv;
    LONG    CurrentMa;
    LONG    RateMw;
    ULONG   TempTenthK;
    ULONG   PowerState;
    ULONG   CapacityMwh;
    BOOLEAN UsbOnline;
    UCHAR   ChgStat1;
    UCHAR   ChgStat2;
    UCHAR   CapRaw0;
    UCHAR   CapRaw1;
    USHORT  RawTemp;
    ULONGLONG Timestamp100ns;
} UMI_BATT_SAMPLE;

typedef struct _BATT_CTX {
    WDFDEVICE       Device;
    WDFTIMER        Timer;
    PVOID           ClassData;
    volatile LONG   Running;

    PUCHAR          SpmiCore;
    PUCHAR          SpmiOwners;
    PUCHAR          SpmiObs;
    PUCHAR          WrChg;
    PUCHAR          WrDcdc;
    PUCHAR          WrUsbin;
    PUCHAR          WrTypec;
    PUCHAR          WrMisc;
    PUCHAR          WrPdphy;
    PUCHAR          WrGpio12;

    USHORT          ApidFgSoc;
    USHORT          ApidFgBat;
    USHORT          ApidFgAdc;
    USHORT          ApidChg;
    USHORT          ApidDcdc;
    USHORT          ApidUsbin;
    USHORT          ApidTypec;
    USHORT          ApidMisc;
    USHORT          ApidPdphy;
    USHORT          ApidGpio12;

    KMUTEX          Lock;
    UMI_BATT_SAMPLE Sample;
    BOOLEAN         PrevUsbOnline;
    BOOLEAN         PrevOtgOn;
    BOOLEAN         ApsdConfigured;
    BOOLEAN         PdAttempted;
    BOOLEAN         PdActive;
    ULONG           PdVoltageMv;
    ULONG           PdCurrentMa;
    UCHAR           PdMsgId;

    BOOLEAN         NotifyEnabled;
    BATTERY_NOTIFY  Notify;
    ULONG           Ticks;
} BATT_CTX, *PBATT_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BATT_CTX, GetBattCtx);

/* ---- Simple Passive-Level File Logger ---- */
static KMUTEX g_log_lock;
static BOOLEAN g_log_ready = FALSE;

static void BattLogInit(void)
{
    KeInitializeMutex(&g_log_lock, 0);
    UNICODE_STRING path;
    RtlInitUnicodeString(&path, L"\\SystemRoot\\Temp\\umi-battery.log");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE fh = NULL;
    IO_STATUS_BLOCK iosb;
    if (NT_SUCCESS(ZwCreateFile(&fh, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                                FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
                                NULL, 0))) {
        ZwClose(fh);
        g_log_ready = TRUE;
    }
}

static void BattLog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    NTSTATUS st = RtlStringCbVPrintfA(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!NT_SUCCESS(st)) return;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "%s", buf);
    if (g_log_ready && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        SIZE_T len = 0;
        while (buf[len] && len < sizeof(buf)) len++;
        KeWaitForSingleObject(&g_log_lock, Executive, KernelMode, FALSE, NULL);
        UNICODE_STRING path;
        RtlInitUnicodeString(&path, L"\\SystemRoot\\Temp\\umi-battery.log");
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        HANDLE fh = NULL;
        IO_STATUS_BLOCK iosb;
        if (NT_SUCCESS(ZwCreateFile(&fh, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                                    FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
                                    NULL, 0))) {
            ZwWriteFile(fh, NULL, NULL, NULL, &iosb, buf, (ULONG)len, NULL, NULL);
            ZwClose(fh);
        }
        KeReleaseMutex(&g_log_lock, FALSE);
    }
}

/* ---- SPMI PMIC Arbiter v5 Observer (Atomic Burst Read) + Channel Write ---- */
static __inline ULONG SpmiReg(PUCHAR base, ULONG off)
{
    return READ_REGISTER_ULONG((PULONG)(base + off));
}

static USHORT SpmiFindApid(PBATT_CTX c, USHORT ppid, ULONG channels, ULONG *out_owner)
{
    USHORT found = SPMI_NO_APID;
    ULONG best_owner = 0xFF;
    for (ULONG i = 0; i < channels; i++) {
        ULONG m = SpmiReg(c->SpmiCore, 0x900 + 4 * i);
        if (m && ((m >> 8) & 0xFFFu) == ppid) {
            ULONG owner = SpmiReg(c->SpmiOwners, 0x700 + 4 * i) & 7u;
            if (found == SPMI_NO_APID || owner == 0) {
                found = (USHORT)i;
                best_owner = owner;
            }
        }
    }
    if (out_owner) *out_owner = best_owner;
    return found;
}

/* Atomic 1..4 byte burst read through observer channel */
static NTSTATUS SpmiReadBurst(PBATT_CTX c, USHORT apid, UCHAR reg, UCHAR *out, ULONG count)
{
    if (apid == SPMI_NO_APID || apid >= 512 || !c->SpmiObs || count == 0 || count > 4)
        return STATUS_DEVICE_NOT_READY;
    PUCHAR ch = c->SpmiObs + 0x80u * apid;
    for (int tries = 0; tries < 3; tries++) {
        ULONGLONG limit = KeQueryInterruptTime() + 20000ULL;
        WRITE_REGISTER_ULONG((PULONG)ch, (1u << 27) | ((ULONG)reg << 4) | ((count - 1u) & 7u));
        BOOLEAN done = FALSE;
        ULONG st = 0;
        do {
            st = SpmiReg(ch, 8);
            if (st & 1u) { done = TRUE; break; }
            KeStallExecutionProcessor(1);
        } while (KeQueryInterruptTime() < limit);
        if (!done) return STATUS_IO_TIMEOUT;
        if (st & 8u) continue; /* STATUS_DROPPED: retry */
        if (st & 6u) return STATUS_IO_DEVICE_ERROR;
        ULONG d0 = SpmiReg(ch, 0x18);
        for (ULONG i = 0; i < count; i++)
            out[i] = (UCHAR)((d0 >> (8u * i)) & 0xFFu);
        return STATUS_SUCCESS;
    }
    return STATUS_IO_DEVICE_ERROR;
}

static NTSTATUS SpmiReadByte(PBATT_CTX c, USHORT apid, UCHAR reg, UCHAR *out)
{
    return SpmiReadBurst(c, apid, reg, out, 1);
}

static NTSTATUS SpmiReadBytes(PBATT_CTX c, USHORT apid, UCHAR reg, UCHAR *out, ULONG len)
{
    ULONG off = 0;
    while (off < len) {
        ULONG chunk = (len - off > 4u) ? 4u : (len - off);
        NTSTATUS st = SpmiReadBurst(c, apid, (UCHAR)(reg + off), out + off, chunk);
        if (!NT_SUCCESS(st)) return st;
        off += chunk;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SpmiWriteByte(PUCHAR wr_ch, UCHAR reg, UCHAR val)
{
    if (!wr_ch) return STATUS_DEVICE_NOT_READY;
    for (int tries = 0; tries < 3; tries++) {
        ULONGLONG limit = KeQueryInterruptTime() + 20000ULL;
        WRITE_REGISTER_ULONG((PULONG)(wr_ch + 0x10), (ULONG)val);
        WRITE_REGISTER_ULONG((PULONG)(wr_ch + 0x00), (0u << 27) | ((ULONG)reg << 4));
        BOOLEAN done = FALSE;
        ULONG st = 0;
        do {
            st = SpmiReg(wr_ch, 8);
            if (st & 1u) { done = TRUE; break; }
            KeStallExecutionProcessor(1);
        } while (KeQueryInterruptTime() < limit);
        if (!done) return STATUS_IO_TIMEOUT;
        if (st & 8u) continue; /* DROPPED: retry */
        if (st & 6u) return STATUS_IO_DEVICE_ERROR;
        return STATUS_SUCCESS;
    }
    return STATUS_IO_DEVICE_ERROR;
}

static NTSTATUS SpmiUpdateBits(PBATT_CTX c, USHORT apid, PUCHAR wr_ch, UCHAR reg, UCHAR mask, UCHAR val)
{
    UCHAR cur = 0;
    NTSTATUS st = SpmiReadByte(c, apid, reg, &cur);
    if (!NT_SUCCESS(st)) return st;
    UCHAR next = (UCHAR)((cur & ~mask) | (val & mask));
    if (next == cur) return STATUS_SUCCESS;
    return SpmiWriteByte(wr_ch, reg, next);
}

static PUCHAR MapWriteChannel(USHORT apid, ULONG owner)
{
    if (apid == SPMI_NO_APID || owner != 0) return NULL;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)(SPMI_CHNLS_PHYS + 0x10000ULL * apid);
    return (PUCHAR)MmMapIoSpace(pa, 0x1000, MmNonCached);
}

static void SpmiUnmap(PBATT_CTX c)
{
    if (c->WrChg)      { MmUnmapIoSpace(c->WrChg, 0x1000); c->WrChg = NULL; }
    if (c->WrDcdc)     { MmUnmapIoSpace(c->WrDcdc, 0x1000); c->WrDcdc = NULL; }
    if (c->WrUsbin)    { MmUnmapIoSpace(c->WrUsbin, 0x1000); c->WrUsbin = NULL; }
    if (c->WrTypec)    { MmUnmapIoSpace(c->WrTypec, 0x1000); c->WrTypec = NULL; }
    if (c->WrMisc)     { MmUnmapIoSpace(c->WrMisc, 0x1000); c->WrMisc = NULL; }
    if (c->WrPdphy)    { MmUnmapIoSpace(c->WrPdphy, 0x1000); c->WrPdphy = NULL; }
    if (c->WrGpio12)   { MmUnmapIoSpace(c->WrGpio12, 0x1000); c->WrGpio12 = NULL; }
    if (c->SpmiCore)   { MmUnmapIoSpace(c->SpmiCore, SPMI_CORE_SIZE); c->SpmiCore = NULL; }
    if (c->SpmiOwners) { MmUnmapIoSpace(c->SpmiOwners, SPMI_OWNERS_SIZE); c->SpmiOwners = NULL; }
    if (c->SpmiObs)    { MmUnmapIoSpace(c->SpmiObs, SPMI_OBS_SIZE); c->SpmiObs = NULL; }
}

/* Safe subset of qcom_pm8150b_charger.c smb5_init_seq: Sink-only Type-C, disable OTG & wdog, enable AICL */
static void Smb5InitHw(PBATT_CTX c)
{
    if (c->WrDcdc) {
        SpmiUpdateBits(c, c->ApidDcdc, c->WrDcdc, REG_DCDC_OTG_ILIM_CFG, 0x07, 0x02);   /* 1500mA OTG VBUS limit */
        SpmiUpdateBits(c, c->ApidDcdc, c->WrDcdc, REG_DCDC_OTG_CFG, 0x02, 0x00);         /* SW CMD_OTG control (matches Linux qcom_usb_vbus) */
        SpmiUpdateBits(c, c->ApidDcdc, c->WrDcdc, REG_DCDC_CMD_OTG, 0x01, 0x00);         /* start with OTG off until Rd detected */
    }
    if (c->WrUsbin) {
        SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_CMD_IL, 0x01, 0x00);       /* clear USBIN_SUSPEND */
        SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_TYPEC_CFG, 0x80, 0x00);    /* clear BC1P2_START_ON_CC */
        SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_OPT1_CFG, 0x08, 0x08);     /* BC1P2_SRC_DETECT */
        SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_ICL_OPT, 0x01, 0x01);      /* USBIN_MODE_CHG */
        SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_CMD_ICL_OVR, 0x01, 0x00);  /* clear ICL_OVERRIDE */
        SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_LOAD_CFG, 0x10, 0x00);     /* clear ICL_OVR_AFTER_APSD */
        SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_AICL_OPT, 0x9C, 0x9C);     /* AICL_EN + periodic */
    }
    if (c->WrMisc) {
        SpmiUpdateBits(c, c->ApidMisc, c->WrMisc, REG_MISC_AICL_RERUN, 0x03, 0x00);
        SpmiWriteByte(c->WrMisc, REG_MISC_SNARL_WD_CFG, 0x00);                           /* disable wdog */
        SpmiUpdateBits(c, c->ApidMisc, c->WrMisc, REG_MISC_WD_CFG, 0xC2, 0x00);
    }
    if (c->WrTypec) {
        /* Enable Dual-Role Port (Try.SNK): Sink for chargers/PCs, Source+5V OTG boost for USB devices */
        SpmiUpdateBits(c, c->ApidTypec, c->WrTypec, REG_TYPEC_MODE_CFG, 0x1F, 0x10);
    }
    if (c->WrChg) {
        SpmiUpdateBits(c, c->ApidChg, c->WrChg, REG_CHG_ENABLE_CMD, 0x01, 0x01);         /* CHARGING_ENABLE */
    }
    UCHAR tc_snk = 0, tc_mode = 0, ppath = 0, usbin_il = 0;
    SpmiReadByte(c, c->ApidTypec, REG_TYPEC_SNK_STATUS, &tc_snk);
    SpmiReadByte(c, c->ApidTypec, REG_TYPEC_MODE_CFG, &tc_mode);
    SpmiReadByte(c, c->ApidDcdc, REG_DCDC_POWER_PATH, &ppath);
    SpmiReadByte(c, c->ApidUsbin, REG_USBIN_CMD_IL, &usbin_il);
    BattLog("smb5: init done tc_mode=0x%02x tc_snk=0x%02x ppath=0x%02x il=0x%02x\n",
            tc_mode, tc_snk, ppath, usbin_il);
}

/* Called when USB is plugged in to ensure APSD + ICL are configured after replug */
static void Smb5ConfigurePluggedInLocked(PBATT_CTX c)
{
    if (!c->WrUsbin) return;
    /* Always ensure USBIN is not suspended and charging is enabled */
    SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_CMD_IL, 0x01, 0x00);
    if (c->WrChg)
        SpmiUpdateBits(c, c->ApidChg, c->WrChg, REG_CHG_ENABLE_CMD, 0x01, 0x01);

    UCHAR apsd_st = 0, apsd_res = 0;
    if (!NT_SUCCESS(SpmiReadByte(c, c->ApidUsbin, REG_USBIN_APSD_STATUS, &apsd_st)))
        return;
    if (!(apsd_st & 0x01u)) {
        /* APSD not done yet: trigger APSD rerun and wait for next poll */
        SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_CMD_APSD, 0x01, 0x01);
        BattLog("smb5: apsd not done (0x%02x), triggered APSD rerun\n", apsd_st);
        return;
    }
    if (!NT_SUCCESS(SpmiReadByte(c, c->ApidUsbin, REG_USBIN_APSD_RESULT, &apsd_res)))
        return;

    UCHAR r = apsd_res & 0x7Fu;
    UCHAR icl_raw = 10; /* 500 mA default (SDP) */
    if (r & (0x40u | 0x20u | 0x10u | 0x08u | 0x02u))      /* QC3/QC2/FLOAT/DCP/OCP */
        icl_raw = 60; /* 3000 mA (AICL will safely throttle to adapter limit) */
    else if (r & 0x04u)                                    /* CDP */
        icl_raw = 30; /* 1500 mA */

    SpmiWriteByte(c->WrUsbin, REG_USBIN_ADAPTER_ALLOW, 0x03); /* Allow 5V or 9V */
    if (!c->PdActive)
        SpmiWriteByte(c->WrUsbin, REG_USBIN_ICL_CFG, icl_raw);
    c->ApsdConfigured = TRUE;
    BattLog("smb5: apsd=0x%02x res=0x%02x -> set ICL=%umA (pd=%u)\n",
            apsd_st, apsd_res, (ULONG)icl_raw * 50u, c->PdActive ? 1u : 0u);
}

/* ---- PM8150B USB-PD 2.0/3.0 Sink Negotiation (up to 9V @ 3A = 27W) ---- */
static void PdSleepMs(ULONG ms)
{
    LARGE_INTEGER li;
    li.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &li);
}

static void PdClearTxControl(PBATT_CTX c)
{
    UCHAR dummy = 0;
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_TX_CONTROL, 0x00);
    SpmiReadByte(c, c->ApidPdphy, REG_PDPHY_TX_CONTROL, &dummy);
}

static BOOLEAN PdReceiveMsg(PBATT_CTX c, USHORT *out_hdr, ULONG *out_objs, ULONG *out_nobj)
{
    UCHAR latched = 0, rx_ack = 0;
    SpmiReadByte(c, c->ApidPdphy, REG_PDPHY_INT_LATCHED_STS, &latched);
    SpmiReadByte(c, c->ApidPdphy, REG_PDPHY_RX_ACK, &rx_ack);
    if (!(latched & PDPHY_IRQ_MSG_RX) && !(rx_ack & 0x01u))
        return FALSE;

    UCHAR rx_size = 0, rx_status = 0;
    SpmiReadByte(c, c->ApidPdphy, REG_PDPHY_RX_SIZE, &rx_size);
    SpmiReadByte(c, c->ApidPdphy, REG_PDPHY_RX_STATUS, &rx_status);

    ULONG size = (ULONG)rx_size + 1u;
    UCHAR buf[32];
    RtlZeroMemory(buf, sizeof(buf));
    BOOLEAN ok = FALSE;
    if (size >= 2u && size <= 30u) {
        if (NT_SUCCESS(SpmiReadBytes(c, c->ApidPdphy, REG_PDPHY_RX_BUFFER, buf, size)))
            ok = TRUE;
    }

    /* Return RX buffer ownership to hardware immediately */
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_RX_ACK, 0x00);
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_INT_LATCHED_CLR, PDPHY_IRQ_MSG_RX);

    if (!ok || (rx_status & 0x07u) != 0u) /* only SOP */
        return FALSE;

    USHORT hdr = (USHORT)((ULONG)buf[0] | ((ULONG)buf[1] << 8));
    ULONG nobj = ((ULONG)hdr >> 12) & 0x07u;
    if (2u + nobj * 4u > size)
        nobj = (size - 2u) / 4u;
    for (ULONG i = 0; i < nobj && i < 7u; i++) {
        ULONG o = 2u + i * 4u;
        out_objs[i] = (ULONG)buf[o] | ((ULONG)buf[o + 1] << 8) |
                      ((ULONG)buf[o + 2] << 16) | ((ULONG)buf[o + 3] << 24);
    }
    *out_hdr = hdr;
    *out_nobj = nobj;
    return TRUE;
}

static BOOLEAN PdTransmitMsg(PBATT_CTX c, USHORT hdr, const ULONG *objs, ULONG nobj)
{
    UCHAR rx_ack = 0;
    SpmiReadByte(c, c->ApidPdphy, REG_PDPHY_RX_ACK, &rx_ack);
    if (rx_ack & 0x01u)
        return FALSE; /* RX pending */

    PdClearTxControl(c);
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_TX_BUFFER_HDR,     (UCHAR)(hdr & 0xFFu));
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_TX_BUFFER_HDR + 1, (UCHAR)((hdr >> 8) & 0xFFu));
    for (ULONG i = 0; i < nobj && i < 7u; i++) {
        ULONG v = objs[i];
        UCHAR base = (UCHAR)(REG_PDPHY_TX_BUFFER_DATA + i * 4u);
        SpmiWriteByte(c->WrPdphy, base + 0, (UCHAR)(v & 0xFFu));
        SpmiWriteByte(c->WrPdphy, base + 1, (UCHAR)((v >> 8) & 0xFFu));
        SpmiWriteByte(c->WrPdphy, base + 2, (UCHAR)((v >> 16) & 0xFFu));
        SpmiWriteByte(c->WrPdphy, base + 3, (UCHAR)((v >> 24) & 0xFFu));
    }

    UCHAR tx_size = (UCHAR)(2u + nobj * 4u - 1u);
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_TX_SIZE, tx_size);
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_INT_LATCHED_CLR,
                  PDPHY_IRQ_MSG_TX | PDPHY_IRQ_MSG_TX_FAIL | PDPHY_IRQ_MSG_TX_DISCARD);
    PdClearTxControl(c);

    /* Retry=3 (bits[6:5]=3 -> 0x60), Frame=SOP (0), SEND_MSG (bit 0 = 1) */
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_TX_CONTROL, 0x61);
    c->PdMsgId = (UCHAR)((c->PdMsgId + 1u) & 7u);

    for (ULONG t = 0; t < 25u; t++) {
        UCHAR st = 0;
        SpmiReadByte(c, c->ApidPdphy, REG_PDPHY_INT_LATCHED_STS, &st);
        if (st & PDPHY_IRQ_MSG_TX) {
            SpmiWriteByte(c->WrPdphy, REG_PDPHY_INT_LATCHED_CLR, PDPHY_IRQ_MSG_TX);
            return TRUE;
        }
        if (st & (PDPHY_IRQ_MSG_TX_FAIL | PDPHY_IRQ_MSG_TX_DISCARD)) {
            SpmiWriteByte(c->WrPdphy, REG_PDPHY_INT_LATCHED_CLR,
                          PDPHY_IRQ_MSG_TX_FAIL | PDPHY_IRQ_MSG_TX_DISCARD);
            return FALSE;
        }
        PdSleepMs(1);
    }
    return FALSE;
}

static void UsbPdDisableLocked(PBATT_CTX c)
{
    if (c->WrPdphy) {
        SpmiWriteByte(c->WrPdphy, REG_PDPHY_TX_CONTROL, 0x00);
        SpmiWriteByte(c->WrPdphy, REG_PDPHY_FRAME_FILTER, 0x00);
        SpmiWriteByte(c->WrPdphy, REG_PDPHY_EN_CONTROL, 0x00);
    }
    if (c->WrUsbin && c->PdActive) {
        SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_CMD_ICL_OVR, 0x01, 0x00);
    }
    c->PdAttempted = FALSE;
    c->PdActive = FALSE;
    c->PdVoltageMv = 0;
    c->PdCurrentMa = 0;
    c->PdMsgId = 0;
}

static void UsbPdNegotiateLocked(PBATT_CTX c)
{
    if (!c->WrPdphy || c->ApidPdphy == SPMI_NO_APID || c->PdAttempted)
        return;
    c->PdAttempted = TRUE;
    c->PdActive = FALSE;
    c->PdMsgId = 0;

    /* Permit 5V/9V input on SMB5 before requesting higher voltage */
    if (c->WrUsbin)
        SpmiWriteByte(c->WrUsbin, REG_USBIN_ADAPTER_ALLOW, 0x03);

    /* Reset & enable PDPHY (PD 2.0, Sink, UFP) */
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_TX_CONTROL, 0x00);
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_FRAME_FILTER, 0x00);
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_EN_CONTROL, 0x00);
    PdSleepMs(1);

    SpmiWriteByte(c->WrPdphy, REG_PDPHY_MSG_CONFIG, 0x01); /* Rev 2.0, PR=Sink, DR=UFP */
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_EN_CONTROL, 0x01); /* ENABLE */
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_FRAME_FILTER, 0x21); /* EN_SOP | EN_HARD_RESET */
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_INT_EN_SET, 0x7F);
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_INT_LATCHED_CLR, 0xFF);
    SpmiWriteByte(c->WrPdphy, REG_PDPHY_RX_ACK, 0x00); /* Give RX buffer to HW */

    USHORT hdr = 0;
    ULONG objs[7];
    ULONG nobj = 0;
    BOOLEAN got_src_cap = FALSE;

    /* Phase 1: Wait up to 350ms for unsolicited Source_Capabilities, then send Get_Source_Cap */
    for (ULONG t = 0; t < 700u; t++) {
        if (t == 350u) {
            USHORT get_cap_hdr = (USHORT)(((ULONG)c->PdMsgId << 9) | (1u << 6) | 0x07u);
            BOOLEAN sent = PdTransmitMsg(c, get_cap_hdr, NULL, 0);
            BattLog("pd: sent Get_Source_Cap tx=%u\n", sent ? 1u : 0u);
        }
        if (PdReceiveMsg(c, &hdr, objs, &nobj)) {
            UCHAR mtype = (UCHAR)(hdr & 0x1Fu);
            if (nobj >= 1u && mtype == 0x01u) { /* Source_Capabilities */
                got_src_cap = TRUE;
                break;
            }
        }
        PdSleepMs(1);
    }

    if (!got_src_cap) {
        BattLog("pd: no Source_Capabilities (standard 5V/non-PD charger)\n");
        SpmiWriteByte(c->WrPdphy, REG_PDPHY_FRAME_FILTER, 0x00);
        SpmiWriteByte(c->WrPdphy, REG_PDPHY_EN_CONTROL, 0x00);
        return;
    }

    /* Phase 2: Select best Fixed PDO <= 9000mV (capped at 3000mA = 27W max) */
    ULONG best_idx = 0, best_mv = 5000, best_ma = 500, best_mw = 0;
    for (ULONG i = 0; i < nobj && i < 7u; i++) {
        ULONG pdo = objs[i];
        ULONG ptype = (pdo >> 30) & 3u;
        if (ptype == 0u) { /* Fixed Supply */
            ULONG mv = ((pdo >> 10) & 0x3FFu) * 50u;
            ULONG ma = (pdo & 0x3FFu) * 10u;
            BattLog("pd: PDO[%u] Fixed %umV %umA\n", i + 1u, mv, ma);
            if (mv >= 5000u && mv <= 9000u) {
                ULONG req_ma = (ma > 3000u) ? 3000u : ma;
                ULONG mw = (mv * req_ma) / 1000u;
                if (mw > best_mw) {
                    best_mw = mw;
                    best_idx = i;
                    best_mv = mv;
                    best_ma = req_ma;
                }
            }
        } else {
            BattLog("pd: PDO[%u] type=%u raw=0x%08x\n", i + 1u, ptype, pdo);
        }
    }

    /* Phase 3: Transmit Request RDO immediately (within <5ms of Source_Capabilities) */
    ULONG op_10ma = best_ma / 10u;
    ULONG rdo = ((best_idx + 1u) << 28) | (1u << 25) /* USB Comm */ | (1u << 24) /* No Suspend */ |
                ((op_10ma & 0x3FFu) << 10) | (op_10ma & 0x3FFu);
    USHORT req_hdr = (USHORT)((1u << 12) | ((ULONG)c->PdMsgId << 9) | (1u << 6) | 0x02u);
    BOOLEAN tx_ok = PdTransmitMsg(c, req_hdr, &rdo, 1);
    BattLog("pd: Request PDO[%u] %umV @ %umA (rdo=0x%08x) tx=%u\n",
            best_idx + 1u, best_mv, best_ma, rdo, tx_ok ? 1u : 0u);
    if (!tx_ok)
        return;

    /* Phase 4: Wait up to 600ms for Accept + PS_RDY */
    BOOLEAN got_accept = FALSE, got_psrdy = FALSE;
    for (ULONG t = 0; t < 600u; t++) {
        if (PdReceiveMsg(c, &hdr, objs, &nobj)) {
            UCHAR mtype = (UCHAR)(hdr & 0x1Fu);
            if (nobj == 0u) {
                if (mtype == 0x03u) { /* Accept */
                    got_accept = TRUE;
                    BattLog("pd: Accept received\n");
                } else if (mtype == 0x06u) { /* PS_RDY */
                    got_psrdy = TRUE;
                    BattLog("pd: PS_RDY received!\n");
                    break;
                } else if (mtype == 0x04u) { /* Reject */
                    BattLog("pd: Reject received\n");
                    break;
                }
            }
        }
        PdSleepMs(1);
    }

    if (got_psrdy || got_accept) {
        c->PdActive = TRUE;
        c->PdVoltageMv = best_mv;
        c->PdCurrentMa = best_ma;
        if (c->WrUsbin) {
            UCHAR icl_raw = (UCHAR)(best_ma / 50u);
            if (icl_raw > 60u) icl_raw = 60u;
            if (icl_raw < 10u) icl_raw = 10u;
            SpmiWriteByte(c->WrUsbin, REG_USBIN_ADAPTER_ALLOW, 0x03);
            SpmiUpdateBits(c, c->ApidUsbin, c->WrUsbin, REG_USBIN_CMD_ICL_OVR, 0x01, 0x01);
            SpmiWriteByte(c->WrUsbin, REG_USBIN_ICL_CFG, icl_raw);
            BattLog("pd: CONTRACT ACTIVE %umV @ %umA -> SMB5 ICL=%umA\n",
                    best_mv, best_ma, (ULONG)icl_raw * 50u);
        }
        /* Brief 100ms tail to politely Reject any immediate DR_Swap/Get_Sink_Cap */
        for (ULONG t = 0; t < 100u; t++) {
            if (PdReceiveMsg(c, &hdr, objs, &nobj)) {
                UCHAR mtype = (UCHAR)(hdr & 0x1Fu);
                if (nobj == 0u && (mtype == 0x08u || mtype == 0x09u || mtype == 0x0Au)) {
                    USHORT rej_hdr = (USHORT)(((ULONG)c->PdMsgId << 9) | (1u << 6) | 0x04u);
                    PdTransmitMsg(c, rej_hdr, NULL, 0);
                }
            }
            PdSleepMs(1);
        }
    }
}

static NTSTATUS SpmiInit(PBATT_CTX c)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)SPMI_CORE_PHYS;
    c->SpmiCore = (PUCHAR)MmMapIoSpace(pa, SPMI_CORE_SIZE, MmNonCached);
    pa.QuadPart = (LONGLONG)SPMI_OWNERS_PHYS;
    c->SpmiOwners = (PUCHAR)MmMapIoSpace(pa, SPMI_OWNERS_SIZE, MmNonCached);
    pa.QuadPart = (LONGLONG)SPMI_OBS_PHYS;
    c->SpmiObs = (PUCHAR)MmMapIoSpace(pa, SPMI_OBS_SIZE, MmNonCached);
    if (!c->SpmiCore || !c->SpmiOwners || !c->SpmiObs) {
        SpmiUnmap(c);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ULONG version = SpmiReg(c->SpmiCore, 0);
    ULONG channels = SpmiReg(c->SpmiCore, 4) & 0x7FFu;
    BattLog("spmi: version=0x%08x channels=%u\n", version, channels);
    if (version < 0x50000000u || version >= 0x70000000u || !channels || channels > 512) {
        SpmiUnmap(c);
        return STATUS_NOT_SUPPORTED;
    }

    ULONG ow_chg = 0xFF, ow_dcdc = 0xFF, ow_usb = 0xFF, ow_tc = 0xFF, ow_misc = 0xFF, ow_pd = 0xFF, ow_g12 = 0xFF;
    c->ApidFgSoc = SpmiFindApid(c, PPID_PM8150B_FG_SOC, channels, NULL);
    c->ApidFgBat = SpmiFindApid(c, PPID_PM8150B_FG_BAT, channels, NULL);
    c->ApidFgAdc = SpmiFindApid(c, PPID_PM8150B_FG_ADC, channels, NULL);
    c->ApidChg   = SpmiFindApid(c, PPID_PM8150B_CHG,    channels, &ow_chg);
    c->ApidDcdc  = SpmiFindApid(c, PPID_PM8150B_DCDC,   channels, &ow_dcdc);
    c->ApidUsbin = SpmiFindApid(c, PPID_PM8150B_USBIN,  channels, &ow_usb);
    c->ApidTypec = SpmiFindApid(c, PPID_PM8150B_TYPEC,  channels, &ow_tc);
    c->ApidMisc  = SpmiFindApid(c, PPID_PM8150B_MISC,   channels, &ow_misc);
    c->ApidPdphy = SpmiFindApid(c, PPID_PM8150B_PDPHY,  channels, &ow_pd);
    c->ApidGpio12 = SpmiFindApid(c, PPID_PM8150B_GPIO12, channels, &ow_g12);

    BattLog("spmi: fg=%u/%u/%u chg=%u(o%u) dcdc=%u(o%u) usbin=%u(o%u) typec=%u(o%u) misc=%u(o%u) pd=%u(o%u) g12=%u(o%u)\n",
            c->ApidFgSoc, c->ApidFgBat, c->ApidFgAdc,
            c->ApidChg, ow_chg, c->ApidDcdc, ow_dcdc, c->ApidUsbin, ow_usb,
            c->ApidTypec, ow_tc, c->ApidMisc, ow_misc, c->ApidPdphy, ow_pd, c->ApidGpio12, ow_g12);

    if (c->ApidFgSoc == SPMI_NO_APID || c->ApidFgBat == SPMI_NO_APID) {
        SpmiUnmap(c);
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    c->WrChg   = MapWriteChannel(c->ApidChg, ow_chg);
    c->WrDcdc  = MapWriteChannel(c->ApidDcdc, ow_dcdc);
    c->WrUsbin = MapWriteChannel(c->ApidUsbin, ow_usb);
    c->WrTypec = MapWriteChannel(c->ApidTypec, ow_tc);
    c->WrMisc  = MapWriteChannel(c->ApidMisc, ow_misc);
    c->WrPdphy = MapWriteChannel(c->ApidPdphy, ow_pd);
    c->WrGpio12 = MapWriteChannel(c->ApidGpio12, ow_g12);
    Smb5InitHw(c);
    return STATUS_SUCCESS;
}

static BOOLEAN ReadHardwareSampleLocked(PBATT_CTX c, UMI_BATT_SAMPLE *out)
{
    UMI_BATT_SAMPLE s;
    RtlZeroMemory(&s, sizeof(s));
    s.TempTenthK = c->Sample.Valid ? c->Sample.TempTenthK : 2982u;

    /* 1. Atomic 2-byte SOC read (0x4009..0x400A): both bytes must match */
    UCHAR cap[2] = {0, 0};
    if (!NT_SUCCESS(SpmiReadBurst(c, c->ApidFgSoc, REG_FG_MONOTONIC_SOC, cap, 2)))
        return FALSE;
    s.CapRaw0 = cap[0];
    s.CapRaw1 = cap[1];
    if (cap[0] != cap[1] && c->Sample.Valid) {
        s.SocPercent = c->Sample.SocPercent;
    } else {
        UCHAR cv = (cap[0] == cap[1]) ? cap[0] : (cap[0] ? cap[0] : cap[1]);
        ULONG raw_soc;
        if (cv == 0) raw_soc = 0;
        else if (cv >= 255) raw_soc = 100;
        else {
            raw_soc = (((ULONG)(cv - 1) * 98u) + 126u) / 253u + 1u;
            if (raw_soc > 100u) raw_soc = 100u;
        }
        s.SocPercent = raw_soc;
    }
    s.CapacityMwh = (s.SocPercent * UMI_DESIGN_MWH) / 100u;

    /* 2. Atomic 2-byte Voltage read (0x41A0..0x41A1) */
    UCHAR vb[2] = {0, 0};
    ULONG mv_calc = (c->Sample.Valid && c->Sample.VoltageMv != BATTERY_UNKNOWN_VOLTAGE) ? c->Sample.VoltageMv : 3850u;
    if (NT_SUCCESS(SpmiReadBurst(c, c->ApidFgBat, REG_FG_BATT_VOLTAGE, vb, 2))) {
        USHORT raw_v = (USHORT)((ULONG)vb[0] | ((ULONG)vb[1] << 8));
        ULONG mv = (ULONG)(((ULONGLONG)raw_v * 122070ULL) / 1000000ULL);
        if (mv >= 2500u && mv <= 5000u) {
            s.VoltageMv = mv;
            mv_calc = mv;
        } else {
            s.VoltageMv = c->Sample.Valid ? c->Sample.VoltageMv : BATTERY_UNKNOWN_VOLTAGE;
        }
    } else {
        s.VoltageMv = c->Sample.Valid ? c->Sample.VoltageMv : BATTERY_UNKNOWN_VOLTAGE;
    }

    /* 3. Atomic 2-byte Current read (0x41A2..0x41A3) */
    UCHAR cb[2] = {0, 0};
    BOOLEAN cur_ok = FALSE;
    if (NT_SUCCESS(SpmiReadBurst(c, c->ApidFgBat, REG_FG_BATT_CURRENT, cb, 2))) {
        SHORT raw_i = (SHORT)((USHORT)cb[0] | ((USHORT)cb[1] << 8));
        LONGLONG ua = ((LONGLONG)raw_i * 488281LL) / 1000LL;
        s.CurrentMa = (LONG)(ua / 1000LL);
        s.RateMw = -(LONG)(((LONGLONG)s.CurrentMa * (LONGLONG)mv_calc) / 1000LL);
        cur_ok = TRUE;
    } else {
        s.RateMw = (LONG)BATTERY_UNKNOWN_RATE;
    }

    /* 4. Atomic 2-byte Temperature read (0x4288..0x4289) */
    if (c->ApidFgAdc != SPMI_NO_APID) {
        UCHAR tb[2] = {0, 0};
        if (NT_SUCCESS(SpmiReadBurst(c, c->ApidFgAdc, REG_FG_ADC_BATT_TEMP, tb, 2))) {
            SHORT raw_t = (SHORT)((USHORT)tb[0] | ((USHORT)tb[1] << 8));
            s.RawTemp = (USHORT)raw_t;
            if (raw_t >= -20 && raw_t <= 90 && raw_t != 0)
                s.TempTenthK = (ULONG)((LONG)raw_t * 10 + 2732);
            else if (raw_t >= -200 && raw_t <= 900 && raw_t != 0)
                s.TempTenthK = (ULONG)((LONG)raw_t + 2732);
        }
    }

    /* 5. Automatic 5V OTG VBUS Boost + VCONN + Charger Online & Charging State */
    BOOLEAN got_usb = FALSE;
    BOOLEAN otg_src = FALSE;
    UCHAR usbin = 0, tc_src = 0, tc_misc = 0;
    if (c->ApidTypec != SPMI_NO_APID) {
        SpmiReadByte(c, c->ApidTypec, REG_TYPEC_SRC_STATUS, &tc_src);
        SpmiReadByte(c, c->ApidTypec, REG_TYPEC_MISC_STATUS, &tc_misc);
        /* Source attached: CC_ATTACHED (bit0) + SNK_SRC_MODE=SRC (bit6), excluding audio Ra/Ra (0x01) */
        if (((tc_misc & 0x41u) == 0x41u && (tc_src & 0x1Fu) != 0x01u) || (tc_src & 0x0Cu))
            otg_src = TRUE;
    }
    if (c->WrDcdc && otg_src != c->PrevOtgOn) {
        /* 1. Drive Xiaomi Mi 10 external 5V OTG boost IC via PM8150B GPIO12 (0xCB00, active-high) */
        if (c->WrGpio12) {
            if (otg_src) {
                SpmiWriteByte(c->WrGpio12, 0x41, 0x00); /* DIG_VIN_CTL = VIN0 */
                SpmiWriteByte(c->WrGpio12, 0x42, 0x05); /* DIG_PULL_CTL = NO_PULL */
                SpmiWriteByte(c->WrGpio12, 0x45, 0x01); /* DIG_OUT_CTL = CMOS push-pull */
                SpmiWriteByte(c->WrGpio12, 0x44, 0x80); /* LV_MV_DIG_OUT_SOURCE_CTL = HIGH (invert=1, src=0) */
                SpmiWriteByte(c->WrGpio12, 0x40, 0x01); /* MODE_CTL = OUTPUT */
                SpmiWriteByte(c->WrGpio12, 0x46, 0x80); /* EN_CTL = MASTER_EN */
            } else {
                SpmiWriteByte(c->WrGpio12, 0x44, 0x00); /* OUTPUT LOW */
            }
        }
        /* 2. Also toggle PM8150B internal SMB5 OTG boost + VCONN */
        SpmiUpdateBits(c, c->ApidDcdc, c->WrDcdc, REG_DCDC_CMD_OTG, 0x01, otg_src ? 0x01 : 0x00);
        if (c->WrTypec) {
            if (otg_src && (tc_src & 0x04u)) {
                /* Enable VCONN on opposite CC pin for active Ra+Rd USB-C hubs/adapters */
                UCHAR vconn_ori = (tc_misc & 0x02u) ? 0x00u : 0x04u;
                SpmiUpdateBits(c, c->ApidTypec, c->WrTypec, REG_TYPEC_VCONN_CTRL, 0x07, (UCHAR)(vconn_ori | 0x03u));
            } else if (!otg_src) {
                SpmiUpdateBits(c, c->ApidTypec, c->WrTypec, REG_TYPEC_VCONN_CTRL, 0x02, 0x00);
            }
        }
        c->PrevOtgOn = otg_src;
        BattLog("smb5: OTG 5V VBUS %s (tc_misc=0x%02x tc_src=0x%02x g12=%u)\n",
                otg_src ? "ON" : "OFF", tc_misc, tc_src, c->WrGpio12 ? 1u : 0u);
    }
    if (c->ApidUsbin != SPMI_NO_APID &&
        NT_SUCCESS(SpmiReadByte(c, c->ApidUsbin, REG_USBIN_INT_RT_STS, &usbin))) {
        got_usb = TRUE;
        s.UsbOnline = (!otg_src && ((usbin & (1u << 4)) != 0));
    }

    if (got_usb) {
        if (s.UsbOnline) {
            if (!c->PrevUsbOnline || !c->ApsdConfigured)
                Smb5ConfigurePluggedInLocked(c);
            if (!c->PrevUsbOnline || !c->PdAttempted)
                UsbPdNegotiateLocked(c);
            s.PowerState = BATTERY_POWER_ON_LINE;
            UCHAR st1 = 0, st2 = 0;
            if (c->ApidChg != SPMI_NO_APID &&
                NT_SUCCESS(SpmiReadByte(c, c->ApidChg, REG_CHG_STATUS_1, &st1)) &&
                NT_SUCCESS(SpmiReadByte(c, c->ApidChg, REG_CHG_STATUS_2, &st2))) {
                s.ChgStat1 = st1;
                s.ChgStat2 = st2;
                UCHAR chg_mode = st1 & 0x07u;
                if (!(st2 & (1u << 5)) && chg_mode <= 4u && s.SocPercent < 100u)
                    s.PowerState |= BATTERY_CHARGING;
            } else if (cur_ok && s.CurrentMa < -5 && s.SocPercent < 100u) {
                s.PowerState |= BATTERY_CHARGING;
            }
        } else {
            c->ApsdConfigured = FALSE;
            if (c->PdAttempted || c->PdActive)
                UsbPdDisableLocked(c);
            s.PowerState = BATTERY_DISCHARGING;
        }
        c->PrevUsbOnline = s.UsbOnline;
    } else {
        if (cur_ok && s.CurrentMa < -20) {
            s.UsbOnline = TRUE;
            s.PowerState = BATTERY_POWER_ON_LINE | (s.SocPercent < 100u ? BATTERY_CHARGING : 0);
        } else {
            s.PowerState = BATTERY_DISCHARGING;
        }
    }

    if ((s.PowerState & BATTERY_DISCHARGING) && s.SocPercent <= 5u)
        s.PowerState |= BATTERY_CRITICAL;

    if (cur_ok) {
        if ((s.PowerState & BATTERY_DISCHARGING) && s.RateMw > 0) s.RateMw = 0;
        if ((s.PowerState & BATTERY_CHARGING) && s.RateMw < 0)    s.RateMw = 0;
    }

    s.Timestamp100ns = KeQueryInterruptTime();
    s.Valid = TRUE;
    *out = s;
    return TRUE;
}

/* ---- Battery Class Miniclass Callbacks ---- */
static NTSTATUS BattQueryTag(_In_ PVOID Context, _Out_ PULONG BatteryTag)
{
    UNREFERENCED_PARAMETER(Context);
    *BatteryTag = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS BattQueryInformation(
    _In_ PVOID Context, _In_ ULONG BatteryTag,
    _In_ BATTERY_QUERY_INFORMATION_LEVEL Level, _In_ LONG AtRate,
    _Out_writes_bytes_to_(BufferLength, *ReturnedLength) PVOID Buffer,
    _In_ ULONG BufferLength, _Out_ PULONG ReturnedLength)
{
    PBATT_CTX c = (PBATT_CTX)Context;
    if (BatteryTag != 1) return STATUS_NO_SUCH_DEVICE;

    PVOID src = NULL;
    ULONG len = 0;
    BATTERY_INFORMATION bi;
    BATTERY_REPORTING_SCALE scale;
    BATTERY_MANUFACTURE_DATE mdate;
    ULONG ulval = 0;

    static const WCHAR kDevName[]  = L"Xiaomi Mi 10 Battery";
    static const WCHAR kMfgName[]  = L"Xiaomi";
    static const WCHAR kUniqueId[] = L"UMI-BM4N-001";
    static const WCHAR kSerial[]   = L"UMI0001";

    switch (Level) {
    case BatteryInformation:
        RtlZeroMemory(&bi, sizeof(bi));
        bi.Capabilities = BATTERY_SYSTEM_BATTERY;
        bi.Technology = 1;
        bi.Chemistry[0] = 'L'; bi.Chemistry[1] = 'I'; bi.Chemistry[2] = 'O'; bi.Chemistry[3] = 'N';
        bi.DesignedCapacity = UMI_DESIGN_MWH;
        bi.FullChargedCapacity = UMI_DESIGN_MWH;
        bi.DefaultAlert1 = UMI_ALERT_CRIT_MWH;
        bi.DefaultAlert2 = UMI_ALERT_LOW_MWH;
        src = &bi; len = sizeof(bi);
        break;
    case BatteryGranularityInformation:
        scale.Granularity = UMI_DESIGN_MWH / 100u;
        scale.Capacity = UMI_DESIGN_MWH;
        src = &scale; len = sizeof(scale);
        break;
    case BatteryTemperature:
        KeWaitForSingleObject(&c->Lock, Executive, KernelMode, FALSE, NULL);
        ulval = c->Sample.Valid ? c->Sample.TempTenthK : 2982u;
        KeReleaseMutex(&c->Lock, FALSE);
        src = &ulval; len = sizeof(ulval);
        break;
    case BatteryEstimatedTime: {
        UMI_BATT_SAMPLE s;
        KeWaitForSingleObject(&c->Lock, Executive, KernelMode, FALSE, NULL);
        s = c->Sample;
        KeReleaseMutex(&c->Lock, FALSE);
        LONG rate = (AtRate != 0) ? AtRate : s.RateMw;
        if (s.Valid && rate < -50 && rate != (LONG)BATTERY_UNKNOWN_RATE) {
            ULONGLONG secs = ((ULONGLONG)s.CapacityMwh * 3600ULL) / (ULONGLONG)(-rate);
            ulval = (secs > 0xFFFFFFFEULL) ? BATTERY_UNKNOWN_TIME : (ULONG)secs;
        } else {
            ulval = BATTERY_UNKNOWN_TIME;
        }
        src = &ulval; len = sizeof(ulval);
        break;
    }
    case BatteryDeviceName:      src = (PVOID)kDevName;  len = sizeof(kDevName);  break;
    case BatteryManufactureName: src = (PVOID)kMfgName;  len = sizeof(kMfgName);  break;
    case BatteryUniqueID:        src = (PVOID)kUniqueId; len = sizeof(kUniqueId); break;
    case BatterySerialNumber:    src = (PVOID)kSerial;   len = sizeof(kSerial);   break;
    case BatteryManufactureDate:
        mdate.Day = 13; mdate.Month = 2; mdate.Year = 2020;
        src = &mdate; len = sizeof(mdate);
        break;
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    *ReturnedLength = len;
    if (BufferLength < len) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(Buffer, src, len);
    return STATUS_SUCCESS;
}

static NTSTATUS BattQueryStatus(_In_ PVOID Context, _In_ ULONG BatteryTag, _Out_ PBATTERY_STATUS BatteryStatus)
{
    PBATT_CTX c = (PBATT_CTX)Context;
    if (BatteryTag != 1) return STATUS_NO_SUCH_DEVICE;

    UMI_BATT_SAMPLE s;
    KeWaitForSingleObject(&c->Lock, Executive, KernelMode, FALSE, NULL);
    ULONGLONG now = KeQueryInterruptTime();
    if (!c->Sample.Valid || (now - c->Sample.Timestamp100ns) >= 5000000ULL) {
        if (ReadHardwareSampleLocked(c, &s)) c->Sample = s;
    }
    s = c->Sample;
    KeReleaseMutex(&c->Lock, FALSE);

    if (!s.Valid) return STATUS_DEVICE_NOT_READY;
    BatteryStatus->PowerState = s.PowerState;
    BatteryStatus->Capacity = s.CapacityMwh;
    BatteryStatus->Voltage = s.VoltageMv;
    BatteryStatus->Rate = s.RateMw;
    return STATUS_SUCCESS;
}

static NTSTATUS BattSetStatusNotify(_In_ PVOID Context, _In_ ULONG BatteryTag, _In_ PBATTERY_NOTIFY BatteryNotify)
{
    PBATT_CTX c = (PBATT_CTX)Context;
    if (BatteryTag != 1) return STATUS_NO_SUCH_DEVICE;
    KeWaitForSingleObject(&c->Lock, Executive, KernelMode, FALSE, NULL);
    c->Notify = *BatteryNotify;
    c->NotifyEnabled = TRUE;
    KeReleaseMutex(&c->Lock, FALSE);
    return STATUS_SUCCESS;
}

static NTSTATUS BattDisableStatusNotify(_In_ PVOID Context)
{
    PBATT_CTX c = (PBATT_CTX)Context;
    KeWaitForSingleObject(&c->Lock, Executive, KernelMode, FALSE, NULL);
    c->NotifyEnabled = FALSE;
    KeReleaseMutex(&c->Lock, FALSE);
    return STATUS_SUCCESS;
}

static NTSTATUS BattSetInformation(_In_ PVOID Context, _In_ ULONG BatteryTag,
                                   _In_ BATTERY_SET_INFORMATION_LEVEL Level, _In_opt_ PVOID Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Buffer);
    return STATUS_NOT_SUPPORTED;
}

/* ---- KMDF Callbacks ---- */
EVT_WDF_TIMER BattTimerTick;
VOID BattTimerTick(_In_ WDFTIMER Timer)
{
    PBATT_CTX c = GetBattCtx(WdfTimerGetParentObject(Timer));
    if (!InterlockedCompareExchange(&c->Running, 1, 1)) return;

    BOOLEAN notify = FALSE;
    UMI_BATT_SAMPLE prev, cur;

    KeWaitForSingleObject(&c->Lock, Executive, KernelMode, FALSE, NULL);
    prev = c->Sample;
    BOOLEAN ok = ReadHardwareSampleLocked(c, &cur);
    if (ok) {
        c->Sample = cur;
        if (!prev.Valid || prev.PowerState != cur.PowerState || prev.SocPercent != cur.SocPercent) {
            notify = TRUE;
        } else if (c->NotifyEnabled) {
            if ((cur.PowerState != c->Notify.PowerState) ||
                (cur.CapacityMwh < c->Notify.LowCapacity) ||
                (cur.CapacityMwh > c->Notify.HighCapacity)) {
                notify = TRUE;
            }
        }
    }
    KeReleaseMutex(&c->Lock, FALSE);

    if (ok && (notify || (c->Ticks % 15) == 0)) {
        BattLog("batt: soc=%u%%(raw=%02x/%02x) V=%umV I=%dmA rate=%dmW T=%u usb=%u chg=%02x/%02x ps=0x%x\n",
                cur.SocPercent, cur.CapRaw0, cur.CapRaw1, cur.VoltageMv, cur.CurrentMa, cur.RateMw,
                cur.TempTenthK, cur.UsbOnline ? 1u : 0u, cur.ChgStat1, cur.ChgStat2, cur.PowerState);
    }
    c->Ticks++;

    if (notify && c->ClassData)
        BatteryClassStatusNotify(c->ClassData);

    if (InterlockedCompareExchange(&c->Running, 1, 1))
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(UMI_POLL_MS));
}

EVT_WDFDEVICE_WDM_IRP_PREPROCESS BattPreprocessIoctl;
NTSTATUS BattPreprocessIoctl(_In_ WDFDEVICE Device, _Inout_ PIRP Irp)
{
    PBATT_CTX c = GetBattCtx(Device);
    if (c->ClassData) {
        NTSTATUS st = BatteryClassIoctl(c->ClassData, Irp);
        if (st != STATUS_NOT_SUPPORTED)
            return st;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}

EVT_WDF_DEVICE_PREPARE_HARDWARE BattPrepareHardware;
NTSTATUS BattPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Tr)
{
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Tr);
    PBATT_CTX c = GetBattCtx(Device);
    BattLogInit();
    BattLog("=== UMI Battery KMDF v1.0.0.8 (USB-PD Sink + Ext 5V Boost GPIO12 + OTG + VCONN) PrepareHardware ===\n");

    NTSTATUS st = SpmiInit(c);
    if (!NT_SUCCESS(st)) {
        BattLog("batt: SpmiInit failed 0x%x\n", (ULONG)st);
        return st;
    }

    UMI_BATT_SAMPLE s;
    KeWaitForSingleObject(&c->Lock, Executive, KernelMode, FALSE, NULL);
    if (ReadHardwareSampleLocked(c, &s)) {
        c->Sample = s;
        BattLog("batt: initial soc=%u%%(raw=%02x/%02x) V=%umV I=%dmA rate=%dmW T=%u usb=%u chg=%02x/%02x ps=0x%x\n",
                s.SocPercent, s.CapRaw0, s.CapRaw1, s.VoltageMv, s.CurrentMa, s.RateMw,
                s.TempTenthK, s.UsbOnline ? 1u : 0u, s.ChgStat1, s.ChgStat2, s.PowerState);
    }
    KeReleaseMutex(&c->Lock, FALSE);

    BATTERY_MINIPORT_INFO binfo;
    RtlZeroMemory(&binfo, sizeof(binfo));
    binfo.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
    binfo.MinorVersion = BATTERY_CLASS_MINOR_VERSION;
    binfo.Context = c;
    binfo.QueryTag = BattQueryTag;
    binfo.QueryInformation = BattQueryInformation;
    binfo.SetInformation = BattSetInformation;
    binfo.QueryStatus = BattQueryStatus;
    binfo.SetStatusNotify = BattSetStatusNotify;
    binfo.DisableStatusNotify = BattDisableStatusNotify;
    binfo.Pdo = WdfDeviceWdmGetPhysicalDevice(Device);
    binfo.DeviceName = NULL;

    st = BatteryClassInitializeDevice(&binfo, &c->ClassData);
    BattLog("batt: BatteryClassInitializeDevice st=0x%x\n", (ULONG)st);
    if (!NT_SUCCESS(st)) {
        SpmiUnmap(c);
        return st;
    }
    return STATUS_SUCCESS;
}

EVT_WDF_DEVICE_RELEASE_HARDWARE BattReleaseHardware;
NTSTATUS BattReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Tr)
{
    UNREFERENCED_PARAMETER(Tr);
    PBATT_CTX c = GetBattCtx(Device);
    InterlockedExchange(&c->Running, 0);
    WdfTimerStop(c->Timer, TRUE);
    PVOID cd = c->ClassData;
    c->ClassData = NULL;
    if (cd) BatteryClassUnload(cd);
    SpmiUnmap(c);
    return STATUS_SUCCESS;
}

EVT_WDF_DEVICE_D0_ENTRY BattD0Entry;
NTSTATUS BattD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Prev)
{
    UNREFERENCED_PARAMETER(Prev);
    PBATT_CTX c = GetBattCtx(Device);
    InterlockedExchange(&c->Running, 1);
    WdfTimerStart(c->Timer, WDF_REL_TIMEOUT_IN_MS(UMI_POLL_MS));
    return STATUS_SUCCESS;
}

EVT_WDF_DEVICE_D0_EXIT BattD0Exit;
NTSTATUS BattD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Next)
{
    UNREFERENCED_PARAMETER(Next);
    PBATT_CTX c = GetBattCtx(Device);
    InterlockedExchange(&c->Running, 0);
    WdfTimerStop(c->Timer, TRUE);
    return STATUS_SUCCESS;
}

EVT_WDF_DRIVER_DEVICE_ADD BattDeviceAdd;
NTSTATUS BattDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BATTERY);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    NTSTATUS st = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit, BattPreprocessIoctl, IRP_MJ_DEVICE_CONTROL, NULL, 0);
    if (!NT_SUCCESS(st)) return st;

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = BattPrepareHardware;
    pnp.EvtDeviceReleaseHardware = BattReleaseHardware;
    pnp.EvtDeviceD0Entry = BattD0Entry;
    pnp.EvtDeviceD0Exit = BattD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BATT_CTX);
    attr.ExecutionLevel = WdfExecutionLevelPassive;

    WDFDEVICE device;
    st = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(st)) return st;

    PBATT_CTX c = GetBattCtx(device);
    RtlZeroMemory(c, sizeof(*c));
    c->Device = device;
    c->ApidFgSoc = SPMI_NO_APID;
    c->ApidFgBat = SPMI_NO_APID;
    c->ApidFgAdc = SPMI_NO_APID;
    c->ApidChg   = SPMI_NO_APID;
    c->ApidDcdc  = SPMI_NO_APID;
    c->ApidUsbin = SPMI_NO_APID;
    c->ApidTypec = SPMI_NO_APID;
    c->ApidMisc  = SPMI_NO_APID;
    KeInitializeMutex(&c->Lock, 0);

    WDF_TIMER_CONFIG tc;
    WDF_TIMER_CONFIG_INIT(&tc, BattTimerTick);
    tc.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfTimerCreate(&tc, &attr, &c->Timer);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, BattDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
