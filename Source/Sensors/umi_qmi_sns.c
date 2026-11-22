/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Xiaomi Mi 10 (umi) QRTR TFTP Registry Server + QMI SNS_CLIENT_SVC (0x190) Protobuf Client */

#include "umi_sensors.h"

#define QRTR_PORT_CTRL      0xFFFFFFFEu
#define QRTR_NODE_BCAST     0xFFFFFFFFu
#define QRTR_TYPE_DATA      1u
#define QRTR_TYPE_NEW_SRV   4u

#define LOCAL_NODE          1u
#define LOCAL_TFTP_PORT     50u
#define LOCAL_SNS_PORT      60u

#define QMI_SVC_SNS_CLIENT  0x190u
#define QMI_SVC_TFTP        4096u /* 0x1000 */

#define QMI_FLAG_REQ        0x00u
#define QMI_FLAG_RESP       0x02u
#define QMI_FLAG_IND        0x04u
#define QMI_SNS_CLIENT_REQ  0x0020u
#define QMI_SNS_REPORT_IND  0x0021u
#define QMI_SNS_JUMBO_IND   0x0022u

#define SNS_SUID_REQ        512u
#define SNS_SUID_EVENT      768u
#define SNS_STD_CONFIG      513u
#define SNS_STD_ON_CHANGE   514u
#define SNS_STD_EVENT       1025u

typedef struct {
    const char *name;
    BOOLEAN     on_change;
    BOOLEAN     has_suid;
    BOOLEAN     subscribed;
    u64         suid_lo;
    u64         suid_hi;
    u32         samples;
} SNS_TRACK;

static SNS_TRACK g_sns[] = {
    { "",              FALSE, FALSE, FALSE, 0, 0, 0 },
    { "timer",         FALSE, FALSE, FALSE, 0, 0, 0 },
    { "accel",         FALSE, FALSE, FALSE, 0, 0, 0 },
    { "gyro",          FALSE, FALSE, FALSE, 0, 0, 0 },
    { "mag",           FALSE, FALSE, FALSE, 0, 0, 0 },
    { "ambient_light", TRUE,  FALSE, FALSE, 0, 0, 0 },
    { "pressure",      FALSE, FALSE, FALSE, 0, 0, 0 },
    { "proximity",     TRUE,  FALSE, FALSE, 0, 0, 0 },
};
#define NUM_SNS (sizeof(g_sns) / sizeof(g_sns[0]))

static u32 g_sns_node = 0, g_sns_port = 0;
static BOOLEAN g_sns_queried = FALSE;
static u32 g_sns_ticks = 0;
static u16 g_qmi_txn = 1;

/* Active TFTP session (SLPI requests registry files one at a time) */
typedef struct {
    BOOLEAN active;
    u32     peer_node;
    u32     peer_port;
    u8     *data;
    u32     size;
    u32     blksize;
    u16     next_blk;
} TFTP_SESS;

static TFTP_SESS g_tftp;

/* --- Minimal IEEE-754 float (u32 bits) -> signed milli-units (no FPU needed) --- */
static s32 f32_to_milli(u32 bits)
{
    int sign = (bits >> 31) & 1;
    int exp  = (int)((bits >> 23) & 0xFF) - 127;
    u32 mant = (bits & 0x7FFFFFu);
    if (exp == -127) return 0; /* zero/subnormal */
    if (exp > 20) return sign ? -999999999 : 999999999;
    u64 v = (1ULL << 23) | mant;
    /* value = v * 2^(exp - 23); milli = v * 1000 * 2^(exp - 23) */
    v *= 1000ULL;
    int sh = exp - 23;
    if (sh >= 0) v <<= sh;
    else if (sh > -63) v >>= (-sh);
    else v = 0;
    s32 res = (s32)v;
    return sign ? -res : res;
}

/* --- Minimal Protobuf Encoder --- */
typedef struct { u8 *buf; u32 pos, cap; } PB_ENC;

static void pb_varint(PB_ENC *e, u64 v)
{
    while (v >= 0x80 && e->pos < e->cap) {
        e->buf[e->pos++] = (u8)((v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    if (e->pos < e->cap) e->buf[e->pos++] = (u8)v;
}

static void pb_tag(PB_ENC *e, u32 field, u32 wire) { pb_varint(e, ((u64)field << 3) | wire); }

static void pb_fixed32(PB_ENC *e, u32 field, u32 v)
{
    pb_tag(e, field, 5);
    if (e->pos + 4 <= e->cap) { RtlCopyMemory(e->buf + e->pos, &v, 4); e->pos += 4; }
}

static void pb_fixed64(PB_ENC *e, u32 field, u64 v)
{
    pb_tag(e, field, 1);
    if (e->pos + 8 <= e->cap) { RtlCopyMemory(e->buf + e->pos, &v, 8); e->pos += 8; }
}

static void pb_bytes(PB_ENC *e, u32 field, const void *data, u32 len)
{
    pb_tag(e, field, 2);
    pb_varint(e, len);
    if (len && e->pos + len <= e->cap) { RtlCopyMemory(e->buf + e->pos, data, len); e->pos += len; }
}

/* --- Minimal Protobuf Decoder --- */
typedef struct { const u8 *buf; u32 pos, len; } PB_DEC;

static BOOLEAN pb_read_varint(PB_DEC *d, u64 *out)
{
    u64 v = 0;
    u32 sh = 0;
    while (d->pos < d->len && sh < 64) {
        u8 b = d->buf[d->pos++];
        v |= ((u64)(b & 0x7Fu)) << sh;
        if (!(b & 0x80u)) { *out = v; return TRUE; }
        sh += 7;
    }
    return FALSE;
}

static BOOLEAN pb_skip(PB_DEC *d, u32 wire)
{
    u64 v = 0;
    if (wire == 0) return pb_read_varint(d, &v);
    if (wire == 1) { if (d->pos + 8 > d->len) return FALSE; d->pos += 8; return TRUE; }
    if (wire == 2) { if (!pb_read_varint(d, &v) || d->pos + v > d->len) return FALSE; d->pos += (u32)v; return TRUE; }
    if (wire == 5) { if (d->pos + 4 > d->len) return FALSE; d->pos += 4; return TRUE; }
    return FALSE;
}

static BOOLEAN pb_decode_suid(const u8 *buf, u32 len, u64 *lo, u64 *hi)
{
    PB_DEC d = { buf, 0, len };
    *lo = 0; *hi = 0;
    while (d.pos < d.len) {
        u64 tag = 0;
        if (!pb_read_varint(&d, &tag)) return FALSE;
        u32 f = (u32)(tag >> 3), w = (u32)(tag & 7);
        if (f == 1 && w == 1 && d.pos + 8 <= d.len) { RtlCopyMemory(lo, d.buf + d.pos, 8); d.pos += 8; }
        else if (f == 2 && w == 1 && d.pos + 8 <= d.len) { RtlCopyMemory(hi, d.buf + d.pos, 8); d.pos += 8; }
        else if (!pb_skip(&d, w)) return FALSE;
    }
    return (*lo != 0 || *hi != 0);
}

/* --- Send QMI SNS_CLIENT_REQ (0x0020) wrapping a Protobuf sns_client_request_msg --- */
static BOOLEAN sns_send_req(u64 suid_lo, u64 suid_hi, u32 msg_id, const u8 *inner_pb, u32 inner_len)
{
    if (!g_sns_node || !g_sns_port) return FALSE;

    u8 suid_buf[32], susp_buf[16], req_buf[128], pb[256];
    PB_ENC e;

    e.buf = suid_buf; e.pos = 0; e.cap = sizeof(suid_buf);
    pb_fixed64(&e, 1, suid_lo);
    pb_fixed64(&e, 2, suid_hi);
    u32 suid_len = e.pos;

    e.buf = susp_buf; e.pos = 0; e.cap = sizeof(susp_buf);
    pb_tag(&e, 1, 0); pb_varint(&e, 1); /* APSS */
    pb_tag(&e, 2, 0); pb_varint(&e, 0); /* DELIVERY_WAKEUP */
    u32 susp_len = e.pos;

    e.buf = req_buf; e.pos = 0; e.cap = sizeof(req_buf);
    if (inner_pb && inner_len) pb_bytes(&e, 2, inner_pb, inner_len);
    u32 req_len = e.pos;

    e.buf = pb; e.pos = 0; e.cap = sizeof(pb);
    pb_bytes(&e, 1, suid_buf, suid_len);
    pb_fixed32(&e, 2, msg_id);
    pb_bytes(&e, 3, susp_buf, susp_len);
    pb_bytes(&e, 4, req_buf, req_len);
    u32 pb_len = e.pos;

    /* QMI header (7 bytes) + TLV 0x01 (1 + 2 + 2 + pb_len) — omit jumbo TLV so SLPI sends normal 0x0021 IND */
    u8 qmi[320];
    u32 tlv1_len = 2 + pb_len;
    u32 qmi_payload_len = 3 + tlv1_len;
    qmi[0] = QMI_FLAG_REQ;
    u16 txn = g_qmi_txn++;
    qmi[1] = (u8)(txn & 0xFF); qmi[2] = (u8)(txn >> 8);
    qmi[3] = (u8)(QMI_SNS_CLIENT_REQ & 0xFF); qmi[4] = (u8)(QMI_SNS_CLIENT_REQ >> 8);
    qmi[5] = (u8)(qmi_payload_len & 0xFF); qmi[6] = (u8)(qmi_payload_len >> 8);

    u32 p = 7;
    qmi[p++] = 0x01;
    qmi[p++] = (u8)(tlv1_len & 0xFF); qmi[p++] = (u8)(tlv1_len >> 8);
    qmi[p++] = (u8)(pb_len & 0xFF);   qmi[p++] = (u8)(pb_len >> 8);
    RtlCopyMemory(qmi + p, pb, pb_len); p += pb_len;

    return umi_qrtr_send(QRTR_TYPE_DATA, LOCAL_SNS_PORT, g_sns_node, g_sns_port, qmi, p);
}

static void sns_query_suid(const char *dtype)
{
    u8 inner[64];
    PB_ENC e = { inner, 0, sizeof(inner) };
    u32 slen = 0;
    while (dtype[slen]) slen++;
    pb_bytes(&e, 1, dtype, slen);
    pb_tag(&e, 2, 0); pb_varint(&e, 1); /* register_updates = true */
    pb_tag(&e, 3, 0); pb_varint(&e, 0); /* default_only = false */
    BOOLEAN ok = sns_send_req(0xababababababababULL, 0xababababababababULL, SNS_SUID_REQ, inner, e.pos);
    umi_log("sns: SUID_REQ '%s' ok=%u\n", dtype, ok);
}

static void sns_subscribe(SNS_TRACK *t)
{
    if (t->subscribed || !t->has_suid) return;
    t->subscribed = TRUE;
    if (t->on_change) {
        BOOLEAN ok = sns_send_req(t->suid_lo, t->suid_hi, SNS_STD_ON_CHANGE, NULL, 0);
        umi_log("sns: ON_CHANGE_CONFIG '%s' ok=%u\n", t->name, ok);
    } else {
        u8 inner[16];
        PB_ENC e = { inner, 0, sizeof(inner) };
        pb_fixed32(&e, 1, 0x41200000u); /* 10.0f Hz */
        BOOLEAN ok = sns_send_req(t->suid_lo, t->suid_hi, SNS_STD_CONFIG, inner, e.pos);
        umi_log("sns: STD_CONFIG '%s' 10Hz ok=%u\n", t->name, ok);
    }
}

static void sns_handle_suid_event(const u8 *buf, u32 len)
{
    PB_DEC d = { buf, 0, len };
    char dtype[32];
    RtlZeroMemory(dtype, sizeof(dtype));
    u64 lo = 0, hi = 0;
    BOOLEAN got = FALSE;
    while (d.pos < d.len) {
        u64 tag = 0;
        if (!pb_read_varint(&d, &tag)) break;
        u32 f = (u32)(tag >> 3), w = (u32)(tag & 7);
        if (f == 1 && w == 2) {
            u64 l = 0;
            if (!pb_read_varint(&d, &l) || d.pos + l > d.len) break;
            u32 cp = (u32)(l < 31 ? l : 31);
            RtlCopyMemory(dtype, d.buf + d.pos, cp);
            dtype[cp] = 0;
            d.pos += (u32)l;
        } else if (f == 2 && w == 2) {
            u64 l = 0;
            if (!pb_read_varint(&d, &l) || d.pos + l > d.len) break;
            if (!got && pb_decode_suid(d.buf + d.pos, (u32)l, &lo, &hi)) got = TRUE;
            d.pos += (u32)l;
        } else if (!pb_skip(&d, w)) break;
    }
    if (dtype[0] != 0 || got) {
        umi_log("sns: SUID_EVENT '%s' got=%u suid=%llx:%llx\n", dtype, got, hi, lo);
    }
    if (!got) return;
    if (dtype[0] == 0 || (dtype[0] == 't' && dtype[1] == 'i')) return; /* don't subscribe to "" or "timer" */
    for (u32 i = 0; i < NUM_SNS; i++) {
        u32 k = 0;
        while (g_sns[i].name[k] && dtype[k] && g_sns[i].name[k] == dtype[k]) k++;
        if (g_sns[i].name[k] == 0 && dtype[k] == 0) {
            g_sns[i].has_suid = TRUE;
            g_sns[i].suid_lo = lo;
            g_sns[i].suid_hi = hi;
            sns_subscribe(&g_sns[i]);
            break;
        }
    }
}

static void sns_handle_std_event(u64 suid_lo, u64 suid_hi, const u8 *buf, u32 len)
{
    SNS_TRACK *t = NULL;
    for (u32 i = 0; i < NUM_SNS; i++) {
        if (g_sns[i].has_suid && g_sns[i].suid_lo == suid_lo && g_sns[i].suid_hi == suid_hi) {
            t = &g_sns[i]; break;
        }
    }
    if (!t) return;
    t->samples++;

    PB_DEC d = { buf, 0, len };
    s32 vals[6] = { 0 };
    u32 nvals = 0;
    u32 status = 0;
    while (d.pos < d.len) {
        u64 tag = 0;
        if (!pb_read_varint(&d, &tag)) break;
        u32 f = (u32)(tag >> 3), w = (u32)(tag & 7);
        if (f == 1 && w == 5 && d.pos + 4 <= d.len) {
            u32 bits = 0; RtlCopyMemory(&bits, d.buf + d.pos, 4); d.pos += 4;
            if (nvals < 6) vals[nvals++] = f32_to_milli(bits);
        } else if (f == 1 && w == 2) { /* packed repeated float */
            u64 l = 0;
            if (!pb_read_varint(&d, &l) || d.pos + l > d.len) break;
            u32 end = d.pos + (u32)l;
            while (d.pos + 4 <= end) {
                u32 bits = 0; RtlCopyMemory(&bits, d.buf + d.pos, 4); d.pos += 4;
                if (nvals < 6) vals[nvals++] = f32_to_milli(bits);
            }
            d.pos = end;
        } else if (f == 2 && w == 0) {
            u64 st = 0; if (pb_read_varint(&d, &st)) status = (u32)st;
        } else if (!pb_skip(&d, w)) break;
    }
    if (t->samples <= 5) {
        umi_log("sns: DATA '%s' #%u n=%u st=%u v=[%d, %d, %d]\n",
                t->name, t->samples, nvals, status, vals[0], vals[1], vals[2]);
    }
    /* Forward live sample to Windows 11 VHF HID Sensor Collection */
    if (t->name[0] == 'a' && t->name[1] == 'c')      umi_hid_submit_sensor(1u, vals[0], vals[1], vals[2]);
    else if (t->name[0] == 'g')                      umi_hid_submit_sensor(2u, vals[0], vals[1], vals[2]);
    else if (t->name[0] == 'm')                      umi_hid_submit_sensor(3u, vals[0], vals[1], vals[2]);
    else if (t->name[0] == 'a' && t->name[1] == 'm') umi_hid_submit_sensor(4u, vals[0], vals[1], vals[2]);
    else if (t->name[0] == 'p' && t->name[1] == 'r' && t->name[2] == 'e') umi_hid_submit_sensor(5u, vals[0], vals[1], vals[2]);
}

static void sns_handle_client_event_msg(const u8 *pb, u32 pb_len)
{
    PB_DEC d = { pb, 0, pb_len };
    u64 suid_lo = 0, suid_hi = 0;
    while (d.pos < d.len) {
        u64 tag = 0;
        if (!pb_read_varint(&d, &tag)) break;
        u32 f = (u32)(tag >> 3), w = (u32)(tag & 7);
        if (f == 1 && w == 2) {
            u64 l = 0;
            if (!pb_read_varint(&d, &l) || d.pos + l > d.len) break;
            pb_decode_suid(d.buf + d.pos, (u32)l, &suid_lo, &suid_hi);
            d.pos += (u32)l;
        } else if (f == 2 && w == 2) {
            u64 l = 0;
            if (!pb_read_varint(&d, &l) || d.pos + l > d.len) break;
            PB_DEC ev = { d.buf + d.pos, 0, (u32)l };
            u32 ev_msg_id = 0;
            const u8 *ev_pay = NULL;
            u32 ev_pay_len = 0;
            while (ev.pos < ev.len) {
                u64 et = 0;
                if (!pb_read_varint(&ev, &et)) break;
                u32 ef = (u32)(et >> 3), ew = (u32)(et & 7);
                if (ef == 1 && ew == 5 && ev.pos + 4 <= ev.len) {
                    RtlCopyMemory(&ev_msg_id, ev.buf + ev.pos, 4);
                    ev.pos += 4;
                } else if (ef == 1 && ew == 0) {
                    u64 v = 0; if (pb_read_varint(&ev, &v)) ev_msg_id = (u32)v;
                } else if (ef == 3 && ew == 2) {
                    u64 pl = 0;
                    if (!pb_read_varint(&ev, &pl) || ev.pos + pl > ev.len) break;
                    ev_pay = ev.buf + ev.pos; ev_pay_len = (u32)pl;
                    ev.pos += (u32)pl;
                } else if (!pb_skip(&ev, ew)) break;
            }
            if (ev_msg_id == SNS_SUID_EVENT && ev_pay) sns_handle_suid_event(ev_pay, ev_pay_len);
            else if (ev_msg_id == SNS_STD_EVENT && ev_pay) sns_handle_std_event(suid_lo, suid_hi, ev_pay, ev_pay_len);
            d.pos += (u32)l;
        } else if (!pb_skip(&d, w)) break;
    }
}

static void sns_handle_qmi(const u8 *data, u32 len)
{
    if (len < 7) return;
    u8 flags = data[0];
    u16 txn = (u16)(data[1] | (data[2] << 8));
    u16 mid = (u16)(data[3] | (data[4] << 8));
    u16 mlen = (u16)(data[5] | (data[6] << 8));
    if (7u + mlen > len) mlen = (u16)(len - 7u);

    if (flags == QMI_FLAG_RESP) {
        u16 qres = 0xFFFF, qerr = 0xFFFF;
        u64 cid = 0;
        u32 serr = 0xFFFFFFFF;
        u32 p = 7;
        while (p + 3 <= 7u + mlen) {
            u8 tag = data[p];
            u16 tlen = (u16)(data[p + 1] | (data[p + 2] << 8));
            p += 3;
            if (p + tlen > 7u + mlen) break;
            if (tag == 0x02 && tlen >= 4) {
                qres = (u16)(data[p] | (data[p + 1] << 8));
                qerr = (u16)(data[p + 2] | (data[p + 3] << 8));
            } else if (tag == 0x10 && tlen >= 8) {
                RtlCopyMemory(&cid, data + p, 8);
            } else if (tag == 0x11 && tlen >= 4) {
                RtlCopyMemory(&serr, data + p, 4);
            }
            p += tlen;
        }
        umi_log("sns: QMI RESP txn=%u mid=0x%x len=%u res=%u err=%u cid=%llx serr=%u\n",
                txn, mid, mlen, qres, qerr, cid, serr);
        return;
    }
    if (flags != QMI_FLAG_IND || (mid != QMI_SNS_REPORT_IND && mid != QMI_SNS_JUMBO_IND)) {
        umi_log("sns: QMI other fl=0x%x txn=%u mid=0x%x len=%u\n", flags, txn, mid, mlen);
        return;
    }

    u32 p = 7;
    while (p + 3 <= 7u + mlen) {
        u8 tag = data[p];
        u16 tlen = (u16)(data[p + 1] | (data[p + 2] << 8));
        p += 3;
        if (p + tlen > 7u + mlen) break;
        if (tag == 0x02 && tlen >= 2) {
            u16 alen = (u16)(data[p] | (data[p + 1] << 8));
            if (2u + alen <= tlen) {
                sns_handle_client_event_msg(data + p + 2, alen);
            } else if (tlen >= 4) {
                u32 alen32 = *(const u32 *)(data + p);
                if (4u + alen32 <= tlen) sns_handle_client_event_msg(data + p + 4, alen32);
            }
        }
        p += tlen;
    }
}

/* --- Minimal QRTR TFTP Server (port 50, svc 4096) for SLPI Registry --- */
static void tftp_send_error(u32 dnode, u32 dport, u16 code, const char *msg)
{
    u8 pkt[64];
    pkt[0] = 0; pkt[1] = 5; /* ERROR */
    pkt[2] = (u8)(code >> 8); pkt[3] = (u8)(code & 0xFF);
    u32 p = 4;
    while (*msg && p < 62) pkt[p++] = (u8)*msg++;
    pkt[p++] = 0;
    umi_qrtr_send(QRTR_TYPE_DATA, LOCAL_TFTP_PORT, dnode, dport, pkt, p);
}

static void tftp_send_block(void)
{
    if (!g_tftp.active) return;
    u32 off = (u32)(g_tftp.next_blk - 1u) * g_tftp.blksize;
    u32 chunk = (off < g_tftp.size) ? (g_tftp.size - off) : 0;
    if (chunk > g_tftp.blksize) chunk = g_tftp.blksize;
    u8 *pkt = (u8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 4 + chunk, UMI_TAG);
    if (!pkt) return;
    pkt[0] = 0; pkt[1] = 3; /* DATA */
    pkt[2] = (u8)(g_tftp.next_blk >> 8); pkt[3] = (u8)(g_tftp.next_blk & 0xFF);
    if (chunk) RtlCopyMemory(pkt + 4, g_tftp.data + off, chunk);
    umi_qrtr_send(QRTR_TYPE_DATA, LOCAL_TFTP_PORT, g_tftp.peer_node, g_tftp.peer_port, pkt, 4 + chunk);
    ExFreePoolWithTag(pkt, UMI_TAG);
    if (chunk < g_tftp.blksize) {
        umi_free_fw(g_tftp.data);
        g_tftp.data = NULL;
        g_tftp.active = FALSE;
    }
}

static void tftp_handle(u32 snode, u32 sport, const u8 *data, u32 len)
{
    if (len < 4) return;
    u16 op = (u16)((data[0] << 8) | data[1]);
    if (op == 1) { /* RRQ */
        const char *fname = (const char *)(data + 2);
        u32 flen = 0;
        while (2 + flen < len && fname[flen]) flen++;
        /* Extract basename after last '/' */
        const char *base = fname;
        for (u32 i = 0; i < flen; i++) if (fname[i] == '/') base = fname + i + 1;
        char rel[128];
        const char *pfx = "registry\\";
        u32 idx = 0;
        while (pfx[idx]) { rel[idx] = pfx[idx]; idx++; }
        for (u32 i = 0; base[i] && idx < 126; i++) rel[idx++] = base[i];
        rel[idx] = 0;

        if (g_tftp.data) { umi_free_fw(g_tftp.data); g_tftp.data = NULL; }
        g_tftp.active = FALSE;

        u32 fsz = 0;
        u8 *fbuf = umi_load_fw(rel, &fsz);
        umi_log("tftp: RRQ '%s' -> '%s' found=%u sz=%u\n", fname, rel, fbuf != NULL, fsz);
        if (!fbuf) {
            tftp_send_error(snode, sport, 1, "File not found");
            return;
        }
        g_tftp.active = TRUE;
        g_tftp.peer_node = snode;
        g_tftp.peer_port = sport;
        g_tftp.data = fbuf;
        g_tftp.size = fsz;
        g_tftp.blksize = 512;
        g_tftp.next_blk = 1;

        /* Check for RFC 2347 options (blksize / tsize) after mode string */
        u32 p = 2 + flen + 1;
        while (p < len && data[p]) p++;
        p++; /* skip mode NUL */
        BOOLEAN has_opt = FALSE;
        u8 oack[128];
        u32 op_pos = 2;
        oack[0] = 0; oack[1] = 6; /* OACK */
        while (p < len) {
            const char *k = (const char *)(data + p);
            while (p < len && data[p]) p++;
            p++;
            if (p >= len) break;
            const char *v = (const char *)(data + p);
            while (p < len && data[p]) p++;
            p++;
            if (k[0] == 'b' && k[1] == 'l' && k[2] == 'k') {
                u32 bs = 0;
                for (u32 j = 0; v[j] >= '0' && v[j] <= '9'; j++) bs = bs * 10 + (u32)(v[j] - '0');
                if (bs >= 512 && bs <= 4096) g_tftp.blksize = bs;
                const char *bk = "blksize";
                for (u32 j = 0; bk[j]; j++) oack[op_pos++] = (u8)bk[j];
                oack[op_pos++] = 0;
                char nb[12]; int ni = 0; u32 tmp = g_tftp.blksize;
                do { nb[ni++] = (char)('0' + (tmp % 10)); tmp /= 10; } while (tmp);
                while (ni > 0) oack[op_pos++] = (u8)nb[--ni];
                oack[op_pos++] = 0;
                has_opt = TRUE;
            } else if (k[0] == 't' && k[1] == 's' && k[2] == 'i') {
                const char *tk = "tsize";
                for (u32 j = 0; tk[j]; j++) oack[op_pos++] = (u8)tk[j];
                oack[op_pos++] = 0;
                char nb[12]; int ni = 0; u32 tmp = g_tftp.size;
                do { nb[ni++] = (char)('0' + (tmp % 10)); tmp /= 10; } while (tmp);
                while (ni > 0) oack[op_pos++] = (u8)nb[--ni];
                oack[op_pos++] = 0;
                has_opt = TRUE;
            }
        }
        if (has_opt) {
            umi_qrtr_send(QRTR_TYPE_DATA, LOCAL_TFTP_PORT, snode, sport, oack, op_pos);
        } else {
            tftp_send_block();
        }
    } else if (op == 4 && g_tftp.active) { /* ACK */
        u16 blk = (u16)((data[2] << 8) | data[3]);
        if (blk == 0) {
            g_tftp.next_blk = 1;
            tftp_send_block();
        } else if (blk == g_tftp.next_blk) {
            g_tftp.next_blk++;
            tftp_send_block();
        }
    } else {
        umi_log("tftp: op=%u from %u:%u len=%u\n", op, snode, sport, len);
    }
}

void umi_sns_init(void)
{
    g_sns_node = 0; g_sns_port = 0; g_sns_queried = FALSE; g_sns_ticks = 0;
    g_qmi_txn = 1;
    RtlZeroMemory(&g_tftp, sizeof(g_tftp));
    for (u32 i = 0; i < NUM_SNS; i++) {
        g_sns[i].has_suid = FALSE; g_sns[i].subscribed = FALSE; g_sns[i].samples = 0;
    }
}

void umi_sns_publish_tftp(void)
{
    /* Publish TFTP service (4096 = 0x1000) for SLPI registry access */
    u32 ns[5] = { QRTR_TYPE_NEW_SRV, QMI_SVC_TFTP, 0x1u, LOCAL_NODE, LOCAL_TFTP_PORT };
    umi_qrtr_send(QRTR_TYPE_NEW_SRV, QRTR_PORT_CTRL, QRTR_NODE_BCAST, QRTR_PORT_CTRL, ns, sizeof(ns));
    ns[2] = 0x3u;
    umi_qrtr_send(QRTR_TYPE_NEW_SRV, QRTR_PORT_CTRL, QRTR_NODE_BCAST, QRTR_PORT_CTRL, ns, sizeof(ns));
}

void umi_sns_on_qrtr(u32 type, u32 snode, u32 sport, u32 dport, const u8 *payload, u32 len)
{
    if (type == QRTR_TYPE_NEW_SRV && len >= 20) {
        const u32 *w = (const u32 *)payload;
        if (w[1] == QMI_SVC_SNS_CLIENT) {
            g_sns_node = w[3];
            g_sns_port = w[4];
            umi_log("sns: discovered SNS_CLIENT_SVC at %u:%u\n", g_sns_node, g_sns_port);
        }
    } else if (type == QRTR_TYPE_DATA) {
        if (dport == LOCAL_SNS_PORT) sns_handle_qmi(payload, len);
        else if (dport == LOCAL_TFTP_PORT) tftp_handle(snode, sport, payload, len);
        else umi_log("sns: unhandled QRTR DATA %u:%u -> :%u len=%u\n", snode, sport, dport, len);
    }
}

void umi_sns_tick(void)
{
    if (!g_sns_node || !g_sns_port) return;
    g_sns_ticks++;
    if (!g_sns_queried) {
        g_sns_queried = TRUE;
        /* Also send a QMI_TEST_PING_REQ (0x0020) to 9:1 (svc=0xf) to verify QMI/QRTR round-trip */
        u8 ping[14] = { 0x00, 0x01, 0x00, 0x20, 0x00, 0x07, 0x00, 0x01, 0x04, 0x00, 'p', 'i', 'n', 'g' };
        BOOLEAN pok = umi_qrtr_send(QRTR_TYPE_DATA, LOCAL_SNS_PORT, g_sns_node, 1u, ping, sizeof(ping));
        umi_log("sns: QMI_TEST ping -> %u:1 ok=%u\n", g_sns_node, pok);
        umi_sleep_ms(30);
        umi_slpi_pump();
        for (u32 i = 0; i < NUM_SNS; i++) {
            sns_query_suid(g_sns[i].name);
            umi_sleep_ms(20);
            umi_slpi_pump();
        }
    } else if (g_sns_ticks == 400) {
        /* Re-query missing SUIDs after FastRPC finishes loading the sensor registry */
        for (u32 i = 2; i < NUM_SNS; i++) {
            if (!g_sns[i].has_suid) {
                sns_query_suid(g_sns[i].name);
                umi_sleep_ms(20);
                umi_slpi_pump();
            }
        }
    }
}
