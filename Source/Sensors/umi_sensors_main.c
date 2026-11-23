/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Xiaomi Mi 10 (umi) SLPI Sensor Hub Stage 1 Driver (umi-sensors.sys) */

#include "umi_sensors.h"
#include <wdf.h>

static HANDLE g_LogHandle = NULL;

void umi_log_init(void)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&path, L"\\SystemRoot\\Temp\\umi-sensors.log");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    ZwCreateFile(&g_LogHandle, FILE_GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                 FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0);
}

void umi_log_close(void)
{
    if (g_LogHandle) { ZwClose(g_LogHandle); g_LogHandle = NULL; }
}

void umi_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int len = 0;
    va_start(ap, fmt);
    const char *p = fmt;
    while (*p && len < (int)sizeof(buf) - 2) {
        if (*p == '%' && p[1]) {
            p++;
            int is_ll = 0;
            if (*p == 'l' && p[1] == 'l') { is_ll = 1; p += 2; }
            if (*p == 's') {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && len < (int)sizeof(buf) - 2) buf[len++] = *s++;
            } else if (*p == 'u' || *p == 'd') {
                int is_signed = (*p == 'd');
                long long v = is_ll ? va_arg(ap, long long) : (is_signed ? (long long)va_arg(ap, int) : (long long)va_arg(ap, unsigned int));
                if (is_signed && v < 0) { buf[len++] = '-'; v = -v; }
                char tmp[24]; int n = 0;
                unsigned long long uv = (unsigned long long)v;
                do { tmp[n++] = (char)('0' + (uv % 10)); uv /= 10; } while (uv && n < 24);
                while (n-- > 0 && len < (int)sizeof(buf) - 2) buf[len++] = tmp[n];
            } else if (*p == 'x' || *p == 'X') {
                unsigned long long v = is_ll ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
                const char *hex = (*p == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                char tmp[20]; int n = 0;
                do { tmp[n++] = hex[v & 0xF]; v >>= 4; } while (v && n < 20);
                while (n-- > 0 && len < (int)sizeof(buf) - 2) buf[len++] = tmp[n];
            } else {
                buf[len++] = *p;
            }
            p++;
        } else {
            buf[len++] = *p++;
        }
    }
    va_end(ap);
    buf[len] = '\0';
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "umi-sns: %s", buf);
    if (g_LogHandle && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        IO_STATUS_BLOCK iosb;
        ZwWriteFile(g_LogHandle, NULL, NULL, NULL, &iosb, buf, (ULONG)len, NULL, NULL);
    }
}

u8 *umi_load_fw(const char *name, u32 *out_size)
{
    WCHAR wpath[160];
    const WCHAR *prefix = L"\\SystemRoot\\System32\\drivers\\umi-sensors\\";
    u32 idx = 0;
    while (prefix[idx]) { wpath[idx] = prefix[idx]; idx++; }
    for (u32 i = 0; name[i] && idx < 158; i++) wpath[idx++] = (WCHAR)name[i];
    wpath[idx] = 0;

    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    FILE_STANDARD_INFORMATION fsi;
    *out_size = 0;

    RtlInitUnicodeString(&path, wpath);
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS st = ZwCreateFile(&h, FILE_GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                               FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) return NULL;

    st = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (!NT_SUCCESS(st) || fsi.EndOfFile.QuadPart <= 0 || fsi.EndOfFile.QuadPart > 8 * 1024 * 1024) {
        ZwClose(h); return NULL;
    }
    u32 sz = (u32)fsi.EndOfFile.QuadPart;
    u8 *buf = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, UMI_TAG);
    if (!buf) { ZwClose(h); return NULL; }
    st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sz, NULL, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(st) || iosb.Information != sz) {
        ExFreePoolWithTag(buf, UMI_TAG);
        return NULL;
    }
    *out_size = sz;
    return buf;
}

void umi_free_fw(u8 *buf)
{
    if (buf) ExFreePoolWithTag(buf, UMI_TAG);
}

static volatile BOOLEAN g_StopThread = FALSE;
static PKTHREAD g_WorkerThread = NULL;

static VOID SensorWorkerThread(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    /* Wait briefly so umi-audio ADSP boot finishes QMP/RPMh first */
    umi_sleep_ms(1500);

    umi_sns_init();
    umi_slpi_set_qrtr_rx(umi_sns_on_qrtr);
    umi_slpi_set_fastrpc_rx(umi_fastrpc_on_rx);
    if (!umi_slpi_start()) {
        umi_log("slpi: umi_slpi_start FAILED\n");
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }

    umi_sns_publish_tftp();
    umi_log("slpi: worker thread pumping QRTR + FastRPC + QMI sensors...\n");

    while (!g_StopThread) {
        umi_slpi_pump();
        umi_fastrpc_try_start();
        umi_sns_tick();
        umi_sleep_ms(10);
    }

    umi_log("slpi: worker thread stopping\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS EvtDevicePrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);

    umi_log_init();
    umi_log("=== umi-sensors.sys v1.0.0.14 PrepareHardware (SLPI + FastRPC + QMI + 6-Sensor HUTRR39 + Mag & Compass) ===\n");

    umi_hid_init(Device);

    g_StopThread = FALSE;
    HANDLE hThread = NULL;
    NTSTATUS st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, NULL, NULL, NULL, SensorWorkerThread, NULL);
    if (NT_SUCCESS(st) && hThread) {
        ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&g_WorkerThread, NULL);
        ZwClose(hThread);
    } else {
        umi_log("slpi: PsCreateSystemThread failed 0x%x\n", (u32)st);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Translated);
    g_StopThread = TRUE;
    if (g_WorkerThread) {
        KeWaitForSingleObject(g_WorkerThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_WorkerThread);
        g_WorkerThread = NULL;
    }
    umi_hid_stop();
    umi_fastrpc_stop();
    umi_slpi_stop();
    umi_log_close();
    return STATUS_SUCCESS;
}

static NTSTATUS EvtDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDFDEVICE dev = NULL;
    return WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &dev);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDriverDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
