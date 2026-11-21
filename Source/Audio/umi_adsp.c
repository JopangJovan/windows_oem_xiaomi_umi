/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Xiaomi Mi 10 (UMI) ADSP PIL + RPMh + AOSS QMP + SMEM + SMP2P + GLINK + APR (Windows ARM64)
 * Ported from hardware-verified HackiOS UMI implementation.
 */
#include "umi_hw.h"

#define UMI_ADSP_MEMORY     0x8BB00000ULL
#define UMI_ADSP_SIZE       0x02500000u /* 37 MiB */
#define PAS_ADSP            1u

#define SMEM_PHYS           0x80900000ULL
#define SMEM_SIZE           0x00200000u
#define SMEM_US             0u
#define SMEM_ADSP           2u
#define SMEM_GLOBAL         0xFFFEu
#define SMEM_CANARY         0xA5A5u

#define ITEM_SMP2P_OURS         429u
#define ITEM_SMP2P_THEIRS       443u
#define ITEM_GLINK_DESCRIPTOR   478u
#define ITEM_GLINK_OUR_QUEUE    479u
#define ITEM_GLINK_THEIR_QUEUE  480u

#define AOSS_PHYS           0x0C300000ULL
#define AOSS_SIZE           0x00000400u
#define IPCC_PHYS           0x00408000ULL
#define IPCC_SIZE           0x00001000u
#define CMD_DB_PHYS         0x80860000ULL
#define CMD_DB_SIZE         0x00020000u
#define RSC_PHYS            0x18220000ULL
#define RSC_SIZE            0x00010000u
#define SMEM_LOCK_PHYS      0x01F43000ULL
#define SMEM_LOCK_SIZE      0x00001000u

#define IPCC_SEND           0x0Cu
#define IPCC_RECEIVED       0x10u
#define IPCC_ENABLE         0x14u
#define IPCC_DISABLE        0x18u
#define IPCC_CLEAR          0x1Cu
#define IPCC_NOBODY         0xFFFFFFFFu
#define IPCC_AOP            0u
#define IPCC_LPASS          3u
#define IPCC_GLINK          0u
#define IPCC_SMP2P          2u

#define QMP_MAGIC           0x00u
#define QMP_VERSION         0x04u
#define QMP_THEIR_LINK      0x0Cu
#define QMP_THEIR_LINK_ACK  0x10u
#define QMP_THEIR_CHANNEL   0x14u
#define QMP_THEIR_CH_ACK    0x18u
#define QMP_OUR_LINK        0x24u
#define QMP_OUR_LINK_ACK    0x28u
#define QMP_OUR_CHANNEL     0x2Cu
#define QMP_OUR_CH_ACK      0x30u
#define QMP_OUR_BOX_SIZE    0x34u
#define QMP_OUR_BOX         0x38u
#define QMP_MAGIC_VALUE     0x4D41494Cu /* "MAIL" */
#define QMP_UP              0x0000FFFFu
#define QMP_DOWN            0xFFFF0000u
#define QMP_MESSAGE         64u

#define RSC_TCS_BASE        0xD00u
#define RSC_IRQ_STATUS      (RSC_TCS_BASE + 0x04u)
#define RSC_IRQ_CLEAR       (RSC_TCS_BASE + 0x08u)
#define RSC_TCS(n)          (RSC_TCS_BASE + 672u * (n))
#define TCS_WAIT_FOR_DONE   0x10u
#define TCS_CONTROL         0x14u
#define TCS_STATUS          0x18u
#define TCS_ENABLED         0x1Cu
#define TCS_COMMAND(c)      (0x30u + 20u * (c))
#define TCS_ACTIVE_MODE     (1u << 16)
#define TCS_TRIGGER         (1u << 24)
#define MSGID_WRITE_WAITED  (8u | (1u << 8) | (1u << 16))

#define SCM_64              (1ULL << 30)
#define SCM_SIP             (2ULL << 24)
#define SCM_CALL(s, c)      (((u64)(s) << 8) | (c))
#define SCM_PAS_INIT_IMAGE  SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP   SCM_CALL(2, 2)
#define SCM_PAS_AUTH_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN    SCM_CALL(2, 6)
#define SCM_ONE_VALUE       1u
#define SCM_THREE_VALUES    3u
#define SCM_VALUE_AND_BUF   (2u | (2u << 6))
#define SCM_INTERRUPTED     1

#pragma pack(push, 1)
struct cmd_db_slave  { u16 id, header_offset, data_offset, count, version, reserved[3]; };
struct cmd_db_header { u32 version; u8 magic[4]; struct cmd_db_slave slave[8]; u32 checksum, reserved; u8 data[1]; };
struct cmd_db_entry  { char id[8]; u32 priority[2]; u32 address; u16 length, offset; };

struct smem_table_entry { u32 offset, size, flags; u16 host0, host1; u32 cacheline, reserved[7]; };
struct smem_table       { u8 magic[4]; u32 version, count, reserved[5]; };
struct smem_partition   { u8 magic[4]; u16 host0, host1; u32 size, free_uncached, free_cached, reserved[3]; };
struct smem_entry       { u16 canary, item; u32 size; u16 padding_data, padding_header; u32 reserved; };

struct smp2p {
    u32 magic, version;
    u16 local, remote, total, valid;
    u32 flags;
    struct { char name[16]; u32 value; } entry[16];
};

struct elf32_phdr { u32 type, offset, vaddr, paddr, file_size, mem_size, flags, align; };

struct glink_msg { u16 cmd, param1; u32 param2; };

struct apr_header {
    u16 field, size;
    u8  from_service, from_domain;
    u16 from_port;
    u8  to_service, to_domain;
    u16 to_port;
    u32 token, opcode;
};
#pragma pack(pop)

#define SMP2P_MAGIC         0x504D5324u
#define SMP2P_VERSION       1u
#define SMP2P_RESTART_ACKS  1u
#define SMP2P_RESTART_DONE  (1u << 0)
#define SMP2P_RESTART_ACK   (1u << 1)
#define SMP2P_AT_VERSION    4u
#define SMP2P_AT_PROCS      8u
#define SMP2P_AT_COUNTS     12u
#define SMP2P_AT_FLAGS      16u
#define SMP2P_AT_NAME(i)    (20u + 20u * (i))
#define SMP2P_AT_VALUE(i)   (36u + 20u * (i))
#define DSP_FATAL           (1u << 0)
#define DSP_READY           (1u << 1)
#define DSP_HANDOVER        (1u << 2)
#define DSP_STOP_ACK        (1u << 3)
#define DSP_STOP            (1u << 0)

#define GLINK_OUR_TAIL      0u
#define GLINK_OUR_HEAD      4u
#define GLINK_THEIR_TAIL    8u
#define GLINK_THEIR_HEAD    12u
#define GLINK_RESERVE       16u
enum {
    GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3,
    GLINK_OPEN_ACK = 4, GLINK_INTENT = 5, GLINK_DONE = 6, GLINK_INTENT_REQ = 7,
    GLINK_INTENT_REQ_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
    GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};

#define APR_DOMAIN_ADSP     4u
#define APR_DOMAIN_APPS     5u
#define APR_MSG_TYPE_SEQ    2u
#define APR_HDR_FIELD(t, b) ((u16)(((t) << 8) | (((b) / 4u) << 4)))

static volatile u8 *g_smem, *g_aoss, *g_ipcc, *g_cmd_db, *g_rsc, *g_smem_lock;
static u32 g_our_box, g_our_item;
static BOOLEAN g_dsp_started, g_rails_asked, g_smp2p_agreed, g_smp2p_racks, g_smp2p_racked;
static FAST_MUTEX g_glink_lock, g_apr_lock;

typedef struct {
    u32 id, size;
    BOOLEAN there, in_use;
} GLINK_THEIR_INTENT;

typedef struct {
    u32 size, fill;
    u8 *data;
} GLINK_OUR_INTENT;

typedef struct {
    const char *name;
    u32 ours, theirs;
    BOOLEAN asked, acked, open, req_ans, req_ok;
    GLINK_THEIR_INTENT their_intents[64];
    GLINK_OUR_INTENT   our_intents[24];
    u32 our_intent_count;
} GLINK_CHAN;

static struct {
    u32 desc, ours, ours_sz, theirs, theirs_sz;
    BOOLEAN up, broken, agreed;
    u32 features;
    GLINK_CHAN apr_ch;
} g_glink;

static UMI_APR_EVENT_FN g_apr_events;
static volatile LONG g_apr_answer_ready;
static u32 g_apr_waited_token, g_apr_next_token = 1;
static UMI_APR_ANSWER g_apr_answer;

static s32 smc_call(u64 fn, u32 arginfo, u64 a1, u64 a2, u64 a3, u64 *ans)
{
    u64 func = fn | SCM_64 | SCM_SIP;
    u64 session = 0;
    for (u32 i = 0; i < 2000; i++) {
        register u64 x0 __asm__("x0") = (i == 0) ? func : (u64)SCM_INTERRUPTED;
        register u64 x1 __asm__("x1") = (i == 0) ? (u64)arginfo : 0;
        register u64 x2 __asm__("x2") = (i == 0) ? a1 : 0;
        register u64 x3 __asm__("x3") = (i == 0) ? a2 : 0;
        register u64 x4 __asm__("x4") = (i == 0) ? a3 : 0;
        register u64 x5 __asm__("x5") = 0;
        register u64 x6 __asm__("x6") = session;
        register u64 x7 __asm__("x7") = 0;
        __asm__ volatile("smc #0"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
            :
            : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        session = x6;
        if ((s64)x0 != SCM_INTERRUPTED) {
            if (ans) *ans = x1;
            if (i > 0) umi_log("adsp: smc 0x%llx finished after %u retries (rc=%d)\n", fn, i, (s32)x0);
            return (s32)x0;
        }
        if (i >= 20) umi_spin_us(500);
    }
    umi_log("adsp: smc 0x%llx TIMED OUT after 2000 SCM_INTERRUPTED retries\n", fn);
    return -2;
}

static BOOLEAN smem_peek(u32 off, void *dst, u32 sz)
{
    if (!g_smem || (u64)off + sz > SMEM_SIZE) return FALSE;
    for (u32 i = 0; i < sz; i++) ((u8 *)dst)[i] = g_smem[off + i];
    return TRUE;
}
static u32  smem_word(u32 off) { return umi_rd32(g_smem, off); }
static void smem_poke(u32 off, u32 val) { umi_wr32(g_smem, off, val); }

static u32 smem_partition(u16 h0, u16 h1, u32 *cl, u32 *psz)
{
    struct smem_table tbl;
    struct smem_table_entry e;
    struct smem_partition hdr;
    if (!smem_peek(SMEM_SIZE - 0x1000, &tbl, sizeof(tbl)) ||
        RtlCompareMemory(tbl.magic, "$TOC", 4) != 4 || tbl.version != 1 || tbl.count > 64)
        return 0;
    for (u32 i = 0; i < tbl.count; i++) {
        if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(tbl) + i * sizeof(e), &e, sizeof(e)) || !e.offset || !e.size)
            continue;
        if (!((e.host0 == h0 && e.host1 == h1) || (e.host0 == h1 && e.host1 == h0))) continue;
        if ((u64)e.offset + e.size > SMEM_SIZE - 0x1000 || !smem_peek(e.offset, &hdr, sizeof(hdr)) ||
            RtlCompareMemory(hdr.magic, "$PRT", 4) != 4 || hdr.size != e.size) return 0;
        if (cl) *cl = e.cacheline;
        if (psz) *psz = e.size;
        return e.offset;
    }
    return 0;
}

static u32 smem_item(u32 item, u32 *out_sz)
{
    static const u16 hosts[2][2] = { { SMEM_US, SMEM_ADSP }, { SMEM_GLOBAL, SMEM_GLOBAL } };
    for (u32 h = 0; h < 2; h++) {
        u32 cl = 0, psz = 0;
        u32 part = smem_partition(hosts[h][0], hosts[h][1], &cl, &psz);
        struct smem_partition hdr;
        struct smem_entry e;
        if (!part || !smem_peek(part, &hdr, sizeof(hdr)) || hdr.free_uncached > hdr.size || hdr.free_cached > hdr.size)
            continue;
        for (u32 at = sizeof(hdr); at + sizeof(e) <= hdr.free_uncached;) {
            if (!smem_peek(part + at, &e, sizeof(e)) || e.canary != SMEM_CANARY || e.size > hdr.size) break;
            if (e.padding_data > e.size || (u64)at + sizeof(e) + e.padding_header + e.size > hdr.size) break;
            if (e.item == item) {
                if (out_sz) *out_sz = e.size - e.padding_data;
                return part + at + sizeof(e) + e.padding_header;
            }
            at += sizeof(e) + e.padding_header + e.size;
        }
        if (cl && !(cl & (cl - 1))) {
            u32 sb = ((u32)sizeof(e) + cl - 1) & ~(cl - 1);
            for (u32 at = hdr.size - sb; at > hdr.free_cached && at < hdr.size;) {
                if (!smem_peek(part + at, &e, sizeof(e)) || e.canary != SMEM_CANARY || e.size > at || e.padding_data > e.size)
                    break;
                if (e.item == item) {
                    if (out_sz) *out_sz = e.size - e.padding_data;
                    return part + at - e.size;
                }
                if (e.size + sb > at) break;
                at -= e.size + sb;
            }
        }
    }
    return 0;
}

static BOOLEAN smem_make_private(u32 part, u32 item, u32 bytes)
{
    struct smem_partition hdr;
    struct smem_entry e;
    u32 item_sz = (bytes + 7u) & ~7u;
    u32 need_b = (u32)sizeof(e) + item_sz;
    u32 owner = ~0u;
    for (u32 ms = 0; ms <= 1000; ms++) {
        umi_wr32(g_smem_lock, 0, 1u);
        owner = umi_rd32(g_smem_lock, 0);
        if (owner == 1u) break;
        umi_sleep_ms(1);
    }
    if (owner != 1u) return FALSE;
    BOOLEAN ok = FALSE;
    if (smem_peek(part, &hdr, sizeof(hdr)) && hdr.free_uncached <= hdr.free_cached && hdr.free_cached <= hdr.size) {
        u32 at = sizeof(hdr);
        while (at < hdr.free_uncached) {
            if (!smem_peek(part + at, &e, sizeof(e)) || e.canary != SMEM_CANARY || e.size > hdr.size) break;
            if (e.item == item) { ok = TRUE; goto out; }
            at += sizeof(e) + e.padding_header + e.size;
        }
        if (at == hdr.free_uncached && at + need_b <= hdr.free_cached) {
            smem_poke(part + at, SMEM_CANARY | (item << 16));
            smem_poke(part + at + 4, item_sz);
            smem_poke(part + at + 8, item_sz - bytes);
            smem_poke(part + at + 12, 0);
            umi_dsb();
            smem_poke(part + FIELD_OFFSET(struct smem_partition, free_uncached), at + need_b);
            umi_dsb();
            ok = TRUE;
        }
    }
out:
    umi_wr32(g_smem_lock, 0, 0);
    return ok;
}

static BOOLEAN qmp_wait(u32 off, u32 val, u32 ms)
{
    for (u32 i = 0; i < ms * 10; i++) {
        if (umi_rd32(g_aoss, off) == val) return TRUE;
        umi_spin_us(100);
    }
    return umi_rd32(g_aoss, off) == val;
}

static BOOLEAN qmp_send(const char *text)
{
    if (g_our_box == 0) {
        if (umi_rd32(g_aoss, QMP_MAGIC) != QMP_MAGIC_VALUE || umi_rd32(g_aoss, QMP_VERSION) != 1) return FALSE;
        u32 box = umi_rd32(g_aoss, QMP_OUR_BOX), sz = umi_rd32(g_aoss, QMP_OUR_BOX_SIZE);
        if (sz < QMP_MESSAGE + 4 || box < 0x3c || box >= AOSS_SIZE || sz > AOSS_SIZE - box) return FALSE;
        if (!(umi_rd32(g_aoss, QMP_OUR_LINK) == QMP_UP && umi_rd32(g_aoss, QMP_OUR_LINK_ACK) == QMP_UP &&
              umi_rd32(g_aoss, QMP_OUR_CHANNEL) == QMP_UP && umi_rd32(g_aoss, QMP_THEIR_CHANNEL) == QMP_UP &&
              umi_rd32(g_aoss, QMP_OUR_CH_ACK) == QMP_UP)) {
            umi_wr32(g_aoss, QMP_THEIR_LINK_ACK, umi_rd32(g_aoss, QMP_THEIR_LINK));
            umi_wr32(g_aoss, QMP_OUR_LINK, QMP_UP);
            umi_wr32(g_ipcc, IPCC_SEND, (IPCC_AOP << 16) | 0);
            if (!qmp_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) return FALSE;
            umi_wr32(g_aoss, QMP_OUR_CHANNEL, QMP_UP);
            umi_wr32(g_ipcc, IPCC_SEND, (IPCC_AOP << 16) | 0);
            if (!qmp_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) return FALSE;
            umi_wr32(g_aoss, QMP_THEIR_CH_ACK, QMP_UP);
            umi_wr32(g_ipcc, IPCC_SEND, (IPCC_AOP << 16) | 0);
            if (!qmp_wait(QMP_OUR_CH_ACK, QMP_UP, 1000)) return FALSE;
        }
        g_our_box = box;
    }
    if (umi_rd32(g_aoss, g_our_box) != 0) return FALSE;
    u32 words[QMP_MESSAGE / 4];
    RtlZeroMemory(words, sizeof(words));
    SIZE_T n = 0;
    while (text[n] && n < QMP_MESSAGE - 1) { ((char *)words)[n] = text[n]; n++; }
    for (u32 i = 0; i < QMP_MESSAGE / 4; i++) umi_wr32(g_aoss, g_our_box + 4 + 4 * i, words[i]);
    umi_wr32(g_aoss, g_our_box, QMP_MESSAGE);
    (void)umi_rd32(g_aoss, g_our_box);
    umi_wr32(g_ipcc, IPCC_SEND, (IPCC_AOP << 16) | 0);
    return qmp_wait(g_our_box, 0, 1000);
}

static int rpmh_rail_pos(const char *name, u32 level, u32 *addr)
{
    const struct cmd_db_header *db = (const struct cmd_db_header *)g_cmd_db;
    if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) return -1;
    char id[8] = { 0 };
    for (u32 i = 0; i < 8 && name[i]; i++) id[i] = name[i];
    for (u32 s = 0; s < 8; s++) {
        const struct cmd_db_slave *sl = &db->slave[s];
        if (!sl->id || sl->count > 512) continue;
        const struct cmd_db_entry *en = (const struct cmd_db_entry *)(db->data + sl->header_offset);
        for (u32 i = 0; i < sl->count; i++) {
            if (RtlCompareMemory(en[i].id, id, 8) != 8) continue;
            const u8 *d = db->data + sl->data_offset + en[i].offset;
            for (u32 l = 0; l < en[i].length / 2u && l < 16; l++) {
                u32 v = (u32)d[2 * l] | ((u32)d[2 * l + 1] << 8);
                if (v >= level) { *addr = en[i].address; return (int)l; }
            }
            return -1;
        }
    }
    return -1;
}

static BOOLEAN rpmh_rails(u32 level)
{
    static const char *const names[2] = { "lcx.lvl", "lmx.lvl" };
    u32 addrs[2] = { 0, 0 }, vals[2] = { 0, 0 };
    for (u32 r = 0; r < 2; r++) {
        int pos = level ? rpmh_rail_pos(names[r], level, &addrs[r]) : 0;
        if (!level) rpmh_rail_pos(names[r], 0, &addrs[r]);
        if (pos < 0 || !addrs[r]) return FALSE;
        vals[r] = (u32)pos;
    }
    if (umi_rd32(g_rsc, RSC_TCS(0) + TCS_STATUS) == 0) return FALSE;
    umi_wr32(g_rsc, RSC_IRQ_CLEAR, 1u);
    for (u32 r = 0; r < 2; r++) {
        umi_wr32(g_rsc, RSC_TCS(0) + TCS_COMMAND(r), MSGID_WRITE_WAITED);
        umi_wr32(g_rsc, RSC_TCS(0) + TCS_COMMAND(r) + 4, addrs[r]);
        umi_wr32(g_rsc, RSC_TCS(0) + TCS_COMMAND(r) + 8, vals[r]);
    }
    umi_wr32(g_rsc, RSC_TCS(0) + TCS_WAIT_FOR_DONE, 3u);
    umi_wr32(g_rsc, RSC_TCS(0) + TCS_ENABLED, 3u);
    umi_dsb();
    u32 ctrl = umi_rd32(g_rsc, RSC_TCS(0) + TCS_CONTROL);
    umi_wr32(g_rsc, RSC_TCS(0) + TCS_CONTROL, ctrl & ~TCS_TRIGGER);
    umi_wr32(g_rsc, RSC_TCS(0) + TCS_CONTROL, (ctrl & ~TCS_TRIGGER) | TCS_ACTIVE_MODE);
    umi_wr32(g_rsc, RSC_TCS(0) + TCS_CONTROL, (ctrl & ~TCS_TRIGGER) | TCS_ACTIVE_MODE | TCS_TRIGGER);
    u32 done = 0;
    for (u32 i = 0; i < 2000; i++) {
        done = umi_rd32(g_rsc, RSC_IRQ_STATUS) & 1u;
        if (done) break;
        umi_spin_us(50);
    }
    umi_wr32(g_rsc, RSC_IRQ_CLEAR, 1u);
    umi_wr32(g_rsc, RSC_TCS(0) + TCS_CONTROL, umi_rd32(g_rsc, RSC_TCS(0) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
    umi_wr32(g_rsc, RSC_TCS(0) + TCS_ENABLED, 0);
    umi_log("adsp: rpmh rails level=%u done=%u\n", level, done);
    return done != 0;
}

static int smp2p_find_entry(const struct smp2p *s, const char *name)
{
    u32 n = s->valid < 16 ? s->valid : 16;
    SIZE_T len = 0; while (name[len]) len++;
    for (u32 i = 0; i < n; i++) {
        if (RtlCompareMemory(s->entry[i].name, name, len) == len &&
            (len == 16 || s->entry[i].name[len] == 0)) return (int)i;
    }
    return -1;
}

static BOOLEAN dsp_words(struct smp2p *out)
{
    u32 sz = 0, at = smem_item(ITEM_SMP2P_THEIRS, &sz);
    return at && sz >= sizeof(*out) && smem_peek(at, out, sizeof(*out)) && out->magic == SMP2P_MAGIC;
}

static u32 dsp_state(const struct smp2p *s)
{
    int idx = smp2p_find_entry(s, "slave-kernel");
    return idx >= 0 ? s->entry[idx].value : 0;
}

static void smp2p_listen(const struct smp2p *theirs)
{
    struct smp2p ours;
    if (!g_our_item || !smem_peek(g_our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) return;
    if (!g_smp2p_agreed) {
        if ((theirs->version & 0xff) != (ours.version & 0xff)) return;
        u32 feat = (ours.version >> 8) & (theirs->version >> 8);
        if (feat != (ours.version >> 8)) smem_poke(g_our_item + SMP2P_AT_VERSION, (feat << 8) | (ours.version & 0xff));
        g_smp2p_racks = (feat & SMP2P_RESTART_ACKS) != 0;
        g_smp2p_agreed = TRUE;
    }
    if (g_smp2p_racks && (((theirs->flags & SMP2P_RESTART_DONE) != 0) != g_smp2p_racked)) {
        g_smp2p_racked = !g_smp2p_racked;
        smem_poke(g_our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (g_smp2p_racked ? SMP2P_RESTART_ACK : 0));
        umi_dsb();
        umi_wr32(g_ipcc, IPCC_SEND, (IPCC_LPASS << 16) | IPCC_SMP2P);
    }
}

static void clear_doorbells(void)
{
    for (u32 i = 0; i < 8; i++) {
        u32 who = umi_rd32(g_ipcc, IPCC_RECEIVED);
        if (who == IPCC_NOBODY || (who >> 16) != IPCC_LPASS) return;
        umi_wr32(g_ipcc, IPCC_CLEAR, who);
    }
}

/* ---- GLINK + APR ---- */
static u32 glink_waiting(void)
{
    u32 h = smem_word(g_glink.desc + GLINK_THEIR_HEAD), t = smem_word(g_glink.desc + GLINK_THEIR_TAIL);
    if (h >= g_glink.theirs_sz || t >= g_glink.theirs_sz) return 0;
    return (h >= t) ? (h - t) : (g_glink.theirs_sz - t + h);
}

static void glink_peek(void *dst, u32 off, u32 cnt)
{
    u32 at = (smem_word(g_glink.desc + GLINK_THEIR_TAIL) + off) % g_glink.theirs_sz;
    for (u32 i = 0; i < cnt; i++) ((u8 *)dst)[i] = g_smem[g_glink.theirs + ((at + i) % g_glink.theirs_sz)];
}

static void glink_adv(u32 cnt)
{
    smem_poke(g_glink.desc + GLINK_THEIR_TAIL, (smem_word(g_glink.desc + GLINK_THEIR_TAIL) + cnt) % g_glink.theirs_sz);
}

static u32 glink_room(void)
{
    u32 h = smem_word(g_glink.desc + GLINK_OUR_HEAD), t = smem_word(g_glink.desc + GLINK_OUR_TAIL);
    if (h >= g_glink.ours_sz || t >= g_glink.ours_sz) return 0;
    u32 r = (t <= h) ? (g_glink.ours_sz - h + t) : (t - h);
    return r < GLINK_RESERVE ? 0 : r - GLINK_RESERVE;
}

static BOOLEAN glink_put(const void *a, u32 alen, const void *b, u32 blen)
{
    u32 len = alen + blen, pad = (len + 7u) & ~7u;
    if (!g_glink.up || glink_room() < pad) return FALSE;
    u32 h = smem_word(g_glink.desc + GLINK_OUR_HEAD);
    volatile u8 *q = g_smem + g_glink.ours;
    for (u32 i = 0; i < pad; i++) {
        q[(h + i) % g_glink.ours_sz] = (i < alen) ? ((const u8 *)a)[i] : (i < len ? ((const u8 *)b)[i - alen] : 0);
    }
    umi_dsb();
    smem_poke(g_glink.desc + GLINK_OUR_HEAD, (h + pad) % g_glink.ours_sz);
    umi_dsb();
    umi_wr32(g_ipcc, IPCC_SEND, (IPCC_LPASS << 16) | IPCC_GLINK);
    return TRUE;
}

static void glink_say(u16 cmd, u16 p1, u32 p2, const void *more, u32 mlen)
{
    struct glink_msg m = { cmd, p1, p2 };
    glink_put(&m, sizeof(m), more, mlen);
}

static BOOLEAN glink_offer(GLINK_CHAN *ch, u32 sz)
{
    u32 i = ch->our_intent_count;
    if (i >= 24 || !sz || sz > 0x10000) return FALSE;
    if (!ch->our_intents[i].data) {
        ch->our_intents[i].data = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, UMI_TAG);
        if (!ch->our_intents[i].data) return FALSE;
    }
    ch->our_intents[i].size = sz;
    ch->our_intents[i].fill = 0;
    ch->our_intent_count++;
    u32 w[2] = { sz, ch->our_intent_count };
    glink_say(GLINK_INTENT, (u16)ch->ours, 1, w, sizeof(w));
    return TRUE;
}

static void apr_deliver(const u8 *data, u32 len)
{
    if (len < sizeof(struct apr_header)) return;
    struct apr_header hdr;
    RtlCopyMemory(&hdr, data, sizeof(hdr));
    u32 hlen = ((hdr.field >> 4) & 0xfu) * 4u;
    if (hlen < sizeof(hdr) || hlen > len) return;
    u32 plen = len - hlen;
    u32 words[64];
    RtlZeroMemory(words, sizeof(words));
    RtlCopyMemory(words, data + hlen, plen < sizeof(words) ? plen : sizeof(words));
    if (hdr.token != g_apr_waited_token || g_apr_waited_token == 0) {
        UMI_APR_EVENT_FN fn = g_apr_events;
        if (fn && fn(hdr.from_service, hdr.from_port, hdr.opcode, hdr.token, words, plen)) return;
    }
    RtlCopyMemory(g_apr_answer.words, words, sizeof(words));
    g_apr_answer.length = plen;
    g_apr_answer.opcode = hdr.opcode;
    g_apr_answer.token  = hdr.token;
    InterlockedExchange(&g_apr_answer_ready, 1);
}

static void glink_pump_locked(void)
{
    struct glink_msg m;
    u32 words[4];
    char name[33];
    clear_doorbells();
    while (g_glink.up && !g_glink.broken) {
        u32 w = glink_waiting();
        if (w < sizeof(m)) return;
        glink_peek(&m, 0, sizeof(m));
        u64 wide = sizeof(m);
        if (m.cmd == GLINK_OPEN) wide += m.param2;
        else if (m.cmd == GLINK_INTENT) wide += 8ULL * m.param2;
        else if (m.cmd == GLINK_DATA || m.cmd == GLINK_DATA_MORE) {
            if (w < 16) return;
            glink_peek(words, 8, 8);
            wide = 16ULL + words[0];
        }
        wide = (wide + 7ULL) & ~7ULL;
        if (wide > g_glink.theirs_sz - 8) { g_glink.broken = TRUE; return; }
        u32 len = (u32)wide;
        if (w < len) return;

        switch (m.cmd) {
        case GLINK_VERSION:
            if (m.param1 == 1) g_glink.features &= m.param2;
            glink_adv(len);
            glink_say(GLINK_VERSION_ACK, 1, g_glink.features, NULL, 0);
            break;
        case GLINK_VERSION_ACK:
            glink_adv(len);
            if (m.param1 == 1 && m.param2 == g_glink.features) g_glink.agreed = TRUE;
            else if (m.param1) { g_glink.features &= m.param2; glink_say(GLINK_VERSION, 1, g_glink.features, NULL, 0); }
            break;
        case GLINK_OPEN:
            RtlZeroMemory(name, sizeof(name));
            glink_peek(name, sizeof(m), m.param2 < 32 ? m.param2 : 32);
            glink_adv(len);
            if (RtlCompareMemory(name, "apr_audio_svc", 14) == 14) {
                g_glink.apr_ch.theirs = m.param1;
                glink_say(GLINK_OPEN_ACK, (u16)g_glink.apr_ch.theirs, 0, NULL, 0);
                if (!g_glink.apr_ch.asked) {
                    u32 nsz = 14;
                    RtlZeroMemory(name, sizeof(name));
                    RtlCopyMemory(name, "apr_audio_svc", nsz);
                    glink_say(GLINK_OPEN, (u16)g_glink.apr_ch.ours, nsz, name, (nsz + 7u) & ~7u);
                    g_glink.apr_ch.asked = TRUE;
                }
            }
            break;
        case GLINK_OPEN_ACK:
            glink_adv(len);
            if (m.param1 == g_glink.apr_ch.ours) g_glink.apr_ch.acked = TRUE;
            break;
        case GLINK_INTENT:
            if (m.param1 == g_glink.apr_ch.theirs) {
                for (u32 i = 0; i < m.param2; i++) {
                    glink_peek(words, 8 + 8 * i, 8);
                    for (u32 n = 0; n < 64; n++) {
                        if (!g_glink.apr_ch.their_intents[n].there || g_glink.apr_ch.their_intents[n].id == words[1]) {
                            g_glink.apr_ch.their_intents[n].there = TRUE;
                            g_glink.apr_ch.their_intents[n].in_use = FALSE;
                            g_glink.apr_ch.their_intents[n].size = words[0];
                            g_glink.apr_ch.their_intents[n].id = words[1];
                            break;
                        }
                    }
                }
            }
            glink_adv(len);
            break;
        case GLINK_DONE:
        case GLINK_DONE_REUSE:
            if (m.param1 == g_glink.apr_ch.theirs) {
                for (u32 n = 0; n < 64; n++) {
                    if (g_glink.apr_ch.their_intents[n].there && g_glink.apr_ch.their_intents[n].id == m.param2) {
                        g_glink.apr_ch.their_intents[n].in_use = FALSE;
                        g_glink.apr_ch.their_intents[n].there = (m.cmd == GLINK_DONE_REUSE);
                    }
                }
            }
            glink_adv(len);
            break;
        case GLINK_INTENT_REQ:
            glink_adv(len);
            if (m.param1 == g_glink.apr_ch.theirs) {
                BOOLEAN gr = glink_offer(&g_glink.apr_ch, m.param2);
                glink_say(GLINK_INTENT_REQ_ACK, (u16)g_glink.apr_ch.ours, gr ? 1 : 0, NULL, 0);
            }
            break;
        case GLINK_INTENT_REQ_ACK:
            glink_adv(len);
            if (m.param1 == g_glink.apr_ch.theirs || m.param1 == g_glink.apr_ch.ours) {
                g_glink.apr_ch.req_ans = TRUE;
                g_glink.apr_ch.req_ok = (m.param2 != 0);
            }
            break;
        case GLINK_DATA:
        case GLINK_DATA_MORE: {
            u32 idx = m.param2 - 1;
            if (m.param1 == g_glink.apr_ch.theirs && idx < g_glink.apr_ch.our_intent_count &&
                words[0] <= g_glink.apr_ch.our_intents[idx].size - g_glink.apr_ch.our_intents[idx].fill) {
                glink_peek(g_glink.apr_ch.our_intents[idx].data + g_glink.apr_ch.our_intents[idx].fill, 16, words[0]);
                g_glink.apr_ch.our_intents[idx].fill += words[0];
                glink_adv(len);
                if (words[1] == 0) {
                    u32 total = g_glink.apr_ch.our_intents[idx].fill;
                    g_glink.apr_ch.our_intents[idx].fill = 0;
                    apr_deliver(g_glink.apr_ch.our_intents[idx].data, total);
                    glink_say(GLINK_DONE_REUSE, (u16)g_glink.apr_ch.ours, m.param2, NULL, 0);
                }
            } else {
                glink_adv(len);
            }
            break;
        }
        case GLINK_READ_NOTIFY:
            glink_adv(len);
            umi_wr32(g_ipcc, IPCC_SEND, (IPCC_LPASS << 16) | IPCC_GLINK);
            break;
        case GLINK_CLOSE:
            glink_adv(len);
            glink_say(GLINK_CLOSE_ACK, m.param1, 0, NULL, 0);
            if (m.param1 == g_glink.apr_ch.theirs) { g_glink.apr_ch.open = FALSE; g_glink.apr_ch.theirs = 0; }
            break;
        default:
            glink_adv(len);
            break;
        }
        if (g_glink.apr_ch.theirs && g_glink.apr_ch.acked && !g_glink.apr_ch.open) {
            g_glink.apr_ch.open = TRUE;
            for (u32 k = 0; k < 8; k++) glink_offer(&g_glink.apr_ch, 0x200);
            for (u32 k = 0; k < 4; k++) glink_offer(&g_glink.apr_ch, 0x1000);
            umi_log("adsp: glink apr_audio_svc OPEN\n");
        }
    }
}

void umi_adsp_pump(void)
{
    ExAcquireFastMutex(&g_glink_lock);
    glink_pump_locked();
    ExReleaseFastMutex(&g_glink_lock);
}

void umi_adsp_set_events(UMI_APR_EVENT_FN fn) { g_apr_events = fn; }

static BOOLEAN glink_send_apr(const void *data, u32 len)
{
    struct { struct glink_msg m; u32 sz, left; } hdr = { { GLINK_DATA, (u16)g_glink.apr_ch.ours, 0 }, len, 0 };
    BOOLEAN asked = FALSE;
    ExAcquireFastMutex(&g_glink_lock);
    for (u32 w = 0; w < 5000; w++) {
        glink_pump_locked();
        if (!g_glink.up || !g_glink.apr_ch.open || g_glink.broken) { ExReleaseFastMutex(&g_glink_lock); return FALSE; }
        int found = -1;
        BOOLEAN fits = FALSE;
        for (u32 n = 0; n < 64; n++) {
            if (!g_glink.apr_ch.their_intents[n].there || g_glink.apr_ch.their_intents[n].size < len) continue;
            fits = TRUE;
            if (!g_glink.apr_ch.their_intents[n].in_use &&
                (found < 0 || g_glink.apr_ch.their_intents[n].size < g_glink.apr_ch.their_intents[found].size))
                found = (int)n;
        }
        if (found >= 0 && glink_room() >= ((len + (u32)sizeof(hdr) + 7u) & ~7u)) {
            g_glink.apr_ch.their_intents[found].in_use = TRUE;
            hdr.m.param2 = g_glink.apr_ch.their_intents[found].id;
            glink_put(&hdr, sizeof(hdr), data, len);
            ExReleaseFastMutex(&g_glink_lock);
            return TRUE;
        }
        if (!fits && !asked) {
            g_glink.apr_ch.req_ans = FALSE;
            glink_say(GLINK_INTENT_REQ, (u16)g_glink.apr_ch.ours, len, NULL, 0);
            asked = TRUE;
        }
        ExReleaseFastMutex(&g_glink_lock);
        umi_sleep_ms(1);
        ExAcquireFastMutex(&g_glink_lock);
    }
    ExReleaseFastMutex(&g_glink_lock);
    return FALSE;
}

BOOLEAN umi_adsp_apr(u8 svc, u16 port, u32 token, u32 opcode, const void *payload, u32 len, u32 ms, UMI_APR_ANSWER *ans)
{
    u8 pkt[sizeof(struct apr_header) + 256];
    if (len > 256 || !g_glink.apr_ch.open) return FALSE;
    RtlZeroMemory(pkt, sizeof(pkt));
    struct apr_header *h = (struct apr_header *)pkt;
    h->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ, sizeof(*h));
    h->size  = (u16)(sizeof(*h) + len);
    h->from_service = svc; h->from_domain = APR_DOMAIN_APPS; h->from_port = port;
    h->to_service   = svc; h->to_domain   = APR_DOMAIN_ADSP; h->to_port   = port;
    h->opcode = opcode;
    if (len) RtlCopyMemory(pkt + sizeof(*h), payload, len);
    if (ms == 0) {
        h->token = token;
        return glink_send_apr(pkt, (u32)sizeof(*h) + len);
    }
    ExAcquireFastMutex(&g_apr_lock);
    if (token == 0) { token = g_apr_next_token++; if (g_apr_next_token >= 0x01000000u) g_apr_next_token = 1; }
    h->token = token;
    InterlockedExchange(&g_apr_answer_ready, 0);
    g_apr_waited_token = token;
    BOOLEAN ok = FALSE;
    if (glink_send_apr(pkt, (u32)sizeof(*h) + len)) {
        for (u32 w = 0; w < ms; w++) {
            umi_adsp_pump();
            if (InterlockedCompareExchange(&g_apr_answer_ready, 0, 1) == 1 && g_apr_answer.token == token) {
                if (ans) *ans = g_apr_answer;
                ok = TRUE;
                break;
            }
            umi_sleep_ms(1);
        }
    }
    g_apr_waited_token = 0;
    ExReleaseFastMutex(&g_apr_lock);
    return ok;
}

BOOLEAN umi_adsp_start(void)
{
    ExInitializeFastMutex(&g_glink_lock);
    ExInitializeFastMutex(&g_apr_lock);
    g_smem      = (volatile u8 *)umi_map_io(SMEM_PHYS, SMEM_SIZE, TRUE);
    g_aoss      = (volatile u8 *)umi_map_io(AOSS_PHYS, AOSS_SIZE, FALSE);
    g_ipcc      = (volatile u8 *)umi_map_io(IPCC_PHYS, IPCC_SIZE, FALSE);
    g_cmd_db    = (volatile u8 *)umi_map_io(CMD_DB_PHYS, CMD_DB_SIZE, TRUE);
    g_rsc       = (volatile u8 *)umi_map_io(RSC_PHYS, RSC_SIZE, FALSE);
    g_smem_lock = (volatile u8 *)umi_map_io(SMEM_LOCK_PHYS, SMEM_LOCK_SIZE, FALSE);
    if (!g_smem || !g_aoss || !g_ipcc || !g_cmd_db || !g_rsc || !g_smem_lock) {
        umi_log("adsp: MMIO map failed\n");
        return FALSE;
    }

    umi_log("adsp: MMIO mapped, loading adsp.mdt...\n");
    /* 1. Load adsp.mdt */
    u32 mdt_sz = 0;
    u8 *mdt = umi_load_fw("adsp.mdt", &mdt_sz);
    if (!mdt || mdt_sz < 64 || RtlCompareMemory(mdt, "\x7f""ELF\x01", 5) != 5) {
        umi_log("adsp: failed to load adsp.mdt (sz=%u)\n", mdt_sz);
        umi_free_fw(mdt);
        return FALSE;
    }
    u16 phnum = *(u16 *)(mdt + 44);
    u32 lowest = 0xFFFFFFFFu;
    u64 highest = 0;
    BOOLEAN reloc = FALSE;
    for (u32 i = 0; i < phnum; i++) {
        struct elf32_phdr ph;
        RtlCopyMemory(&ph, mdt + 52 + 32 * i, sizeof(ph));
        if (ph.type != 1 || ((ph.flags & (7u << 24)) == (2u << 24)) || ph.mem_size == 0) continue;
        if (ph.flags & (1u << 27)) reloc = TRUE;
        if (ph.paddr < lowest) lowest = ph.paddr;
        if ((u64)ph.paddr + ph.mem_size > highest) highest = (u64)ph.paddr + ph.mem_size;
    }
    u32 seg_base = reloc ? lowest : (u32)UMI_ADSP_MEMORY;
    u32 img_sz = (u32)(((highest - seg_base) + 0xFFFu) & ~0xFFFULL);
    if (img_sz == 0 || img_sz > UMI_ADSP_SIZE) {
        umi_log("adsp: invalid image span 0x%x\n", img_sz);
        umi_free_fw(mdt);
        return FALSE;
    }

    /* 2. PAS INIT_IMAGE + MEM_SETUP */
    PHYSICAL_ADDRESS low = { 0 }, high, bound = { 0 };
    high.QuadPart = 0xFFFFFFFFLL;
    SIZE_T hdr_alloc = (mdt_sz + 0xFFFu) & ~0xFFFu;
    u8 *hdr_page = (u8 *)MmAllocateContiguousMemorySpecifyCache(hdr_alloc, low, high, bound, MmNonCached);
    if (!hdr_page) { umi_free_fw(mdt); return FALSE; }
    RtlZeroMemory(hdr_page, hdr_alloc);
    RtlCopyMemory(hdr_page, mdt, mdt_sz);
    umi_dsb();
    PHYSICAL_ADDRESS hdr_pa = MmGetPhysicalAddress(hdr_page);
    u64 ans = ~0ULL;
    s32 rc = smc_call(SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUF, PAS_ADSP, (u64)hdr_pa.QuadPart, 0, &ans);
    MmFreeContiguousMemorySpecifyCache(hdr_page, hdr_alloc, MmNonCached);
    umi_log("adsp: PAS_INIT_IMAGE rc=%d ans=0x%llx\n", rc, ans);
    if (rc != 0 || ans != 0) { umi_free_fw(mdt); return FALSE; }

    rc = smc_call(SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_ADSP, UMI_ADSP_MEMORY, img_sz, &ans);
    umi_log("adsp: PAS_MEM_SETUP rc=%d ans=0x%llx (size=0x%x)\n", rc, ans, img_sz);
    if (rc != 0 || ans != 0) { umi_free_fw(mdt); return FALSE; }

    /* 3. Load ELF segments into 0x8BB00000 */
    volatile u8 *dsp_mem = (volatile u8 *)umi_map_io(UMI_ADSP_MEMORY, img_sz, TRUE);
    if (!dsp_mem) { umi_free_fw(mdt); return FALSE; }
    for (u32 i = 0; i < phnum; i++) {
        struct elf32_phdr ph;
        RtlCopyMemory(&ph, mdt + 52 + 32 * i, sizeof(ph));
        if (ph.type != 1 || ((ph.flags & (7u << 24)) == (2u << 24)) || ph.mem_size == 0) continue;
        u32 at = ph.paddr - seg_base;
        if (ph.file_size) {
            char name[16];
            name[0]='a'; name[1]='d'; name[2]='s'; name[3]='p'; name[4]='.'; name[5]='b';
            name[6]=(char)('0' + (i / 10)); name[7]=(char)('0' + (i % 10)); name[8]=0;
            u32 fsz = 0;
            u8 *seg = umi_load_fw(name, &fsz);
            if (!seg || fsz != ph.file_size) {
                umi_log("adsp: failed loading %s (got %u exp %u)\n", name, fsz, ph.file_size);
                umi_free_fw(seg); umi_unmap_io((void *)dsp_mem, img_sz); umi_free_fw(mdt);
                return FALSE;
            }
            RtlCopyMemory((void *)(dsp_mem + at), seg, fsz);
            umi_free_fw(seg);
        }
        if (ph.mem_size > ph.file_size) {
            RtlZeroMemory((void *)(dsp_mem + at + ph.file_size), ph.mem_size - ph.file_size);
        }
    }
    umi_dsb();
    umi_unmap_io((void *)dsp_mem, img_sz);
    umi_free_fw(mdt);
    umi_log("adsp: all ELF segments loaded into 0x8BB00000\n");

    /* 4. Prepare SMEM SMP2P + GLINK host queue */
    u32 part = smem_partition(SMEM_US, SMEM_ADSP, NULL, NULL);
    if (!part) { umi_log("adsp: no SMEM partition 0-2\n"); return FALSE; }
    u32 sz = 0;
    g_our_item = smem_item(ITEM_SMP2P_OURS, &sz);
    if (!g_our_item) {
        if (!smem_make_private(part, ITEM_SMP2P_OURS, sizeof(struct smp2p))) return FALSE;
        g_our_item = smem_item(ITEM_SMP2P_OURS, &sz);
    }
    if (!g_our_item) return FALSE;
    for (u32 o = 0; o < sizeof(struct smp2p); o += 4) smem_poke(g_our_item + o, 0);
    umi_dsb();
    smem_poke(g_our_item, SMP2P_MAGIC);
    smem_poke(g_our_item + SMP2P_AT_PROCS, SMEM_US | (SMEM_ADSP << 16));
    u32 mkw[4] = { 0 };
    RtlCopyMemory(mkw, "master-kernel", 14);
    for (u32 w = 0; w < 4; w++) smem_poke(g_our_item + SMP2P_AT_NAME(0) + 4 * w, mkw[w]);
    smem_poke(g_our_item + SMP2P_AT_COUNTS, 16u | (1u << 16));
    smem_poke(g_our_item + SMP2P_AT_VERSION, (SMP2P_RESTART_ACKS << 8) | SMP2P_VERSION);
    umi_dsb();
    g_smp2p_agreed = g_smp2p_racks = g_smp2p_racked = FALSE;

    u32 q = smem_item(ITEM_GLINK_OUR_QUEUE, &sz);
    if (!q) {
        if (!smem_make_private(part, ITEM_GLINK_OUR_QUEUE, 0x4000)) return FALSE;
    }
    u32 desc = smem_item(ITEM_GLINK_DESCRIPTOR, &sz);
    if (desc && sz == 32) {
        smem_poke(desc + GLINK_OUR_HEAD, 0);
        smem_poke(desc + GLINK_THEIR_TAIL, 0);
        umi_dsb();
    }

    /* 5. Vote LCX/LMX rails, tell AOP, and PAS_AUTH_AND_RESET */
    clear_doorbells();
    umi_wr32(g_ipcc, IPCC_ENABLE, (IPCC_LPASS << 16) | IPCC_SMP2P);
    umi_wr32(g_ipcc, IPCC_ENABLE, (IPCC_LPASS << 16) | IPCC_GLINK);
    if (!rpmh_rails(384)) { umi_log("adsp: rpmh vote failed\n"); return FALSE; }
    g_rails_asked = TRUE;
    BOOLEAN qmp_ok = qmp_send("{class: image, res: load_state, name: adsp, val: on}");
    umi_log("adsp: qmp adsp=on ok=%u\n", qmp_ok);
    if (!qmp_ok) { rpmh_rails(0); g_rails_asked = FALSE; return FALSE; }
    rc = smc_call(SCM_PAS_AUTH_RESET, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &ans);
    umi_log("adsp: PAS_AUTH_AND_RESET rc=%d ans=0x%llx\n", rc, ans);
    if (rc != 0 || ans != 0) { rpmh_rails(0); g_rails_asked = FALSE; return FALSE; }
    g_dsp_started = TRUE;

    /* 6. Wait for SMP2P READY + HANDOVER */
    BOOLEAN ready = FALSE, handed = FALSE;
    for (u32 ms = 0; ms < 5000; ms++) {
        clear_doorbells();
        struct smp2p tw;
        if (dsp_words(&tw)) {
            smp2p_listen(&tw);
            u32 st = dsp_state(&tw);
            if (st & DSP_FATAL) { umi_log("adsp: DSP_FATAL state=0x%x\n", st); return FALSE; }
            if ((st & DSP_READY) && !ready) { ready = TRUE; umi_log("adsp: DSP_READY at %u ms\n", ms); }
            if ((st & DSP_HANDOVER) && !handed) {
                handed = TRUE;
                rpmh_rails(0);
                g_rails_asked = FALSE;
                umi_log("adsp: DSP_HANDOVER at %u ms\n", ms);
                break;
            }
        }
        umi_sleep_ms(1);
    }
    if (!ready) { umi_log("adsp: timed out waiting for DSP_READY\n"); return FALSE; }

    /* 7. Open GLINK + apr_audio_svc */
    RtlZeroMemory(&g_glink, sizeof(g_glink));
    g_glink.apr_ch.name = "apr_audio_svc";
    g_glink.apr_ch.ours = 2;
    g_glink.features = 1u; /* GLINK_INTENT_REUSE */
    for (u32 ms = 0; ms < 5000; ms++) {
        u32 dsz = 0;
        g_glink.desc = smem_item(ITEM_GLINK_DESCRIPTOR, &dsz);
        g_glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &g_glink.ours_sz);
        g_glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &g_glink.theirs_sz);
        if (g_glink.desc && dsz == 32 && g_glink.ours && g_glink.theirs) break;
        umi_sleep_ms(1);
    }
    if (!g_glink.desc || !g_glink.ours || !g_glink.theirs) {
        umi_log("adsp: GLINK SMEM items missing\n");
        return FALSE;
    }
    g_glink.up = TRUE;
    ExAcquireFastMutex(&g_glink_lock);
    glink_say(GLINK_VERSION, 1, g_glink.features, NULL, 0);
    ExReleaseFastMutex(&g_glink_lock);
    for (u32 ms = 0; ms < 5000; ms++) {
        umi_adsp_pump();
        if (g_glink.apr_ch.open) {
            /* Give DSP a moment to send its initial intents */
            umi_sleep_ms(50);
            umi_adsp_pump();
            return TRUE;
        }
        umi_sleep_ms(1);
    }
    umi_log("adsp: timed out waiting for apr_audio_svc\n");
    return FALSE;
}

void umi_adsp_stop(void)
{
    g_glink.up = FALSE;
    for (u32 i = 0; i < 24; i++) {
        if (g_glink.apr_ch.our_intents[i].data) {
            ExFreePoolWithTag(g_glink.apr_ch.our_intents[i].data, UMI_TAG);
            g_glink.apr_ch.our_intents[i].data = NULL;
        }
    }
    if (g_dsp_started) {
        u64 ans = 0;
        smc_call(SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &ans);
        g_dsp_started = FALSE;
        qmp_send("{class: image, res: load_state, name: adsp, val: off}");
    }
    if (g_rails_asked) { rpmh_rails(0); g_rails_asked = FALSE; }
    umi_unmap_io((void *)g_smem, SMEM_SIZE);         g_smem = NULL;
    umi_unmap_io((void *)g_aoss, AOSS_SIZE);         g_aoss = NULL;
    umi_unmap_io((void *)g_ipcc, IPCC_SIZE);         g_ipcc = NULL;
    umi_unmap_io((void *)g_cmd_db, CMD_DB_SIZE);     g_cmd_db = NULL;
    umi_unmap_io((void *)g_rsc, RSC_SIZE);           g_rsc = NULL;
    umi_unmap_io((void *)g_smem_lock, SMEM_LOCK_SIZE); g_smem_lock = NULL;
}
