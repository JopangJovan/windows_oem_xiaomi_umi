/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Xiaomi Mi 10 (umi) SM8250 SLPI FastRPC (SENSORS_PD + adsp_listener_next2 + apps_std) */

#include "umi_sensors.h"

typedef struct _UMI_FILE_DIR_INFO {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
} UMI_FILE_DIR_INFO;

NTSYSAPI NTSTATUS NTAPI ZwQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass, BOOLEAN ReturnSingleEntry,
    PUNICODE_STRING FileName, BOOLEAN RestartScan);

#define SMMU_PHYS               0x15000000ULL
#define SMMU_SIZE               0x000D0000u
#define SLPI_FASTRPC_SID        0x0541u /* compute-cb@1 in sm8250.dtsi */
#define SLPI_FASTRPC_SID_SHIFT  (1ULL << 32)

#define FASTRPC_INIT_HANDLE     1u
#define FASTRPC_LISTENER_HANDLE 3u
#define SENSORS_PD              2u
#define APPS_STD_HANDLE         0x100u
#define SNS_REG_HANDLE          0x101u

#define FASTRPC_MAX_FDLIST      16u
#define FASTRPC_MAX_CRCLIST     64u
#define FASTRPC_ALIGN           128u
#define FRPC_BUF_SZ             131072u
#define LISTENER_BUF_SZ         65536u

#pragma pack(push, 1)
typedef struct {
    s32 client_id;
    s32 tid;
    u64 ctx;
    u32 handle;
    u32 sc;
    u64 addr;
    u64 size;
} FASTRPC_MSG;

typedef struct {
    u64 ctx;
    s32 retval;
} FASTRPC_RSP;

typedef struct { u64 pv; u64 len; } FASTRPC_REMOTE_BUF;
typedef struct { u32 num; u32 pgidx; } FASTRPC_INVOKE_BUF;
typedef struct { u64 addr; u64 size; } FASTRPC_PHY_PAGE;

typedef struct {
    u64 tsz, dev, ino;
    u32 mode, nlink;
    u64 rdev, size;
    s64 atime, atimensec, mtime, mtimensec, ctime, ctimensec;
} APPS_STD_STAT;
#pragma pack(pop)

typedef struct { void *ptr; u32 len; } FRPC_ARG;

/* Open file table for apps_std */
#define MAX_FILES 32
typedef struct {
    BOOLEAN used;
    u8     *data;
    u32     size;
    u32     pos;
    BOOLEAN eof;
} STD_FILE;
static STD_FILE g_files[MAX_FILES];

/* Open directory table for apps_std */
#define MAX_DIRS 8
typedef struct {
    BOOLEAN used;
    HANDLE  hDir;
    BOOLEAN first;
} STD_DIR;
static STD_DIR g_dirs[MAX_DIRS];

static u8  *g_frpc_va = NULL;
static u64  g_frpc_pa = 0;
static u64  g_frpc_dma = 0;
static u32  g_frpc_seq = 1;
static volatile BOOLEAN g_rsp_got = FALSE;
static u64  g_rsp_ctx = 0;
static s32  g_rsp_val = 0;

/* Async listener state machine */
static BOOLEAN g_frpc_started = FALSE;
static BOOLEAN g_listener_ready = FALSE;
static BOOLEAN g_next2_pending = FALSE;
static u64     g_next2_ctx = 0;
static u32     g_next2_in_scalars[4];
static u32     g_next2_out_scalars[4];
static u8     *g_next2_in_buf = NULL;  /* prevbufs sent TO DSP */
static u8     *g_next2_out_buf = NULL; /* inBufs received FROM DSP */
static u32     g_next2_out_va_off[2];  /* offsets in g_frpc_va for out_scalars & out_buf */

static BOOLEAN smmu_setup_slpi_bypass(void)
{
    volatile u8 *smmu = (volatile u8 *)umi_map_io(SMMU_PHYS, SMMU_SIZE, FALSE);
    if (!smmu) return FALSE;
    u32 idr0 = umi_rd32(smmu, 0x020), idr1 = umi_rd32(smmu, 0x024);
    u32 matches = idr0 & 0xffu, num_cb = idr1 & 0xffu;
    u32 page_sz = (idr1 >> 31) ? 0x10000u : 0x1000u;
    u32 num_pages = 2u << ((idr1 >> 28) & 7u);
    BOOLEAN used_cb[128];
    RtlZeroMemory(used_cb, sizeof(used_cb));
    u32 free_idx = 0xFFFFFFFFu;
    BOOLEAN covered = FALSE;
    for (u32 n = 0; n < matches && n < 128u; n++) {
        u32 smr = umi_rd32(smmu, 0x800 + 4 * n), s2cr = umi_rd32(smmu, 0xc00 + 4 * n);
        if (smr & UMI_BIT(31)) {
            if ((s2cr & 0xffu) < 128u) used_cb[s2cr & 0xffu] = TRUE;
            u32 id = smr & 0x7fffu, mask = (smr >> 16) & 0x7fffu;
            if (((id ^ SLPI_FASTRPC_SID) & ~mask) == 0) covered = TRUE;
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
            umi_wr32(smmu, 0x800u + 4u * free_idx, 0x80000000u | SLPI_FASTRPC_SID);
            u32 rb_smr = umi_rd32(smmu, 0x800u + 4u * free_idx);
            u32 rb_s2cr = umi_rd32(smmu, 0xc00u + 4u * free_idx);
            umi_log("frpc: SMMU SID 0x%x idx=%u cb=%u SMR=0x%x S2CR=0x%x\n",
                    SLPI_FASTRPC_SID, free_idx, free_cb, rb_smr, rb_s2cr);
            ok = (rb_smr & UMI_BIT(31)) != 0;
        }
    }
    umi_unmap_io((void *)smmu, SMMU_SIZE);
    return ok;
}

static BOOLEAN str_starts_with(const char *s, const char *pfx)
{
    while (*pfx) { if (*s++ != *pfx++) return FALSE; }
    return TRUE;
}

/* Map SLPI UNIX path to Windows NT path under \SystemRoot\System32\drivers\umi-sensors\persist_sensors\ */
static void map_dsp_path(const char *unix_path, WCHAR *wout, u32 wcap)
{
    const char *rel = unix_path;
    const char *sub = "";
    if (str_starts_with(unix_path, "/mnt/vendor/persist/sensors/")) {
        rel = unix_path + 28;
    } else if (str_starts_with(unix_path, "/mnt/vendor/persist/sensors")) {
        rel = "";
    } else if (str_starts_with(unix_path, "/persist/sensors/")) {
        rel = unix_path + 17;
    } else if (str_starts_with(unix_path, "/persist/sensors")) {
        rel = "";
    } else if (str_starts_with(unix_path, "/vendor/etc/sensors/config/")) {
        sub = "registry\\config\\";
        rel = unix_path + 27;
    } else if (str_starts_with(unix_path, "/vendor/etc/sensors/config")) {
        sub = "registry\\config";
        rel = "";
    } else if (str_starts_with(unix_path, "/vendor/etc/sensors/")) {
        rel = unix_path + 20;
    } else if (unix_path[0] == '/') {
        /* Fallback: strip up to last '/' and look in registry/registry/ */
        const char *b = unix_path;
        for (const char *p = unix_path; *p; p++) if (*p == '/') b = p + 1;
        sub = "registry\\registry\\";
        rel = b;
    } else {
        sub = "registry\\registry\\";
    }

    const WCHAR *base = L"\\SystemRoot\\System32\\drivers\\umi-sensors\\persist_sensors\\";
    u32 idx = 0;
    while (base[idx] && idx + 1 < wcap) { wout[idx] = base[idx]; idx++; }
    while (*sub && idx + 1 < wcap) { wout[idx++] = (WCHAR)*sub++; }
    while (*rel && idx + 1 < wcap) {
        char c = *rel++;
        wout[idx++] = (c == '/') ? L'\\' : (WCHAR)c;
    }
    /* Strip trailing backslash if not root */
    if (idx > 1 && wout[idx - 1] == L'\\') idx--;
    wout[idx] = 0;
}

static u8 *load_nt_file(const WCHAR *wpath, u32 *out_sz)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    FILE_STANDARD_INFORMATION fsi;
    *out_sz = 0;

    RtlInitUnicodeString(&path, wpath);
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS st = ZwCreateFile(&h, FILE_GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                               FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) return NULL;
    st = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (!NT_SUCCESS(st) || fsi.EndOfFile.QuadPart < 0 || fsi.EndOfFile.QuadPart > 4 * 1024 * 1024) {
        ZwClose(h); return NULL;
    }
    u32 sz = (u32)fsi.EndOfFile.QuadPart;
    u8 *buf = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sz ? sz : 1, UMI_TAG);
    if (!buf) { ZwClose(h); return NULL; }
    if (sz > 0) {
        st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sz, NULL, NULL);
        if (!NT_SUCCESS(st)) { ExFreePoolWithTag(buf, UMI_TAG); ZwClose(h); return NULL; }
        sz = (u32)iosb.Information;
    }
    ZwClose(h);
    *out_sz = sz;
    return buf;
}

static BOOLEAN stat_nt_path(const WCHAR *wpath, BOOLEAN *is_dir, u64 *size)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    FILE_STANDARD_INFORMATION fsi;
    RtlInitUnicodeString(&path, wpath);
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS st = ZwCreateFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb, NULL,
                               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                               FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) return FALSE;
    st = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    ZwClose(h);
    if (!NT_SUCCESS(st)) return FALSE;
    *is_dir = fsi.Directory;
    *size = (u64)fsi.EndOfFile.QuadPart;
    return TRUE;
}

static const char *lookup_virtual_sysfs(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    if (RtlCompareMemory(b, "hw_platform", 12) == 12) return "UMI\n";
    if (RtlCompareMemory(b, "soc_id", 7) == 7) return "356\n";
    if (RtlCompareMemory(b, "platform_subtype_id", 20) == 20) return "0\n";
    if (RtlCompareMemory(b, "platform_version", 17) == 17) return "1638400\n";
    if (RtlCompareMemory(b, "revision", 9) == 9) return "2.1\n";
    if (RtlCompareMemory(b, "max_brightness", 15) == 15) return "2047\n";
    if (RtlCompareMemory(b, "brightness", 11) == 11) return "1024\n";
    if (RtlCompareMemory(b, "dynamic_fps", 12) == 12) return "90\n";
    if (RtlCompareMemory(b, "panel_info", 11) == 11) return "panel_name=dsi_j2_38_0c_0a_mp_dsc_cmd_display\n";
    if (RtlCompareMemory(b, "sns_reg_version", 16) == 16) return "version=4\n";
    return NULL;
}

static u32 g_fopen_ok = 0, g_fopen_fail = 0, g_stat_ok = 0, g_stat_fail = 0;

static s32 std_open_file(const char *unix_path, const char *mode)
{
    BOOLEAN is_write = (mode && (mode[0] == 'w' || mode[0] == 'a'));
    u32 fsz = 0;
    u8 *fdata = NULL;
    const char *virt = is_write ? NULL : lookup_virtual_sysfs(unix_path);
    if (virt) {
        while (virt[fsz]) fsz++;
        fdata = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, fsz + 1, UMI_TAG);
        if (fdata) RtlCopyMemory(fdata, virt, fsz);
    } else if (!is_write) {
        WCHAR wp[260];
        map_dsp_path(unix_path, wp, 260);
        fdata = load_nt_file(wp, &fsz);
        if (!fdata) {
            const char *b = unix_path;
            for (const char *p = unix_path; *p; p++) if (*p == '/') b = p + 1;
            map_dsp_path(b, wp, 260);
            fdata = load_nt_file(wp, &fsz);
        }
    }
    if (!is_write && !fdata) {
        g_fopen_fail++;
        if (g_fopen_fail <= 15) umi_log("apps_std: fopen FAIL '%s' mode='%s'\n", unix_path, mode ? mode : "");
        return -1;
    }
    g_fopen_ok++;
    if (g_fopen_ok <= 10) umi_log("apps_std: fopen OK '%s' sz=%u\n", unix_path, fsz);
    for (s32 i = 1; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used = TRUE;
            g_files[i].data = fdata;
            g_files[i].size = fsz;
            g_files[i].pos  = 0;
            g_files[i].eof  = FALSE;
            return i;
        }
    }
    if (fdata) ExFreePoolWithTag(fdata, UMI_TAG);
    return -1;
}

static u32 align8(u32 v) { return (v + 7u) & ~7u; }

/* Dispatch one reverse-RPC from SLPI and pack reply into g_next2_in_buf */
static s32 dispatch_reverse_rpc(u32 handle, u32 sc, const u8 *in_raw, u32 in_raw_len, u32 *out_packed_len)
{
    u32 mid   = (sc >> 24) & 0x1Fu;
    u32 n_in  = (sc >> 16) & 0xFFu;
    u32 n_out = (sc >> 8)  & 0xFFu;
    *out_packed_len = 0;
    if (n_in > 8 || n_out > 8) return -1;

    const u8 *in_ptr[8] = { 0 };
    u32       in_len[8] = { 0 };
    u32       out_len[8] = { 0 };

    u32 pos = 0;
    for (u32 i = 0; i < n_in; i++) {
        if (pos + 4 > in_raw_len) return -1;
        RtlCopyMemory(&in_len[i], in_raw + pos, 4); pos += 4;
        if (in_len[i]) {
            pos = align8(pos);
            if (pos + in_len[i] > in_raw_len) return -1;
            in_ptr[i] = in_raw + pos;
            pos += in_len[i];
        }
    }
    for (u32 i = 0; i < n_out; i++) {
        if (pos + 4 > in_raw_len) return -1;
        RtlCopyMemory(&out_len[i], in_raw + pos, 4); pos += 4;
    }

    /* Pre-allocate output buffers inside g_next2_in_buf using listener_buf.h layout */
    u8 *out_ptr[8] = { 0 };
    u32 opos = 0;
    for (u32 i = 0; i < n_out; i++) {
        u32 l = out_len[i];
        if (opos + 4 > LISTENER_BUF_SZ) return -1;
        RtlCopyMemory(g_next2_in_buf + opos, &l, 4); opos += 4;
        if (l) {
            opos = align8(opos);
            if (opos + l > LISTENER_BUF_SZ) return -1;
            out_ptr[i] = g_next2_in_buf + opos;
            RtlZeroMemory(out_ptr[i], l);
            opos += l;
        }
    }
    *out_packed_len = opos;

    if (handle == 0) {
        if (mid == 0 && n_in >= 2 && n_out >= 1 && out_len[0] >= 8) {
            const char *name = (const char *)in_ptr[1];
            u32 h = 0; s32 dlerr = -1;
            if (name && RtlCompareMemory(name, "apps_std", 9) == 9) {
                h = APPS_STD_HANDLE; dlerr = 0;
            } else if (name && RtlCompareMemory(name, "sns_registry", 13) == 13) {
                h = SNS_REG_HANDLE; dlerr = 0;
            }
            umi_log("frpc: rev remotectl_open('%s') -> handle=0x%x dlerr=%d\n", name ? name : "", h, dlerr);
            RtlCopyMemory(out_ptr[0], &h, 4);
            RtlCopyMemory(out_ptr[0] + 4, &dlerr, 4);
            return 0;
        }
        if (mid == 1 && n_out >= 1 && out_len[0] >= 4) {
            u32 dlerr = 0;
            RtlCopyMemory(out_ptr[0], &dlerr, 4);
            return 0;
        }
        return 0;
    }

    if (handle == SNS_REG_HANDLE) {
        /* sns_registry_get_property(name, rout val): return empty string / success */
        return 0;
    }

    if (handle != APPS_STD_HANDLE) {
        umi_log("frpc: rev unknown handle=0x%x mid=%u sc=0x%x\n", handle, mid, sc);
        return -1;
    }

    /* Handle apps_std methods (from apps_std_skel.c) */
    switch (mid) {
    case 0: { /* fopen(name, mode, rout FILE) */
        const char *name = (n_in >= 2 && in_ptr[1]) ? (const char *)in_ptr[1] : "";
        const char *mode = (n_in >= 3 && in_ptr[2]) ? (const char *)in_ptr[2] : "r";
        s32 fd = std_open_file(name, mode);
        if (fd < 0) return 2; /* ENOENT */
        if (n_out >= 1 && out_len[0] >= 4) RtlCopyMemory(out_ptr[0], &fd, 4);
        return 0;
    }
    case 19: { /* fopen_with_env(env, delim, name, mode, rout FILE) */
        const char *name = (n_in >= 4 && in_ptr[3]) ? (const char *)in_ptr[3] : "";
        const char *mode = (n_in >= 5 && in_ptr[4]) ? (const char *)in_ptr[4] : "r";
        s32 fd = std_open_file(name, mode);
        if (fd < 0) return 2;
        if (n_out >= 1 && out_len[0] >= 4) RtlCopyMemory(out_ptr[0], &fd, 4);
        return 0;
    }
    case 2: /* fflush */
    case 23: /* fsync */
    case 14: /* clearerr */
        return 0;
    case 3: { /* fclose(fd) */
        s32 fd = (n_in >= 1 && in_len[0] >= 4) ? *(const s32 *)in_ptr[0] : -1;
        if (fd > 0 && fd < MAX_FILES && g_files[fd].used) {
            if (g_files[fd].data) ExFreePoolWithTag(g_files[fd].data, UMI_TAG);
            RtlZeroMemory(&g_files[fd], sizeof(STD_FILE));
        }
        return 0;
    }
    case 4: { /* fread(fd, rout buf, rout bytesRead, rout bEOF) */
        s32 fd = (n_in >= 1 && in_len[0] >= 4) ? *(const s32 *)in_ptr[0] : -1;
        if (fd <= 0 || fd >= MAX_FILES || !g_files[fd].used) return 9; /* EBADF */
        u32 req = (n_out >= 2) ? out_len[1] : 0;
        u32 avail = (g_files[fd].pos < g_files[fd].size) ? (g_files[fd].size - g_files[fd].pos) : 0;
        u32 rd = (req < avail) ? req : avail;
        if (rd && g_files[fd].data && out_ptr[1]) {
            RtlCopyMemory(out_ptr[1], g_files[fd].data + g_files[fd].pos, rd);
            g_files[fd].pos += rd;
        }
        s32 eof = (g_files[fd].pos >= g_files[fd].size) ? 1 : 0;
        g_files[fd].eof = (eof != 0);
        if (n_out >= 1 && out_len[0] >= 8) {
            RtlCopyMemory(out_ptr[0], &rd, 4);
            RtlCopyMemory(out_ptr[0] + 4, &eof, 4);
        }
        return 0;
    }
    case 5: { /* fwrite */
        u32 wlen = (n_in >= 2) ? in_len[1] : 0;
        s32 eof = 0;
        if (n_out >= 1 && out_len[0] >= 8) {
            RtlCopyMemory(out_ptr[0], &wlen, 4);
            RtlCopyMemory(out_ptr[0] + 4, &eof, 4);
        }
        return 0;
    }
    case 8: { /* ftell */
        s32 fd = (n_in >= 1 && in_len[0] >= 4) ? *(const s32 *)in_ptr[0] : -1;
        if (fd <= 0 || fd >= MAX_FILES || !g_files[fd].used) return 9;
        s32 p = (s32)g_files[fd].pos;
        if (n_out >= 1 && out_len[0] >= 4) RtlCopyMemory(out_ptr[0], &p, 4);
        return 0;
    }
    case 9: { /* fseek(fd, off, whence) */
        if (n_in < 1 || in_len[0] < 12) return 22;
        s32 fd = ((const s32 *)in_ptr[0])[0];
        s32 off = ((const s32 *)in_ptr[0])[1];
        s32 wh = ((const s32 *)in_ptr[0])[2];
        if (fd <= 0 || fd >= MAX_FILES || !g_files[fd].used) return 9;
        s64 np = (wh == 0) ? off : ((wh == 1) ? ((s64)g_files[fd].pos + off) : ((s64)g_files[fd].size + off));
        if (np < 0) np = 0;
        if (np > (s64)g_files[fd].size) np = g_files[fd].size;
        g_files[fd].pos = (u32)np;
        g_files[fd].eof = FALSE;
        return 0;
    }
    case 10: { /* flen(fd) -> u64 */
        s32 fd = (n_in >= 1 && in_len[0] >= 4) ? *(const s32 *)in_ptr[0] : -1;
        if (fd <= 0 || fd >= MAX_FILES || !g_files[fd].used) return 9;
        u64 sz = g_files[fd].size;
        if (n_out >= 1 && out_len[0] >= 8) RtlCopyMemory(out_ptr[0], &sz, 8);
        return 0;
    }
    case 11: { /* rewind */
        s32 fd = (n_in >= 1 && in_len[0] >= 4) ? *(const s32 *)in_ptr[0] : -1;
        if (fd > 0 && fd < MAX_FILES && g_files[fd].used) { g_files[fd].pos = 0; g_files[fd].eof = FALSE; }
        return 0;
    }
    case 12: { /* feof */
        s32 fd = (n_in >= 1 && in_len[0] >= 4) ? *(const s32 *)in_ptr[0] : -1;
        s32 eof = (fd > 0 && fd < MAX_FILES && g_files[fd].used && g_files[fd].eof) ? 1 : 0;
        if (n_out >= 1 && out_len[0] >= 4) RtlCopyMemory(out_ptr[0], &eof, 4);
        return 0;
    }
    case 13: { /* ferror */
        s32 err = 0;
        if (n_out >= 1 && out_len[0] >= 4) RtlCopyMemory(out_ptr[0], &err, 4);
        return 0;
    }
    case 15: { /* print_string */
        const char *s = (n_in >= 2 && in_ptr[1]) ? (const char *)in_ptr[1] : "";
        umi_log("slpi_dsp: %s\n", s);
        return 0;
    }
    case 16: { /* getenv */
        return 2; /* ENOENT */
    }
    case 22: { /* fileExists(path, rout bool) */
        const char *p = (n_in >= 2 && in_ptr[1]) ? (const char *)in_ptr[1] : "";
        WCHAR wp[260];
        map_dsp_path(p, wp, 260);
        BOOLEAN is_dir = FALSE; u64 sz = 0;
        BOOLEAN ex = stat_nt_path(wp, &is_dir, &sz);
        umi_log("apps_std: fileExists('%s') -> %u\n", p, ex);
        if (n_out >= 1 && out_len[0] >= 1) out_ptr[0][0] = ex ? 1 : 0;
        return 0;
    }
    case 26: { /* opendir(path, rout DIR) */
        const char *p = (n_in >= 2 && in_ptr[1]) ? (const char *)in_ptr[1] : "";
        WCHAR wp[260];
        map_dsp_path(p, wp, 260);
        UNICODE_STRING path;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK iosb;
        HANDLE h = NULL;
        RtlInitUnicodeString(&path, wp);
        InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        NTSTATUS st = ZwCreateFile(&h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &oa, &iosb, NULL,
                                   FILE_ATTRIBUTE_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                   FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (!NT_SUCCESS(st)) {
            umi_log("apps_std: opendir('%s') failed 0x%x\n", p, (u32)st);
            return 2;
        }
        for (u32 i = 1; i < MAX_DIRS; i++) {
            if (!g_dirs[i].used) {
                g_dirs[i].used = TRUE; g_dirs[i].hDir = h; g_dirs[i].first = TRUE;
                u64 dh = i;
                if (n_out >= 1 && out_len[0] >= 8) RtlCopyMemory(out_ptr[0], &dh, 8);
                umi_log("apps_std: opendir('%s') -> dir=%u\n", p, i);
                return 0;
            }
        }
        ZwClose(h);
        return 24;
    }
    case 27: { /* closedir(DIR) */
        u64 dh = (n_in >= 1 && in_len[0] >= 8) ? *(const u64 *)in_ptr[0] : 0;
        if (dh > 0 && dh < MAX_DIRS && g_dirs[dh].used) {
            ZwClose(g_dirs[dh].hDir);
            RtlZeroMemory(&g_dirs[dh], sizeof(STD_DIR));
        }
        return 0;
    }
    case 28: { /* readdir(DIR, rout DIRENT, rout bEOF) */
        u64 dh = (n_in >= 1 && in_len[0] >= 8) ? *(const u64 *)in_ptr[0] : 0;
        if (dh == 0 || dh >= MAX_DIRS || !g_dirs[dh].used) return 9;
        u8 fbuf[600];
        IO_STATUS_BLOCK iosb;
        NTSTATUS st = ZwQueryDirectoryFile(g_dirs[dh].hDir, NULL, NULL, NULL, &iosb, fbuf, sizeof(fbuf),
                                           FileDirectoryInformation, TRUE, NULL, g_dirs[dh].first);
        g_dirs[dh].first = FALSE;
        if (!NT_SUCCESS(st)) {
            s32 eof = 1;
            if (n_out >= 1 && out_len[0] >= 264) RtlCopyMemory(out_ptr[0] + 260, &eof, 4);
            return 0;
        }
        UMI_FILE_DIR_INFO *info = (UMI_FILE_DIR_INFO *)fbuf;
        u32 nchars = info->FileNameLength / 2u;
        if (nchars > 254) nchars = 254;
        if (n_out >= 1 && out_len[0] >= 264) {
            s32 ino = 1, eof = 0;
            RtlCopyMemory(out_ptr[0], &ino, 4);
            for (u32 k = 0; k < nchars; k++) out_ptr[0][4 + k] = (u8)info->FileName[k];
            out_ptr[0][4 + nchars] = 0;
            RtlCopyMemory(out_ptr[0] + 260, &eof, 4);
        }
        return 0;
    }
    case 29: /* mkdir */
    case 30: /* rmdir */
    case 24: /* fremove */
        return 0;
    case 31: { /* Extended methods: sub_mid = in[0][0] */
        u32 sub = (n_in >= 1 && in_len[0] >= 4) ? *(const u32 *)in_ptr[0] : 0;
        if (sub == 31 && n_in >= 2 && in_ptr[1]) { /* apps_std_stat(path, rout STAT) */
            const char *p = (const char *)in_ptr[1];
            const char *virt = lookup_virtual_sysfs(p);
            BOOLEAN is_dir = FALSE; u64 sz = 0;
            if (virt) {
                while (virt[sz]) sz++;
            } else {
                WCHAR wp[260];
                map_dsp_path(p, wp, 260);
                if (!stat_nt_path(wp, &is_dir, &sz)) {
                    g_stat_fail++;
                    if (g_stat_fail <= 10) umi_log("apps_std: stat FAIL '%s'\n", p);
                    return 2;
                }
            }
            g_stat_ok++;
            APPS_STD_STAT st;
            RtlZeroMemory(&st, sizeof(st));
            st.tsz = sizeof(st);
            st.ino = 1;
            st.mode = is_dir ? 0040755u : 0100644u;
            st.nlink = 1;
            st.size = sz;
            st.mtime = 1230768000; /* matches persist_sensors/registry/registry/sns_reg_config */
            st.ctime = 1230768000;
            st.atime = 1230768000;
            if (n_out >= 1 && out_len[0] >= sizeof(st)) RtlCopyMemory(out_ptr[0], &st, sizeof(st));
            return 0;
        }
        return 0; /* sub=33 (rename) etc. */
    }
    default:
        umi_log("apps_std: unhandled mid=%u sc=0x%x\n", mid, sc);
        return 0;
    }
}

static BOOLEAN frpc_submit_async(BOOLEAN is_kernel, u32 handle, u32 sc, FRPC_ARG *args, u32 n_in, u32 n_out, u64 *out_ctx)
{
    if (!g_frpc_va || !umi_fastrpc_ready()) return FALSE;
    u32 n = n_in + n_out;
    u32 meta_sz = (u32)((sizeof(FASTRPC_REMOTE_BUF) + sizeof(FASTRPC_INVOKE_BUF) + sizeof(FASTRPC_PHY_PAGE)) * n +
                        sizeof(u64) * FASTRPC_MAX_FDLIST + sizeof(u32) * FASTRPC_MAX_CRCLIST + sizeof(u32));
    u32 off = (meta_sz + FASTRPC_ALIGN - 1u) & ~(FASTRPC_ALIGN - 1u);

    RtlZeroMemory(g_frpc_va, FRPC_BUF_SZ);
    FASTRPC_REMOTE_BUF *rpra = (FASTRPC_REMOTE_BUF *)g_frpc_va;
    FASTRPC_INVOKE_BUF *list = (FASTRPC_INVOKE_BUF *)(rpra + n);
    FASTRPC_PHY_PAGE   *pages = (FASTRPC_PHY_PAGE *)(list + n);

    for (u32 i = 0; i < n; i++) {
        u32 alen = args[i].len;
        rpra[i].len = alen;
        list[i].num = alen ? 1u : 0u;
        list[i].pgidx = i;
        if (!alen) continue;
        off = (off + FASTRPC_ALIGN - 1u) & ~(FASTRPC_ALIGN - 1u);
        if (off + alen > FRPC_BUF_SZ) return FALSE;
        u64 buf_dma = g_frpc_dma + off;
        rpra[i].pv = buf_dma;
        pages[i].addr = buf_dma & ~0xFFFULL;
        u64 pg_start = buf_dma >> 12;
        u64 pg_end   = (buf_dma + alen - 1u) >> 12;
        pages[i].size = (pg_end - pg_start + 1u) * 4096u;
        if (i < n_in && args[i].ptr) {
            RtlCopyMemory(g_frpc_va + off, args[i].ptr, alen);
        }
        if (handle == FASTRPC_LISTENER_HANDLE && i >= n_in && (i - n_in) < 2) {
            g_next2_out_va_off[i - n_in] = off;
        }
        off += alen;
    }

    u32 pkt_sz = (off + 4095u) & ~4095u;
    umi_dsb();

    u64 ctx = ((u64)(g_frpc_seq++) << 16) | (1ULL << 8) | SENSORS_PD;
    FASTRPC_MSG msg;
    RtlZeroMemory(&msg, sizeof(msg));
    msg.client_id = is_kernel ? 0 : 100;
    msg.tid       = 100;
    msg.ctx       = ctx;
    msg.handle    = handle;
    msg.sc        = sc;
    msg.addr      = g_frpc_dma;
    msg.size      = pkt_sz;

    if (out_ctx) *out_ctx = ctx;
    return umi_fastrpc_send(&msg, sizeof(msg));
}

static s32 frpc_invoke_sync(BOOLEAN is_kernel, u32 handle, u32 sc, FRPC_ARG *args, u32 n_in, u32 n_out, u32 timeout_ms)
{
    u64 ctx = 0;
    g_rsp_got = FALSE;
    if (!frpc_submit_async(is_kernel, handle, sc, args, n_in, n_out, &ctx)) return -3;

    for (u32 ms = 0; ms < timeout_ms; ms++) {
        umi_slpi_pump();
        if (g_rsp_got && g_rsp_ctx == ctx) {
            FASTRPC_REMOTE_BUF *rpra = (FASTRPC_REMOTE_BUF *)g_frpc_va;
            for (u32 i = n_in; i < n_in + n_out; i++) {
                if (args[i].ptr && args[i].len && rpra[i].pv >= g_frpc_dma) {
                    u32 boff = (u32)(rpra[i].pv - g_frpc_dma);
                    if (boff + args[i].len <= FRPC_BUF_SZ) {
                        RtlCopyMemory(args[i].ptr, g_frpc_va + boff, args[i].len);
                    }
                }
            }
            return g_rsp_val;
        }
        umi_sleep_ms(1);
    }
    return -4;
}

static void submit_next2(u32 prev_ctx, s32 prev_res, u32 prev_len)
{
    g_next2_in_scalars[0] = prev_ctx;
    g_next2_in_scalars[1] = (u32)prev_res;
    g_next2_in_scalars[2] = prev_len;
    g_next2_in_scalars[3] = LISTENER_BUF_SZ;
    FRPC_ARG a[4] = {
        { g_next2_in_scalars, sizeof(g_next2_in_scalars) },
        { g_next2_in_buf, prev_len },
        { g_next2_out_scalars, sizeof(g_next2_out_scalars) },
        { g_next2_out_buf, LISTENER_BUF_SZ },
    };
    g_next2_pending = frpc_submit_async(FALSE, FASTRPC_LISTENER_HANDLE,
                                        (4u << 24) | (2u << 16) | (2u << 8), a, 2, 2, &g_next2_ctx);
}

void umi_fastrpc_on_rx(const u8 *data, u32 len)
{
    if (len < sizeof(FASTRPC_RSP)) return;
    FASTRPC_RSP rsp;
    RtlCopyMemory(&rsp, data, sizeof(rsp));
    g_rsp_ctx = rsp.ctx;
    g_rsp_val = rsp.retval;
    g_rsp_got = TRUE;

    if (g_listener_ready && g_next2_pending && rsp.ctx == g_next2_ctx) {
        g_next2_pending = FALSE;
        if (rsp.retval != 0) {
            umi_log("frpc: next2 rsp err=%d (0x%x)\n", rsp.retval, (u32)rsp.retval);
            return;
        }
        /* Copy out_scalars and out_buf from g_frpc_va */
        RtlCopyMemory(g_next2_out_scalars, g_frpc_va + g_next2_out_va_off[0], sizeof(g_next2_out_scalars));
        u32 req_ctx = g_next2_out_scalars[0];
        u32 req_h   = g_next2_out_scalars[1];
        u32 req_sc  = g_next2_out_scalars[2];
        u32 req_len = g_next2_out_scalars[3];
        if (req_len > LISTENER_BUF_SZ) req_len = LISTENER_BUF_SZ;
        if (req_len) RtlCopyMemory(g_next2_out_buf, g_frpc_va + g_next2_out_va_off[1], req_len);

        u32 reply_len = 0;
        s32 res = dispatch_reverse_rpc(req_h, req_sc, g_next2_out_buf, req_len, &reply_len);
        if (res != 0) reply_len = 0;
        submit_next2(req_ctx, res, reply_len);
    }
}

void umi_fastrpc_try_start(void)
{
    if (g_frpc_started || !umi_fastrpc_ready()) return;
    g_frpc_started = TRUE;

    RtlZeroMemory(g_files, sizeof(g_files));
    RtlZeroMemory(g_dirs, sizeof(g_dirs));
    smmu_setup_slpi_bypass();

    PHYSICAL_ADDRESS low = { 0 }, high, bound = { 0 };
    high.QuadPart = 0xFFFFFFFFLL;
    g_frpc_va = (u8 *)MmAllocateContiguousMemorySpecifyCache(FRPC_BUF_SZ, low, high, bound, MmNonCached);
    g_next2_in_buf  = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, LISTENER_BUF_SZ, UMI_TAG);
    g_next2_out_buf = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, LISTENER_BUF_SZ, UMI_TAG);
    if (!g_frpc_va || !g_next2_in_buf || !g_next2_out_buf) {
        umi_log("frpc: buffer alloc failed\n");
        return;
    }
    g_frpc_pa = (u64)MmGetPhysicalAddress(g_frpc_va).QuadPart;
    g_frpc_dma = SLPI_FASTRPC_SID_SHIFT | g_frpc_pa;

    s32 cid = 100;
    FRPC_ARG a1[1] = { { &cid, sizeof(cid) } };
    s32 rc = frpc_invoke_sync(TRUE, FASTRPC_INIT_HANDLE, (0u << 24) | (1u << 16), a1, 1, 0, 2000);
    umi_log("frpc: INIT_ATTACH(SENSORS_PD) rc=%d\n", rc);
    if (rc != 0) return;

    rc = frpc_invoke_sync(FALSE, FASTRPC_LISTENER_HANDLE, (3u << 24), NULL, 0, 0, 2000);
    umi_log("frpc: adsp_listener_init2 rc=%d\n", rc);
    if (rc != 0) return;

    const char *dl_name = "adsp_default_listener";
    u32 dl_in[2] = { 22u, 256u };
    u32 dl_out[2] = { 0, 0 };
    static char dl_errstr[256];
    RtlZeroMemory(dl_errstr, sizeof(dl_errstr));
    FRPC_ARG a2b[4] = {
        { dl_in, sizeof(dl_in) },
        { (void *)dl_name, 22u },
        { dl_out, sizeof(dl_out) },
        { dl_errstr, sizeof(dl_errstr) },
    };
    rc = frpc_invoke_sync(FALSE, 0u, (0u << 24) | (2u << 16) | (2u << 8), a2b, 2, 2, 2000);
    if (rc != 0 || dl_out[0] == 0) {
        umi_log("frpc: remotectl_open('adsp_default_listener') failed rc=%d\n", rc);
        return;
    }
    rc = frpc_invoke_sync(FALSE, dl_out[0], 0u, NULL, 0, 0, 2000);
    umi_log("frpc: adsp_default_listener_register rc=%d — starting async apps_std server!\n", rc);
    if (rc != 0) return;

    g_listener_ready = TRUE;
    submit_next2(0, 0, 0);
}

void umi_fastrpc_stop(void)
{
    g_listener_ready = FALSE;
    g_next2_pending = FALSE;
    g_frpc_started = FALSE;
    for (u32 i = 1; i < MAX_FILES; i++) {
        if (g_files[i].used && g_files[i].data) ExFreePoolWithTag(g_files[i].data, UMI_TAG);
        g_files[i].used = FALSE; g_files[i].data = NULL;
    }
    for (u32 i = 1; i < MAX_DIRS; i++) {
        if (g_dirs[i].used) ZwClose(g_dirs[i].hDir);
        g_dirs[i].used = FALSE;
    }
    if (g_next2_in_buf)  { ExFreePoolWithTag(g_next2_in_buf, UMI_TAG); g_next2_in_buf = NULL; }
    if (g_next2_out_buf) { ExFreePoolWithTag(g_next2_out_buf, UMI_TAG); g_next2_out_buf = NULL; }
    if (g_frpc_va) {
        MmFreeContiguousMemorySpecifyCache(g_frpc_va, FRPC_BUF_SZ, MmNonCached);
        g_frpc_va = NULL;
    }
}
