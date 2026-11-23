/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Xiaomi Mi 10 (umi) Windows 11 VHF HID Sensor Collection (HUTRR39 Usage Page 0x20)
 * Exposes 3D Accelerometer (Auto-Rotation), Gyrometer, Compass, Ambient Light, and Barometer
 * to Windows 11 inbox SensorsHidClassDriver (hidsensor.sys).
 */

#include "umi_sensors.h"
#include <wdf.h>
#include <vhf.h>

#define HID_RPT_ACCEL   1u
#define HID_RPT_GYRO    2u
#define HID_RPT_MAG     3u
#define HID_RPT_ALS     4u
#define HID_RPT_BARO    5u
#define HID_RPT_COMP    6u

/* HUTRR39 Named Array Feature & Input Report Macros (1-based Logical Min=1) */
#define HID_SNS_FEATURE_PROPS(cs_lo, cs_hi) \
    /* Connection Type (0x0309): 1=Integrated, 2=Attached, 3=External */ \
    0x0A, 0x09, 0x03, 0x15, 0x01, 0x25, 0x03, 0x75, 0x08, 0x95, 0x01, \
    0xA1, 0x02, \
      0x0A, 0x30, 0x08, 0x0A, 0x31, 0x08, 0x0A, 0x32, 0x08, \
      0xB1, 0x00, \
    0xC0, \
    /* Reporting State (0x0316): 1=NoEvents, 2=AllEvents, 3=Threshold, 4=NoWake, 5=AllWake, 6=ThreshWake */ \
    0x0A, 0x16, 0x03, 0x15, 0x01, 0x25, 0x06, 0x75, 0x08, 0x95, 0x01, \
    0xA1, 0x02, \
      0x0A, 0x40, 0x08, 0x0A, 0x41, 0x08, 0x0A, 0x42, 0x08, \
      0x0A, 0x43, 0x08, 0x0A, 0x44, 0x08, 0x0A, 0x45, 0x08, \
      0xB1, 0x00, \
    0xC0, \
    /* Power State (0x0319): 1=Undefined, 2=D0Full, 3=D1Low, 4=D2Standby, 5=D3Sleep, 6=D4Off */ \
    0x0A, 0x19, 0x03, 0x15, 0x01, 0x25, 0x06, 0x75, 0x08, 0x95, 0x01, \
    0xA1, 0x02, \
      0x0A, 0x50, 0x08, 0x0A, 0x51, 0x08, 0x0A, 0x52, 0x08, \
      0x0A, 0x53, 0x08, 0x0A, 0x54, 0x08, 0x0A, 0x55, 0x08, \
      0xB1, 0x00, \
    0xC0, \
    /* Sensor State (0x0201): 1=Undefined, 2=Ready, 3=NotAvail, 4=NoData, 5=Init, 6=AccessDenied, 7=Error */ \
    0x0A, 0x01, 0x02, 0x15, 0x01, 0x25, 0x07, 0x75, 0x08, 0x95, 0x01, \
    0xA1, 0x02, \
      0x0A, 0x00, 0x08, 0x0A, 0x01, 0x08, 0x0A, 0x02, 0x08, 0x0A, 0x03, 0x08, \
      0x0A, 0x04, 0x08, 0x0A, 0x05, 0x08, 0x0A, 0x06, 0x08, \
      0xB1, 0x00, \
    0xC0, \
    /* Report Interval (0x030E, ms, u16) + Per-Sensor Change Sensitivity Abs (u16) */ \
    0x0A, 0x0E, 0x03, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x55, 0x00, 0x75, 0x10, 0x95, 0x01, 0xB1, 0x02, \
    0x0A, cs_lo, cs_hi, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x55, 0x00, 0x75, 0x10, 0x95, 0x01, 0xB1, 0x02

#define HID_SNS_INPUT_HDR \
    /* Sensor State (0x0201) */ \
    0x0A, 0x01, 0x02, 0x15, 0x01, 0x25, 0x07, 0x75, 0x08, 0x95, 0x01, \
    0xA1, 0x02, \
      0x0A, 0x00, 0x08, 0x0A, 0x01, 0x08, 0x0A, 0x02, 0x08, 0x0A, 0x03, 0x08, \
      0x0A, 0x04, 0x08, 0x0A, 0x05, 0x08, 0x0A, 0x06, 0x08, \
      0x81, 0x00, \
    0xC0, \
    /* Sensor Event (0x0202): 1=Unknown, 2=StateChanged, 3=PropChanged, 4=DataUpdated, 5=PollResp, 6=ChangeSens */ \
    0x0A, 0x02, 0x02, 0x15, 0x01, 0x25, 0x06, 0x75, 0x08, 0x95, 0x01, \
    0xA1, 0x02, \
      0x0A, 0x10, 0x08, 0x0A, 0x11, 0x08, 0x0A, 0x12, 0x08, \
      0x0A, 0x13, 0x08, 0x0A, 0x14, 0x08, 0x0A, 0x15, 0x08, \
      0x81, 0x00, \
    0xC0

static const UCHAR g_SensorHidDesc[] = {
    /* ---- Report ID 1: 3D Accelerometer (0x73) ---- */
    0x05, 0x20, 0x09, 0x73, 0xA1, 0x01,
    0x85, HID_RPT_ACCEL,
    HID_SNS_FEATURE_PROPS(0x52, 0x14),
    HID_SNS_INPUT_HDR,
    0x0A, 0x53, 0x04, 0x0A, 0x54, 0x04, 0x0A, 0x55, 0x04,
    0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x55, 0x0D, 0x75, 0x10, 0x95, 0x03, 0x81, 0x02,
    0xC0,

    /* ---- Report ID 2: 3D Gyrometer (0x76) ---- */
    0x05, 0x20, 0x09, 0x76, 0xA1, 0x01,
    0x85, HID_RPT_GYRO,
    HID_SNS_FEATURE_PROPS(0x56, 0x14),
    HID_SNS_INPUT_HDR,
    0x0A, 0x57, 0x04, 0x0A, 0x58, 0x04, 0x0A, 0x59, 0x04,
    0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x55, 0x0E, 0x75, 0x10, 0x95, 0x03, 0x81, 0x02,
    0xC0,

    /* ---- Report ID 3 (Magnetometer 0xC2) + Report ID 6 (Compass 0x83) inside Sensor Collection (0x01) ---- */
    0x05, 0x20, 0x09, 0x01, 0xA1, 0x01,
      0x05, 0x20, 0x09, 0xC2, 0xA1, 0x00,
        0x85, HID_RPT_MAG,
        HID_SNS_FEATURE_PROPS(0x84, 0x14),
        HID_SNS_INPUT_HDR,
        /* Magnetic Flux X/Y/Z (0x0485..0x0487): 0.01 mG (Unit Exp -2) */
        0x0A, 0x85, 0x04, 0x0A, 0x86, 0x04, 0x0A, 0x87, 0x04,
        0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x55, 0x0E, 0x75, 0x10, 0x95, 0x03, 0x81, 0x02,
        /* Magnetometer Accuracy (0x0488): 1=Low (0x08E0), 2=Medium (0x08E1), 3=High (0x08E2) */
        0x0A, 0x88, 0x04, 0x15, 0x01, 0x25, 0x03, 0x55, 0x00, 0x75, 0x08, 0x95, 0x01,
        0xA1, 0x02,
          0x0A, 0xE0, 0x08, 0x0A, 0xE1, 0x08, 0x0A, 0xE2, 0x08,
          0x81, 0x00,
        0xC0,
      0xC0,
      0x05, 0x20, 0x09, 0x83, 0xA1, 0x00,
        0x85, HID_RPT_COMP,
        HID_SNS_FEATURE_PROPS(0x71, 0x14),
        HID_SNS_INPUT_HDR,
        /* All 4 Compass Headings (0x0475..0x0478): 0..359.99 deg (Unit Exp -2) */
        0x0A, 0x75, 0x04, 0x0A, 0x76, 0x04, 0x0A, 0x77, 0x04, 0x0A, 0x78, 0x04,
        0x15, 0x00, 0x27, 0x9F, 0x8C, 0x00, 0x00, 0x55, 0x0E, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02,
        /* Magnetometer Accuracy (0x0488) */
        0x0A, 0x88, 0x04, 0x15, 0x01, 0x25, 0x03, 0x55, 0x00, 0x75, 0x08, 0x95, 0x01,
        0xA1, 0x02,
          0x0A, 0xE0, 0x08, 0x0A, 0xE1, 0x08, 0x0A, 0xE2, 0x08,
          0x81, 0x00,
        0xC0,
      0xC0,
    0xC0,

    /* ---- Report ID 4: Ambient Light Sensor (0x41) ---- */
    0x05, 0x20, 0x09, 0x41, 0xA1, 0x01,
    0x85, HID_RPT_ALS,
    HID_SNS_FEATURE_PROPS(0xD1, 0x14),
    HID_SNS_INPUT_HDR,
    0x0A, 0xD1, 0x04,
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x01, 0x00, 0x55, 0x00, 0x75, 0x20, 0x95, 0x01, 0x81, 0x02,
    0xC0,

    /* ---- Report ID 5: Barometer / Pressure (0x31) ---- */
    0x05, 0x20, 0x09, 0x31, 0xA1, 0x01,
    0x85, HID_RPT_BARO,
    HID_SNS_FEATURE_PROPS(0x31, 0x14),
    HID_SNS_INPUT_HDR,
    0x0A, 0x31, 0x04,
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x03, 0x00, 0x55, 0x0B, 0x75, 0x20, 0x95, 0x01, 0x81, 0x02,
    0xC0
};

#pragma pack(push, 1)
typedef struct {
    u8  report_id;
    u8  conn_type;       /* 1 = PC Integrated */
    u8  report_state;    /* 2 = All Events */
    u8  power_state;     /* 2 = D0 Full Power */
    u8  sensor_state;    /* 2 = Ready */
    u16 report_interval; /* 100 ms */
    u16 change_sens;     /* 0 */
} HID_SNS_FEATURE;

typedef struct {
    u8  report_id;
    u8  conn_type;
    u8  report_state;
    u8  power_state;
    u8  sensor_state;
    u16 report_interval;
    u16 change_sens;
    u16 change_sens2;
} HID_SNS_MAG_FEAT;

typedef struct {
    u8  report_id;
    u8  sensor_state;    /* 2 = Ready */
    u8  sensor_event;    /* 4 = Data Updated */
    s16 x, y, z;
} HID_SNS_3AXIS_IN;

typedef struct {
    u8  report_id;
    u8  sensor_state;    /* 2 = Ready */
    u8  sensor_event;    /* 4 = Data Updated */
    s16 x, y, z;
    u8  accuracy;        /* 3 = High (0x08E2) */
} HID_SNS_MAG_IN;

typedef struct {
    u8  report_id;
    u8  sensor_state;
    u8  sensor_event;
    u16 h_comp_mag;
    u16 h_comp_true;
    u16 h_mag;
    u16 h_true;
    u8  accuracy;
} HID_SNS_COMP_IN;

typedef struct {
    u8  report_id;
    u8  sensor_state;    /* 2 = Ready */
    u8  sensor_event;    /* 4 = Data Updated */
    u32 val;
} HID_SNS_1VAL_IN;

typedef struct {
    u8  report_id;
    u8  sensor_state;    /* 2 = Ready */
    u8  sensor_event;    /* 4 = Data Updated */
    u8  orient;          /* 1=NotRot, 2=Rot90, 3=Rot180, 4=Rot270, 5=FaceUp, 6=FaceDown */
} HID_SNS_ORIENT_IN;
#pragma pack(pop)

static VHFHANDLE g_Vhf = NULL;
static HID_SNS_FEATURE   g_Feat[7];
static HID_SNS_MAG_FEAT  g_MagFeat;
static HID_SNS_3AXIS_IN  g_Last3[3]; /* 1=accel, 2=gyro */
static HID_SNS_MAG_IN    g_LastMag;  /* 3=mag */
static HID_SNS_COMP_IN   g_LastComp; /* 6=compass */
static HID_SNS_1VAL_IN   g_Last1[6]; /* 4=als, 5=baro */
static HID_SNS_ORIENT_IN g_LastOrient;

static u16 umi_heading_cdeg(s32 x, s32 y)
{
    /* Integer atan2(x, y) in centi-degrees [0..35999], 0=North (+Y), 90=East (+X) */
    if (x == 0 && y == 0) return 0;
    s32 ax = (x < 0) ? -x : x;
    s32 ay = (y < 0) ? -y : y;
    s32 mn = (ax < ay) ? ax : ay;
    s32 mx = (ax > ay) ? ax : ay;
    s64 t = ((s64)mn * 1000) / mx;
    s32 a = (s32)((4500 * t) / 1000 + (1570 * t * (1000 - t)) / 1000000); /* 0..4500 */
    if (ax > ay) a = 9000 - a; /* 0..9000 in 1st quadrant */
    s32 deg = 0;
    if (x >= 0 && y >= 0) deg = a;
    else if (x >= 0 && y < 0) deg = 18000 - a;
    else if (x < 0 && y < 0) deg = 18000 + a;
    else deg = 36000 - a;
    if (deg >= 36000) deg -= 36000;
    if (deg < 0) deg += 36000;
    return (u16)deg;
}

static s16 clamp_s16(s64 v)
{
    if (v > 32767) return 32767;
    if (v < -32767) return -32767;
    return (s16)v;
}

static u32 g_hid_feat_logs = 0;

static VOID EvtVhfGetFeature(PVOID Ctx, VHFOPERATIONHANDLE Op, PVOID Unused, PHID_XFER_PACKET Pkt)
{
    UNREFERENCED_PARAMETER(Ctx); UNREFERENCED_PARAMETER(Unused);
    u8 id = Pkt->reportId;
    if (g_hid_feat_logs < 16) { g_hid_feat_logs++; umi_log("hid: GetFeature id=%u len=%u\n", id, Pkt->reportBufferLen); }
    if (id >= 1 && id <= 6 && Pkt->reportBufferLen >= sizeof(HID_SNS_FEATURE)) {
        RtlCopyMemory(Pkt->reportBuffer, &g_Feat[id], sizeof(HID_SNS_FEATURE));
        VhfAsyncOperationComplete(Op, STATUS_SUCCESS);
    } else {
        VhfAsyncOperationComplete(Op, STATUS_INVALID_PARAMETER);
    }
}

static VOID EvtVhfSetFeature(PVOID Ctx, VHFOPERATIONHANDLE Op, PVOID Unused, PHID_XFER_PACKET Pkt)
{
    UNREFERENCED_PARAMETER(Ctx); UNREFERENCED_PARAMETER(Unused);
    u8 id = Pkt->reportId;
    if (g_hid_feat_logs < 16) { g_hid_feat_logs++; umi_log("hid: SetFeature id=%u len=%u\n", id, Pkt->reportBufferLen); }
    if (id >= 1 && id <= 6 && Pkt->reportBufferLen >= sizeof(HID_SNS_FEATURE)) {
        RtlCopyMemory(&g_Feat[id], Pkt->reportBuffer, sizeof(HID_SNS_FEATURE));
        g_Feat[id].report_id = id;
        g_Feat[id].sensor_state = 2; /* Ready */
        VhfAsyncOperationComplete(Op, STATUS_SUCCESS);
    } else {
        VhfAsyncOperationComplete(Op, STATUS_INVALID_PARAMETER);
    }
}

static VOID EvtVhfGetInputReport(PVOID Ctx, VHFOPERATIONHANDLE Op, PVOID Unused, PHID_XFER_PACKET Pkt)
{
    UNREFERENCED_PARAMETER(Ctx); UNREFERENCED_PARAMETER(Unused);
    u8 id = Pkt->reportId;
    if ((id == 1 || id == 2) && Pkt->reportBufferLen >= sizeof(HID_SNS_3AXIS_IN)) {
        g_Last3[id].sensor_event = 5; /* Poll Response */
        RtlCopyMemory(Pkt->reportBuffer, &g_Last3[id], sizeof(HID_SNS_3AXIS_IN));
        VhfAsyncOperationComplete(Op, STATUS_SUCCESS);
    } else if (id == HID_RPT_MAG && Pkt->reportBufferLen >= sizeof(HID_SNS_MAG_IN)) {
        g_LastMag.sensor_event = 5;
        RtlCopyMemory(Pkt->reportBuffer, &g_LastMag, sizeof(HID_SNS_MAG_IN));
        VhfAsyncOperationComplete(Op, STATUS_SUCCESS);
    } else if (id == HID_RPT_COMP && Pkt->reportBufferLen >= sizeof(HID_SNS_COMP_IN)) {
        g_LastComp.sensor_event = 5;
        RtlCopyMemory(Pkt->reportBuffer, &g_LastComp, sizeof(HID_SNS_COMP_IN));
        VhfAsyncOperationComplete(Op, STATUS_SUCCESS);
    } else if ((id == 4 || id == 5) && Pkt->reportBufferLen >= sizeof(HID_SNS_1VAL_IN)) {
        g_Last1[id].sensor_event = 5; /* Poll Response */
        RtlCopyMemory(Pkt->reportBuffer, &g_Last1[id], sizeof(HID_SNS_1VAL_IN));
        VhfAsyncOperationComplete(Op, STATUS_SUCCESS);
    } else {
        VhfAsyncOperationComplete(Op, STATUS_INVALID_PARAMETER);
    }
}

NTSTATUS umi_hid_init(PVOID WdfDevicePtr)
{
    WDFDEVICE dev = (WDFDEVICE)WdfDevicePtr;
    for (u8 id = 1; id <= 6; id++) {
        g_Feat[id].report_id = id;
        g_Feat[id].conn_type = 1;
        g_Feat[id].report_state = 2;
        g_Feat[id].power_state = 2;
        g_Feat[id].sensor_state = 2;
        g_Feat[id].report_interval = 100;
        g_Feat[id].change_sens = 0;
    }
    g_MagFeat.report_id = HID_RPT_MAG;
    g_MagFeat.conn_type = 1; g_MagFeat.report_state = 2; g_MagFeat.power_state = 2;
    g_MagFeat.sensor_state = 2; g_MagFeat.report_interval = 100;
    g_MagFeat.change_sens = 0; g_MagFeat.change_sens2 = 0;
    for (u8 id = 1; id <= 2; id++) {
        g_Last3[id].report_id = id; g_Last3[id].sensor_state = 2; g_Last3[id].sensor_event = 4;
    }
    g_Last3[HID_RPT_ACCEL].z = -1000; /* default 1G flat */
    g_LastMag.report_id = HID_RPT_MAG; g_LastMag.sensor_state = 2; g_LastMag.sensor_event = 4; g_LastMag.accuracy = 3;
    g_LastComp.report_id = HID_RPT_COMP; g_LastComp.sensor_state = 2; g_LastComp.sensor_event = 4; g_LastComp.accuracy = 3;
    for (u8 id = 4; id <= 5; id++) {
        g_Last1[id].report_id = id; g_Last1[id].sensor_state = 2; g_Last1[id].sensor_event = 4;
    }

    VHF_CONFIG cfg;
    VHF_CONFIG_INIT(&cfg, WdfDeviceWdmGetDeviceObject(dev), sizeof(g_SensorHidDesc), (PUCHAR)g_SensorHidDesc);
    cfg.VendorID = 0x2717; /* Xiaomi */
    cfg.ProductID = 0x8250;
    cfg.VersionNumber = 10;
    cfg.EvtVhfAsyncOperationGetFeature = EvtVhfGetFeature;
    cfg.EvtVhfAsyncOperationSetFeature = EvtVhfSetFeature;
    cfg.EvtVhfAsyncOperationGetInputReport = EvtVhfGetInputReport;

    NTSTATUS st = VhfCreate(&cfg, &g_Vhf);
    if (!NT_SUCCESS(st)) {
        umi_log("hid: VhfCreate failed 0x%x\n", (u32)st);
        return st;
    }
    st = VhfStart(g_Vhf);
    umi_log("hid: VHF Sensor Collection (HUTRR39) started st=0x%x\n", (u32)st);
    return st;
}

void umi_hid_stop(void)
{
    if (g_Vhf) {
        VhfDelete(g_Vhf, TRUE);
        g_Vhf = NULL;
    }
}

static BOOLEAN g_hid_sub_logged[7] = { 0 };

void umi_hid_submit_sensor(u32 kind, s32 v0, s32 v1, s32 v2)
{
    if (!g_Vhf || kind < 1 || kind > 5) return;
    NTSTATUS st = STATUS_SUCCESS;
    if (kind == HID_RPT_ACCEL) {
        g_Last3[1].report_id = 1; g_Last3[1].sensor_state = 2; g_Last3[1].sensor_event = 4;
        /* Windows HID Accelerometer uses negated gravity convention vs Android/SLPI */
        g_Last3[1].x = clamp_s16(-((s64)v0 * 1000) / 9807);
        g_Last3[1].y = clamp_s16(-((s64)v1 * 1000) / 9807);
        g_Last3[1].z = clamp_s16(-((s64)v2 * 1000) / 9807);
        HID_XFER_PACKET p = { (PUCHAR)&g_Last3[1], sizeof(HID_SNS_3AXIS_IN), 1 };
        st = VhfReadReportSubmit(g_Vhf, &p);
    } else if (kind == HID_RPT_GYRO) {
        g_Last3[2].report_id = 2; g_Last3[2].sensor_state = 2; g_Last3[2].sensor_event = 4;
        g_Last3[2].x = clamp_s16(((s64)v0 * 5730) / 1000);
        g_Last3[2].y = clamp_s16(((s64)v1 * 5730) / 1000);
        g_Last3[2].z = clamp_s16(((s64)v2 * 5730) / 1000);
        HID_XFER_PACKET p = { (PUCHAR)&g_Last3[2], sizeof(HID_SNS_3AXIS_IN), 2 };
        st = VhfReadReportSubmit(g_Vhf, &p);
    } else if (kind == HID_RPT_MAG) {
        g_LastMag.report_id = 3; g_LastMag.sensor_state = 2; g_LastMag.sensor_event = 4;
        /* SLPI mag is milli-uT; 1 uT = 10 mG, so 1 milli-uT = 0.01 mG (Unit Exp -2) */
        g_LastMag.x = clamp_s16(v0);
        g_LastMag.y = clamp_s16(v1);
        g_LastMag.z = clamp_s16(v2);
        g_LastMag.accuracy = 3; /* High */
        HID_XFER_PACKET p = { (PUCHAR)&g_LastMag, sizeof(HID_SNS_MAG_IN), 3 };
        st = VhfReadReportSubmit(g_Vhf, &p);

        u16 hdg = umi_heading_cdeg(v0, v1);
        g_LastComp.report_id = HID_RPT_COMP; g_LastComp.sensor_state = 2; g_LastComp.sensor_event = 4;
        g_LastComp.h_comp_mag = hdg; g_LastComp.h_comp_true = hdg;
        g_LastComp.h_mag = hdg; g_LastComp.h_true = hdg;
        g_LastComp.accuracy = 3;
        HID_XFER_PACKET pc = { (PUCHAR)&g_LastComp, sizeof(HID_SNS_COMP_IN), HID_RPT_COMP };
        VhfReadReportSubmit(g_Vhf, &pc);
    } else if (kind == HID_RPT_ALS) {
        g_Last1[4].report_id = 4; g_Last1[4].sensor_state = 2; g_Last1[4].sensor_event = 4;
        g_Last1[4].val = (v0 > 0) ? (u32)(v0 / 1000) : 0u;
        HID_XFER_PACKET p = { (PUCHAR)&g_Last1[4], sizeof(HID_SNS_1VAL_IN), 4 };
        st = VhfReadReportSubmit(g_Vhf, &p);
    } else if (kind == HID_RPT_BARO) {
        g_Last1[5].report_id = 5; g_Last1[5].sensor_state = 2; g_Last1[5].sensor_event = 4;
        g_Last1[5].val = (v0 > 0) ? (u32)(v0 / 10) : 101325u;
        HID_XFER_PACKET p = { (PUCHAR)&g_Last1[5], sizeof(HID_SNS_1VAL_IN), 5 };
        st = VhfReadReportSubmit(g_Vhf, &p);
    }
    if (!g_hid_sub_logged[kind]) {
        g_hid_sub_logged[kind] = TRUE;
        umi_log("hid: first submit kind=%u st=0x%x\n", kind, (u32)st);
    }
}
