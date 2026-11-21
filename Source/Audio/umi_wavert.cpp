/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Xiaomi Mi 10 (UMI) Native Windows 11 ARM64 PortCls WaveRT Stereo Speaker Driver */
#define INITGUID
#include <ntddk.h>
#include <ntstrsafe.h>
#include <portcls.h>
#define _NEW_DELETE_OPERATORS_
#include <stdunk.h>
#include "umi_hw.h"

/* ---- Logging & Firmware Loader (shared with umi_adsp.c, umi_cs35l41.c, umi_sound.c) ---- */
static KMUTEX g_log_lock;
static BOOLEAN g_log_ready = FALSE;

extern "C" void umi_log_init(void)
{
    KeInitializeMutex(&g_log_lock, 0);
    UNICODE_STRING path;
    RtlInitUnicodeString(&path, L"\\SystemRoot\\Temp\\umi-audio.log");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE fh = NULL;
    IO_STATUS_BLOCK iosb;
    if (NT_SUCCESS(ZwCreateFile(&fh, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                                FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OVERWRITE_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        ZwClose(fh);
        g_log_ready = TRUE;
    }
}

extern "C" void umi_log_close(void) { g_log_ready = FALSE; }

extern "C" void umi_log(const char *fmt, ...)
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
        RtlInitUnicodeString(&path, L"\\SystemRoot\\Temp\\umi-audio.log");
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        HANDLE fh = NULL;
        IO_STATUS_BLOCK iosb;
        if (NT_SUCCESS(ZwCreateFile(&fh, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                                    FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
            ZwWriteFile(fh, NULL, NULL, NULL, &iosb, buf, (ULONG)len, NULL, NULL);
            ZwClose(fh);
        }
        KeReleaseMutex(&g_log_lock, FALSE);
    }
}

extern "C" u8 *umi_load_fw(const char *name, u32 *out_size)
{
    if (out_size) *out_size = 0;
    WCHAR wpath[160];
    static const WCHAR prefix[] = L"\\SystemRoot\\System32\\drivers\\umi-audio\\";
    SIZE_T p = 0;
    while (prefix[p]) { wpath[p] = prefix[p]; p++; }
    SIZE_T i = 0;
    while (name[i] && p + 1 < 160) { wpath[p++] = (WCHAR)(u8)name[i++]; }
    wpath[p] = 0;

    UNICODE_STRING ustr;
    RtlInitUnicodeString(&ustr, wpath);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &ustr, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE fh = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = ZwCreateFile(&fh, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                               FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) {
        umi_log("fw: open %s failed 0x%x\n", name, (u32)st);
        return NULL;
    }
    FILE_STANDARD_INFORMATION fsi;
    st = ZwQueryInformationFile(fh, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (!NT_SUCCESS(st) || fsi.EndOfFile.QuadPart <= 0 || fsi.EndOfFile.QuadPart > 16 * 1024 * 1024) {
        ZwClose(fh);
        return NULL;
    }
    u32 sz = (u32)fsi.EndOfFile.QuadPart;
    u8 *buf = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, UMI_TAG);
    if (!buf) { ZwClose(fh); return NULL; }
    st = ZwReadFile(fh, NULL, NULL, NULL, &iosb, buf, sz, NULL, NULL);
    ZwClose(fh);
    if (!NT_SUCCESS(st) || iosb.Information != sz) {
        ExFreePoolWithTag(buf, UMI_TAG);
        return NULL;
    }
    if (out_size) *out_size = sz;
    return buf;
}

extern "C" void umi_free_fw(u8 *buf)
{
    if (buf) ExFreePoolWithTag(buf, UMI_TAG);
}

/* Placement / kernel new & sized delete (unsized operator delete is provided by stdunk.lib) */
void *__cdecl operator new(SIZE_T size)
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, UMI_TAG);
}
void __cdecl operator delete(void *p, SIZE_T) noexcept
{
    if (p) ExFreePoolWithTag(p, UMI_TAG);
}

/* ---- Shared Volume/Mute State ---- */
static LONG g_master_vol_db = -16 * 65536; /* -16 dB default */
static BOOL g_master_mute   = FALSE;

/* ---- WaveRT Stream ---- */
class CUmiWaveRTStream : public IMiniportWaveRTStream, public CUnknown
{
    u8      *m_buf;
    u32      m_buf_size;
    KSSTATE  m_state;

public:
    DECLARE_STD_UNKNOWN();
    CUmiWaveRTStream(PUNKNOWN outer) : CUnknown(outer), m_buf(NULL), m_buf_size(0), m_state(KSSTATE_STOP) {}
    ~CUmiWaveRTStream()
    {
        umi_sound_stop_playback();
        if (m_buf) { ExFreePoolWithTag(m_buf, UMI_TAG); m_buf = NULL; }
    }

    STDMETHODIMP_(NTSTATUS) AllocateAudioBuffer(
        _In_ ULONG RequestedSize,
        _Out_ PMDL *AudioBufferMdl,
        _Out_ ULONG *ActualSize,
        _Out_ ULONG *OffsetFromFirstPage,
        _Out_ MEMORY_CACHING_TYPE *CacheType) override
    {
        ULONG sz = (RequestedSize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (sz < 16384) sz = 16384;
        if (sz > 65536) sz = 65536;
        u8 *buf = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, UMI_TAG);
        if (!buf) return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(buf, sz);
        PMDL mdl = IoAllocateMdl(buf, sz, FALSE, FALSE, NULL);
        if (!mdl) { ExFreePoolWithTag(buf, UMI_TAG); return STATUS_INSUFFICIENT_RESOURCES; }
        MmBuildMdlForNonPagedPool(mdl);
        m_buf = buf;
        m_buf_size = sz;
        *AudioBufferMdl = mdl;
        *ActualSize = sz;
        *OffsetFromFirstPage = 0;
        *CacheType = MmCached;
        umi_log("wavert: AllocateAudioBuffer req=%u actual=%u\n", RequestedSize, sz);
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(VOID) FreeAudioBuffer(_In_opt_ PMDL AudioBufferMdl, _In_ ULONG BufferSize) override
    {
        UNREFERENCED_PARAMETER(BufferSize);
        umi_sound_stop_playback();
        if (AudioBufferMdl) IoFreeMdl(AudioBufferMdl);
        if (m_buf) { ExFreePoolWithTag(m_buf, UMI_TAG); m_buf = NULL; m_buf_size = 0; }
    }

    STDMETHODIMP_(VOID) GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY *hwLatency) override
    {
        hwLatency->FifoSize = 3840; /* 20 ms */
        hwLatency->ChipsetDelay = 0;
        hwLatency->CodecDelay = 10;
    }

    STDMETHODIMP_(NTSTATUS) GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER *Register) override
    {
        UNREFERENCED_PARAMETER(Register);
        return STATUS_NOT_SUPPORTED;
    }

    STDMETHODIMP_(NTSTATUS) GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER *Register) override
    {
        UNREFERENCED_PARAMETER(Register);
        return STATUS_NOT_SUPPORTED;
    }

    STDMETHODIMP_(NTSTATUS) SetFormat(_In_ PKSDATAFORMAT DataFormat) override
    {
        UNREFERENCED_PARAMETER(DataFormat);
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) SetState(_In_ KSSTATE State) override
    {
        umi_log("wavert: SetState %u -> %u\n", (u32)m_state, (u32)State);
        if (State == KSSTATE_RUN && m_state != KSSTATE_RUN) {
            if (!m_buf || !m_buf_size) return STATUS_INVALID_DEVICE_STATE;
            if (!umi_sound_start_playback(m_buf, m_buf_size)) return STATUS_UNSUCCESSFUL;
        } else if (State != KSSTATE_RUN && m_state == KSSTATE_RUN) {
            umi_sound_stop_playback();
        }
        m_state = State;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) GetPosition(_Out_ PKSAUDIO_POSITION Position) override
    {
        u64 pos = umi_sound_get_position();
        Position->PlayOffset = pos;
        Position->WriteOffset = m_buf_size ? ((pos + 3840) % m_buf_size) : 0;
        return STATUS_SUCCESS;
    }
};

STDMETHODIMP CUmiWaveRTStream::NonDelegatingQueryInterface(REFIID riid, PVOID *ppv)
{
    if (IsEqualGUIDAligned(riid, IID_IUnknown) || IsEqualGUIDAligned(riid, IID_IMiniportWaveRTStream)) {
        *ppv = (PVOID)(IMiniportWaveRTStream *)this;
        AddRef();
        return STATUS_SUCCESS;
    }
    *ppv = NULL;
    return STATUS_INVALID_PARAMETER;
}

/* ---- WaveRT Miniport ---- */
static KSDATARANGE_AUDIO g_WavePinDataRanges[] = {
    {
        {
            sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        2, 16, 16, 48000, 48000
    }
};
static PKSDATARANGE g_WavePinDataRangePtrs[] = { (PKSDATARANGE)&g_WavePinDataRanges[0] };

static KSDATARANGE g_BridgeDataRanges[] = {
    {
        sizeof(KSDATARANGE), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};
static PKSDATARANGE g_BridgeDataRangePtrs[] = { &g_BridgeDataRanges[0] };

static PCPIN_DESCRIPTOR g_WavePins[] = {
    /* Pin 0: Render stream sink */
    {
        1, 1, 0, NULL,
        { 0, NULL, 0, NULL, 1, g_WavePinDataRangePtrs, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, NULL, 0 }
    },
    /* Pin 1: Bridge to topology */
    {
        0, 0, 0, NULL,
        { 0, NULL, 0, NULL, 1, g_BridgeDataRangePtrs, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 }
    }
};

static PCNODE_DESCRIPTOR g_WaveNodes[] = {
    { 0, NULL, &KSNODETYPE_DAC, NULL }
};

static PCCONNECTION_DESCRIPTOR g_WaveConns[] = {
    { PCFILTER_NODE, 0, 0, 1 },
    { 0, 0, PCFILTER_NODE, 1 }
};

static PCFILTER_DESCRIPTOR g_WaveFilterDesc = {
    0, NULL,
    sizeof(PCPIN_DESCRIPTOR), 2, g_WavePins,
    sizeof(PCNODE_DESCRIPTOR), 1, g_WaveNodes,
    2, g_WaveConns, 0, NULL
};

class CUmiWaveRT : public IMiniportWaveRT, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    CUmiWaveRT(PUNKNOWN outer) : CUnknown(outer) {}
    ~CUmiWaveRT() {}

    STDMETHODIMP_(NTSTATUS) GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description) override
    {
        *Description = &g_WaveFilterDesc;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(
        _In_ ULONG PinId, _In_ PKSDATARANGE ClientDataRange, _In_ PKSDATARANGE MyDataRange,
        _In_ ULONG OutputBufferLength, _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
        _Out_ PULONG ResultantFormatLength) override
    {
        UNREFERENCED_PARAMETER(PinId);
        UNREFERENCED_PARAMETER(ClientDataRange);
        UNREFERENCED_PARAMETER(MyDataRange);
        *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
        if (OutputBufferLength == 0) return STATUS_BUFFER_OVERFLOW;
        if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEX)) return STATUS_BUFFER_TOO_SMALL;
        PKSDATAFORMAT_WAVEFORMATEX fmt = (PKSDATAFORMAT_WAVEFORMATEX)ResultantFormat;
        RtlZeroMemory(fmt, sizeof(*fmt));
        fmt->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
        fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        fmt->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        fmt->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        fmt->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
        fmt->WaveFormatEx.nChannels = 2;
        fmt->WaveFormatEx.nSamplesPerSec = 48000;
        fmt->WaveFormatEx.wBitsPerSample = 16;
        fmt->WaveFormatEx.nBlockAlign = 4;
        fmt->WaveFormatEx.nAvgBytesPerSec = 48000 * 4;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) Init(_In_ PUNKNOWN UnknownAdapter, _In_ PRESOURCELIST ResourceList, _In_ PPORTWAVERT Port) override
    {
        UNREFERENCED_PARAMETER(UnknownAdapter);
        UNREFERENCED_PARAMETER(ResourceList);
        UNREFERENCED_PARAMETER(Port);
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) NewStream(
        _Out_ PMINIPORTWAVERTSTREAM *Stream, _In_ PPORTWAVERTSTREAM PortStream,
        _In_ ULONG Pin, _In_ BOOLEAN Capture, _In_ PKSDATAFORMAT DataFormat) override
    {
        UNREFERENCED_PARAMETER(PortStream);
        UNREFERENCED_PARAMETER(Pin);
        UNREFERENCED_PARAMETER(DataFormat);
        if (Capture) return STATUS_NOT_SUPPORTED;
        CUmiWaveRTStream *s = new CUmiWaveRTStream(NULL);
        if (!s) return STATUS_INSUFFICIENT_RESOURCES;
        s->AddRef();
        *Stream = (PMINIPORTWAVERTSTREAM)s;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DeviceDescription) override
    {
        RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));
        DeviceDescription->Version = DEVICE_DESCRIPTION_VERSION;
        DeviceDescription->Master = FALSE;
        DeviceDescription->ScatterGather = FALSE;
        DeviceDescription->Dma32BitAddresses = TRUE;
        DeviceDescription->InterfaceType = Internal;
        DeviceDescription->MaximumLength = 65536;
        return STATUS_SUCCESS;
    }
};

STDMETHODIMP CUmiWaveRT::NonDelegatingQueryInterface(REFIID riid, PVOID *ppv)
{
    if (IsEqualGUIDAligned(riid, IID_IUnknown) || IsEqualGUIDAligned(riid, IID_IMiniport) ||
        IsEqualGUIDAligned(riid, IID_IMiniportWaveRT)) {
        *ppv = (PVOID)(IMiniportWaveRT *)this;
        AddRef();
        return STATUS_SUCCESS;
    }
    *ppv = NULL;
    return STATUS_INVALID_PARAMETER;
}

/* ---- Topology Miniport (Volume + Mute + Internal Speaker Jack) ---- */
static NTSTATUS PropVolumeHandler(_In_ PPCPROPERTY_REQUEST req)
{
    if (req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (req->ValueSize < sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG)) {
            req->ValueSize = sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG);
            return STATUS_BUFFER_OVERFLOW;
        }
        PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)req->Value;
        desc->AccessFlags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
        desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG);
        desc->PropTypeSet.Set = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id = VT_I4;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount = 1;
        desc->Reserved = 0;
        PKSPROPERTY_MEMBERSHEADER hdr = (PKSPROPERTY_MEMBERSHEADER)(desc + 1);
        hdr->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
        hdr->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
        hdr->MembersCount = 1;
        hdr->Flags = 0;
        PKSPROPERTY_STEPPING_LONG rng = (PKSPROPERTY_STEPPING_LONG)(hdr + 1);
        rng->SteppingDelta = 65536 / 2; /* 0.5 dB */
        rng->Reserved = 0;
        rng->Bounds.SignedMinimum = -96 * 65536;
        rng->Bounds.SignedMaximum = 0;
        req->ValueSize = desc->DescriptionSize;
        return STATUS_SUCCESS;
    }
    if (req->ValueSize < sizeof(LONG)) { req->ValueSize = sizeof(LONG); return STATUS_BUFFER_TOO_SMALL; }
    if (req->Verb & KSPROPERTY_TYPE_GET) {
        *(PLONG)req->Value = g_master_vol_db;
        req->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }
    if (req->Verb & KSPROPERTY_TYPE_SET) {
        g_master_vol_db = *(PLONG)req->Value;
        umi_sound_set_volume(g_master_vol_db, g_master_mute ? TRUE : FALSE);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS PropMuteHandler(_In_ PPCPROPERTY_REQUEST req)
{
    if (req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (req->ValueSize < sizeof(ULONG)) { req->ValueSize = sizeof(ULONG); return STATUS_BUFFER_OVERFLOW; }
        *(PULONG)req->Value = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
        req->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    if (req->ValueSize < sizeof(BOOL)) { req->ValueSize = sizeof(BOOL); return STATUS_BUFFER_TOO_SMALL; }
    if (req->Verb & KSPROPERTY_TYPE_GET) {
        *(PBOOL)req->Value = g_master_mute;
        req->ValueSize = sizeof(BOOL);
        return STATUS_SUCCESS;
    }
    if (req->Verb & KSPROPERTY_TYPE_SET) {
        g_master_mute = *(PBOOL)req->Value;
        umi_sound_set_volume(g_master_vol_db, g_master_mute ? TRUE : FALSE);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS PropCpuResHandler(_In_ PPCPROPERTY_REQUEST req)
{
    if (req->Verb & KSPROPERTY_TYPE_GET) {
        if (req->ValueSize < sizeof(ULONG)) { req->ValueSize = sizeof(ULONG); return STATUS_BUFFER_TOO_SMALL; }
        *(PULONG)req->Value = KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU;
        req->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS PropJackHandler(_In_ PPCPROPERTY_REQUEST req)
{
    if (!(req->Verb & KSPROPERTY_TYPE_GET)) return STATUS_INVALID_DEVICE_REQUEST;
    ULONG needed = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);
    if (req->ValueSize == 0) { req->ValueSize = needed; return STATUS_BUFFER_OVERFLOW; }
    if (req->ValueSize < needed) { req->ValueSize = needed; return STATUS_BUFFER_TOO_SMALL; }
    PKSMULTIPLE_ITEM mi = (PKSMULTIPLE_ITEM)req->Value;
    mi->Size = needed;
    mi->Count = 1;
    PKSJACK_DESCRIPTION jd = (PKSJACK_DESCRIPTION)(mi + 1);
    RtlZeroMemory(jd, sizeof(*jd));
    jd->ChannelMapping = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    jd->Color = 0;
    jd->ConnectionType = eConnTypeAtapiInternal;
    jd->GeoLocation = eGeoLocInsideMobileLid;
    jd->GenLocation = eGenLocPrimaryBox;
    jd->PortConnection = ePortConnIntegratedDevice;
    jd->IsConnected = TRUE;
    req->ValueSize = needed;
    return STATUS_SUCCESS;
}

static PCPROPERTY_ITEM g_VolProps[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, PropVolumeHandler },
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_CPU_RESOURCES, KSPROPERTY_TYPE_GET, PropCpuResHandler }
};
static PCAUTOMATION_TABLE g_VolAuto = { sizeof(PCPROPERTY_ITEM), 2, g_VolProps, 0, 0, NULL, 0, 0, NULL };

static PCPROPERTY_ITEM g_MuteProps[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_MUTE, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, PropMuteHandler },
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_CPU_RESOURCES, KSPROPERTY_TYPE_GET, PropCpuResHandler }
};
static PCAUTOMATION_TABLE g_MuteAuto = { sizeof(PCPROPERTY_ITEM), 2, g_MuteProps, 0, 0, NULL, 0, 0, NULL };

static PCPROPERTY_ITEM g_TopoFilterProps[] = {
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION, KSPROPERTY_TYPE_GET, PropJackHandler }
};
static PCAUTOMATION_TABLE g_TopoFilterAuto = { sizeof(PCPROPERTY_ITEM), 1, g_TopoFilterProps, 0, 0, NULL, 0, 0, NULL };

static PCPIN_DESCRIPTOR g_TopoPins[] = {
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_BridgeDataRangePtrs, KSPIN_DATAFLOW_IN,  KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 } },
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_BridgeDataRangePtrs, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_SPEAKER, NULL, 0 } }
};

static PCNODE_DESCRIPTOR g_TopoNodes[] = {
    { 0, &g_VolAuto,  &KSNODETYPE_VOLUME, NULL },
    { 0, &g_MuteAuto, &KSNODETYPE_MUTE,   NULL }
};

static PCCONNECTION_DESCRIPTOR g_TopoConns[] = {
    { PCFILTER_NODE, 0, 0, 1 },
    { 0, 0, 1, 1 },
    { 1, 0, PCFILTER_NODE, 1 }
};

static PCFILTER_DESCRIPTOR g_TopoFilterDesc = {
    0, &g_TopoFilterAuto,
    sizeof(PCPIN_DESCRIPTOR), 2, g_TopoPins,
    sizeof(PCNODE_DESCRIPTOR), 2, g_TopoNodes,
    3, g_TopoConns, 0, NULL
};

class CUmiTopology : public IMiniportTopology, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    CUmiTopology(PUNKNOWN outer) : CUnknown(outer) {}
    ~CUmiTopology() {}

    STDMETHODIMP_(NTSTATUS) GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description) override
    {
        *Description = &g_TopoFilterDesc;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(
        _In_ ULONG, _In_ PKSDATARANGE, _In_ PKSDATARANGE, _In_ ULONG,
        _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID, _Out_ PULONG) override
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    STDMETHODIMP_(NTSTATUS) Init(_In_ PUNKNOWN, _In_ PRESOURCELIST, _In_ PPORTTOPOLOGY) override
    {
        return STATUS_SUCCESS;
    }
};

STDMETHODIMP CUmiTopology::NonDelegatingQueryInterface(REFIID riid, PVOID *ppv)
{
    if (IsEqualGUIDAligned(riid, IID_IUnknown) || IsEqualGUIDAligned(riid, IID_IMiniport) ||
        IsEqualGUIDAligned(riid, IID_IMiniportTopology)) {
        *ppv = (PVOID)(IMiniportTopology *)this;
        AddRef();
        return STATUS_SUCCESS;
    }
    *ppv = NULL;
    return STATUS_INVALID_PARAMETER;
}

/* ---- PortCls Adapter Lifecycle ---- */
static NTSTATUS StartDevice(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PRESOURCELIST ResourceList)
{
    UNREFERENCED_PARAMETER(Irp);
    umi_log("=== UMI Audio WaveRT StartDevice ===\n");
    if (!umi_sound_open()) {
        umi_log("wavert: umi_sound_open FAILED\n");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    PPORT pWavePort = NULL, pTopoPort = NULL;
    CUmiWaveRT *waveMini = NULL;
    CUmiTopology *topoMini = NULL;
    NTSTATUS st = PcNewPort(&pWavePort, CLSID_PortWaveRT);
    if (!NT_SUCCESS(st)) goto fail;
    waveMini = new CUmiWaveRT(NULL);
    if (!waveMini) { st = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
    waveMini->AddRef();
    st = pWavePort->Init(DeviceObject, NULL, (PMINIPORT)waveMini, NULL, ResourceList);
    waveMini->Release();
    if (!NT_SUCCESS(st)) goto fail;
    st = PcRegisterSubdevice(DeviceObject, (PWSTR)L"Wave", pWavePort);
    if (!NT_SUCCESS(st)) goto fail;

    st = PcNewPort(&pTopoPort, CLSID_PortTopology);
    if (!NT_SUCCESS(st)) goto fail;
    topoMini = new CUmiTopology(NULL);
    if (!topoMini) { st = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
    topoMini->AddRef();
    st = pTopoPort->Init(DeviceObject, NULL, (PMINIPORT)topoMini, NULL, ResourceList);
    topoMini->Release();
    if (!NT_SUCCESS(st)) goto fail;
    st = PcRegisterSubdevice(DeviceObject, (PWSTR)L"Topology", pTopoPort);
    if (!NT_SUCCESS(st)) goto fail;

    st = PcRegisterPhysicalConnection(DeviceObject, (PUNKNOWN)pWavePort, 1, (PUNKNOWN)pTopoPort, 0);
    pWavePort->Release();
    pTopoPort->Release();
    umi_log("wavert: StartDevice complete st=0x%x\n", (u32)st);
    return st;

fail:
    if (pWavePort) pWavePort->Release();
    if (pTopoPort) pTopoPort->Release();
    umi_sound_close();
    umi_log("wavert: StartDevice failed st=0x%x\n", (u32)st);
    return st;
}

static NTSTATUS AddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, StartDevice, 2, 0);
}

static PDRIVER_DISPATCH g_orig_pnp = NULL;
static NTSTATUS UmiPnpDispatch(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    if (sl->MinorFunction == IRP_MN_REMOVE_DEVICE || sl->MinorFunction == IRP_MN_STOP_DEVICE ||
        sl->MinorFunction == IRP_MN_SURPRISE_REMOVAL) {
        umi_sound_close();
    }
    return g_orig_pnp(DeviceObject, Irp);
}

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    umi_log_init();
    NTSTATUS st = PcInitializeAdapterDriver(DriverObject, RegistryPath, AddDevice);
    if (NT_SUCCESS(st)) {
        g_orig_pnp = DriverObject->MajorFunction[IRP_MJ_PNP];
        DriverObject->MajorFunction[IRP_MJ_PNP] = UmiPnpDispatch;
    }
    return st;
}
