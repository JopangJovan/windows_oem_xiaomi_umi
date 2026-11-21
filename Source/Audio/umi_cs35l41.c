/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Xiaomi Mi 10 (UMI) Dual Cirrus Logic CS35L41 Stereo Speaker Driver (Windows ARM64)
 * Ported from hardware-verified HackiOS UMI implementation.
 * QUPv3 SE3 bit-banged I2C: GPIO 119 (SDA), GPIO 120 (SCL), GPIO 114 (RESET) on TLMM NORTH (0x0F900000).
 * Amps: 0x40 (bottom/right), 0x42 (top/left receiver).
 */
#include "umi_hw.h"
#include "umi_cs35l41_otp.h"
#include "umi_cs35l41_audio.h"

#define UMI_TLMM_NORTH_PHYS 0x0F900000ULL
#define UMI_TLMM_SIZE       0x00100000u
#define UMI_I2C_SDA         119u
#define UMI_I2C_SCL         120u
#define UMI_AMP_RESET       114u
#define UMI_CS35L41_CHIP_ID 0x00035A40u

#define GPIO_CFG(pin)       ((pin) * 0x1000u)
#define GPIO_IO(pin)        ((pin) * 0x1000u + 4u)
#define CFG_OUTPUT          (1u << 9)

static volatile u8 *g_tlmm_north;
static u32 g_saved_cfg[2], g_saved_io[2];
static u32 g_reset_cfg, g_reset_io;
static u32 g_halo_xm[2], g_cspl_xm[2];
static BOOLEAN g_unit_cal[2];
static BOOLEAN g_output_ready, g_output_on, g_reset_asserted;
static u32 g_volume_atten = 320; /* -40 dB initial */

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void i2c_line(u32 pin, BOOLEAN release)
{
    umi_wr32(g_tlmm_north, GPIO_IO(pin), 0);
    umi_wr32(g_tlmm_north, GPIO_CFG(pin), release ? 0 : CFG_OUTPUT);
}

static BOOLEAN i2c_level(u32 pin)
{
    return (umi_rd32(g_tlmm_north, GPIO_IO(pin)) & 1u) != 0;
}

static BOOLEAN i2c_take(void)
{
    u32 pins[2] = { UMI_I2C_SDA, UMI_I2C_SCL };
    for (u32 i = 0; i < 2; i++) {
        g_saved_cfg[i] = umi_rd32(g_tlmm_north, GPIO_CFG(pins[i]));
        g_saved_io[i]  = umi_rd32(g_tlmm_north, GPIO_IO(pins[i]));
    }
    i2c_line(UMI_I2C_SDA, TRUE);
    i2c_line(UMI_I2C_SCL, TRUE);
    umi_spin_us(10);
    if (!i2c_level(UMI_I2C_SDA) || !i2c_level(UMI_I2C_SCL)) {
        for (u32 i = 0; i < 2; i++) {
            umi_wr32(g_tlmm_north, GPIO_IO(pins[i]), g_saved_io[i] & 2u);
            umi_wr32(g_tlmm_north, GPIO_CFG(pins[i]), g_saved_cfg[i]);
        }
        return FALSE;
    }
    return TRUE;
}

static void i2c_give(void)
{
    u32 pins[2] = { UMI_I2C_SDA, UMI_I2C_SCL };
    for (u32 i = 0; i < 2; i++) {
        umi_wr32(g_tlmm_north, GPIO_IO(pins[i]), g_saved_io[i] & 2u);
        umi_wr32(g_tlmm_north, GPIO_CFG(pins[i]), g_saved_cfg[i]);
    }
}

static void i2c_start(void)
{
    i2c_line(UMI_I2C_SDA, TRUE);
    i2c_line(UMI_I2C_SCL, TRUE);
    umi_spin_us(5);
    i2c_line(UMI_I2C_SDA, FALSE);
    umi_spin_us(5);
    i2c_line(UMI_I2C_SCL, FALSE);
    umi_spin_us(5);
}

static void i2c_stop(void)
{
    i2c_line(UMI_I2C_SDA, FALSE);
    umi_spin_us(5);
    i2c_line(UMI_I2C_SCL, TRUE);
    umi_spin_us(5);
    i2c_line(UMI_I2C_SDA, TRUE);
    umi_spin_us(5);
}

static BOOLEAN i2c_bit(BOOLEAN out)
{
    i2c_line(UMI_I2C_SDA, out);
    umi_spin_us(5);
    i2c_line(UMI_I2C_SCL, TRUE);
    for (u32 w = 0; !i2c_level(UMI_I2C_SCL) && w < 100; w++) umi_spin_us(5);
    umi_spin_us(5);
    BOOLEAN in = i2c_level(UMI_I2C_SDA);
    i2c_line(UMI_I2C_SCL, FALSE);
    return in;
}

static BOOLEAN i2c_write_byte(u8 b)
{
    for (int bit = 7; bit >= 0; bit--) i2c_bit(((b >> bit) & 1) != 0);
    return !i2c_bit(TRUE);
}

static u8 i2c_read_byte(BOOLEAN more)
{
    u8 b = 0;
    for (int bit = 7; bit >= 0; bit--) b = (u8)((b << 1) | (i2c_bit(TRUE) ? 1 : 0));
    i2c_bit(!more);
    return b;
}

static BOOLEAN cs_bytes(u8 addr, u32 reg, u8 *bytes, u32 len, BOOLEAN write)
{
    if (len == 0 || len > 60 || (len & 3) || (reg & 3) || !i2c_take()) return FALSE;
    i2c_start();
    BOOLEAN ack = i2c_write_byte((u8)(addr << 1));
    for (int s = 24; ack && s >= 0; s -= 8) ack = i2c_write_byte((u8)(reg >> s));
    if (!write && ack) {
        i2c_start();
        ack = i2c_write_byte((u8)((addr << 1) | 1));
    }
    for (u32 i = 0; ack && i < len; i++) {
        if (write) ack = i2c_write_byte(bytes[i]);
        else bytes[i] = i2c_read_byte(i + 1 < len);
    }
    i2c_stop();
    i2c_give();
    return ack;
}

static BOOLEAN cs_read(u8 addr, u32 reg, u32 *val)
{
    u8 d[4];
    if (!cs_bytes(addr, reg, d, 4, FALSE)) return FALSE;
    *val = be32(d);
    return TRUE;
}

static BOOLEAN cs_write(u8 addr, u32 reg, u32 val)
{
    u8 d[4] = { (u8)(val >> 24), (u8)(val >> 16), (u8)(val >> 8), (u8)val };
    BOOLEAN ok = cs_bytes(addr, reg, d, 4, TRUE);
    if (!ok) umi_log("cs35l41: amp 0x%x write failed reg 0x%x val 0x%x\n", addr, reg, val);
    return ok;
}

static BOOLEAN cs_update(u8 addr, u32 reg, u32 mask, u32 val)
{
    u32 old = 0, chk = 0;
    BOOLEAN ok = cs_read(addr, reg, &old) &&
                 cs_write(addr, reg, (old & ~mask) | (val & mask)) &&
                 cs_read(addr, reg, &chk) && ((chk & mask) == (val & mask));
    if (!ok) umi_log("cs35l41: amp 0x%x update failed reg 0x%x old 0x%x chk 0x%x\n", addr, reg, old, chk);
    return ok;
}

static BOOLEAN cs_memory(u8 addr, u32 reg, const u8 *data, u32 len)
{
    u8 chunk[60], rb[60];
    for (u32 done = 0; done < len;) {
        u32 n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        RtlCopyMemory(chunk, data + done, n);
        if (!cs_bytes(addr, reg + done, chunk, n, TRUE) ||
            !cs_bytes(addr, reg + done, rb, n, FALSE) ||
            RtlCompareMemory(chunk, rb, n) != n) {
            umi_log("cs35l41: amp 0x%x memory verify failed at 0x%x\n", addr, reg + done);
            return FALSE;
        }
        done += n;
        umi_spin_us(50);
    }
    return TRUE;
}

static BOOLEAN cs_region(u32 type, u32 off, u32 len, BOOLEAN coeff, u32 *reg)
{
    u32 base, limit, scale;
    switch (type) {
    case 0x10: base = 0x03800000; limit = 0x4fec; scale = 5; break;
    case 0x11: base = 0x02000000; limit = 0x2ff4; scale = 3; break;
    case 0x12: base = 0x02c00000; limit = 0x17f4; scale = 3; break;
    case 5:    base = 0x02800000; limit = 0x3ff8; scale = 4; break;
    case 6:    base = 0x03400000; limit = 0x1ff8; scale = 4; break;
    default: return FALSE;
    }
    u64 at = (u64)off * scale;
    if (type == 0x11 || type == 0x12) at &= ~3ULL;
    if ((type == 0x10 && (len % 20)) || ((type == 0x11 || type == 0x12) && (len % 12))) return FALSE;
    if (coeff) at = off;
    if ((at & 3) || (len & 3) || len == 0 || at > limit || len > limit - at) return FALSE;
    *reg = base + (u32)at;
    return TRUE;
}

static BOOLEAN cs_trims(u8 a)
{
    u32 id = 0, words[32];
    if (!cs_read(a, 0x10, &id)) return FALSE;
    const struct umi_cs_otp_entry *map;
    u32 count;
    if (id == 1 || id == 8) {
        map = umi_otp_map_1;
        count = (u32)(sizeof(umi_otp_map_1) / sizeof(umi_otp_map_1[0]));
    } else if (id == 2 || id == 6) {
        map = umi_otp_map_2;
        count = (u32)(sizeof(umi_otp_map_2) / sizeof(umi_otp_map_2[0]));
    } else {
        umi_log("cs35l41: amp 0x%x unsupported OTP map 0x%x\n", a, id);
        return FALSE;
    }
    for (u32 i = 0; i < 32; i++) {
        if (!cs_read(a, 0x400 + 4 * i, &words[i])) return FALSE;
    }
    if (!cs_write(a, 0x40, 0x55) || !cs_write(a, 0x40, 0xaa)) return FALSE;
    BOOLEAN ok = TRUE;
    u32 bit = 80;
    for (u32 i = 0; i < count; i++) {
        u32 sz = map[i].size, sh = map[i].shift;
        if (!sz || sz > 32 || sh + sz > 32 || bit + sz > 1024) { ok = FALSE; break; }
        u64 pair = words[bit / 32];
        if ((bit % 32) + sz > 32) pair |= (u64)words[bit / 32 + 1] << 32;
        u32 mask = (u32)((1ULL << sz) - 1);
        u32 val  = (u32)(pair >> (bit % 32)) & mask;
        bit += sz;
        if (map[i].reg && !cs_update(a, map[i].reg, mask << sh, val << sh)) { ok = FALSE; break; }
    }
    BOOLEAN lock = cs_write(a, 0x40, 0xcc) && cs_write(a, 0x40, 0x33);
    umi_log("cs35l41: amp 0x%x OTP map 0x%x (%u fields): %s\n", a, id, count, (ok && lock) ? "ok" : "FAIL");
    return ok && lock;
}

static BOOLEAN cs_board_setup(u8 a)
{
    u32 st = 0, w;
    for (w = 0; w < 100; w++) {
        if (!cs_read(a, 0x1001c, &st)) return FALSE;
        if (st & 2) break;
        umi_sleep_ms(1);
    }
    if (w == 100 || !cs_read(a, 0x10018, &st) || (st & 0x80000000u)) return FALSE;
    static const struct { u32 reg, val; } patch[] = {
        {0x40, 0x5555}, {0x40, 0xaaaa}, {0x4100, 0}, {0x4310, 0}, {0x4400, 0},
        {0x381c, 0x51}, {0x02bc20e0, 0}, {0x02bc2020, 0}, {0x4854, 0x01010000},
        {0x40, 0xcccc}, {0x40, 0x3333},
    };
    for (u32 i = 0; i < sizeof(patch) / sizeof(patch[0]); i++) {
        if (!cs_write(a, patch[i].reg, patch[i].val)) return FALSE;
    }
    if (!cs_trims(a)) return FALSE;
    return cs_update(a, 0x3810, 0xffff, 0x2424) &&
           cs_update(a, 0x3814, 0xff03, 0x7500) &&
           cs_update(a, 0x3808, 0x7f, 0x40) &&
           cs_update(a, 0x201c, 0x1000, 0x1000) &&
           cs_write(a, 0x6404, 0x0200530e);
}

static BOOLEAN cs_mailbox(u8 a, u32 cmd, u32 expected)
{
    u32 st = 0;
    if (!cs_write(a, 0x00010814, 1u << 20) ||
        !cs_write(a, 0x00010014, 1u << 21) ||
        !cs_update(a, 0x00010914, 1u << 20, 0) ||
        !cs_write(a, 0x00013020, cmd)) return FALSE;
    BOOLEAN ack = FALSE;
    for (u32 i = 0; i < 30; i++) {
        umi_sleep_ms(1);
        if (!cs_read(a, 0x00010014, &st)) break;
        if (st & (1u << 21)) { ack = TRUE; break; }
    }
    cs_write(a, 0x00010014, 1u << 21);
    cs_update(a, 0x00010914, 1u << 20, 1u << 20);
    if (!cs_read(a, 0x00013004, &st)) return FALSE;
    umi_log("cs35l41: amp 0x%x mailbox cmd=%u ack=%u state=%u (exp %u)\n", a, cmd, ack, st, expected);
    return ack && st == expected;
}

static BOOLEAN cs_preload(u8 addr)
{
    u32 fw_sz = 0, cf_sz = 0;
    u8 *fw = umi_load_fw("cs35l41-dsp1-spk-prot-revb2.wmfw", &fw_sz);
    const char *cf_name = (addr == 0x42) ? "cs35l41-dsp1-spk-prot-RCV-revb2.bin" : "cs35l41-dsp1-spk-prot-revb2.bin";
    u8 *cf = umi_load_fw(cf_name, &cf_sz);
    BOOLEAN ok = FALSE;
    if (!fw || fw_sz < 40 || fw_sz > 128 * 1024 || RtlCompareMemory(fw, "WMFW", 4) != 4 ||
        le32(fw + 4) != 40 || fw[10] != 4 || fw[11] != 3 ||
        !cf || cf_sz < 16 || cf_sz > 64 * 1024 || RtlCompareMemory(cf, "WMDR", 4) != 4 ||
        le32(cf + 4) != 16 || cf[11] != 1) {
        umi_log("cs35l41: amp 0x%x missing/invalid wmfw or bin\n", addr);
        goto out;
    }
    u32 val = 0;
    if (!cs_read(addr, 0x1001c, &val) || !(val & 2) ||
        !cs_read(addr, 0x10018, &val) || (val & 0x80000000u) ||
        !cs_read(addr, 0x2014, &val) || (val & 1) ||
        !cs_write(addr, 0x02bc1000, 0) || !cs_read(addr, 0x02bc1000, &val) || val != 0) {
        umi_log("cs35l41: amp 0x%x prereqs failed\n", addr);
        goto out;
    }
    for (u32 p = 40; p < fw_sz;) {
        if (fw_sz - p < 8) goto out;
        u32 hdr = le32(fw + p), len = le32(fw + p + 4), reg = 0;
        u32 type = hdr >> 24;
        if (len > fw_sz - p - 8) goto out;
        if (type != 0xf2 && type != 0xfe && type != 0xff) {
            if (!cs_region(type, hdr & 0xffffffu, len, FALSE, &reg)) goto out;
            if (!cs_memory(addr, reg, fw + p + 8, len)) goto out;
        }
        p += 8 + len;
    }
    u8 header[40];
    if (!cs_bytes(addr, 0x02800000, header, sizeof(header), FALSE)) goto out;
    u32 algs = be32(header + 36), fw_id = be32(header + 12);
    if ((be32(header + 4) >> 16) != 3 || algs > 16) goto out;
    struct { u32 id, xm, ym; } alg[17];
    alg[0].id = fw_id; alg[0].xm = be32(header + 20); alg[0].ym = be32(header + 28);
    for (u32 i = 0; i < algs; i++) {
        u8 e[24];
        if (!cs_bytes(addr, 0x02800028 + i * 24, e, sizeof(e), FALSE)) goto out;
        alg[i + 1].id = be32(e); alg[i + 1].xm = be32(e + 8); alg[i + 1].ym = be32(e + 16);
    }
    if (fw_id != 0x400a4 || be32(header + 16) != 0x300 || algs != 1 || alg[1].id != 0xcd) goto out;
    g_halo_xm[addr == 0x42] = 0x02800000 + 4 * alg[0].xm;
    g_cspl_xm[addr == 0x42] = 0x02800000 + 4 * alg[1].xm;

    for (u32 p = 16; p < cf_sz;) {
        if (cf_sz - p < 20) goto out;
        u32 off = cf[p] | ((u32)cf[p + 1] << 8);
        u32 type = cf[p + 2] | ((u32)cf[p + 3] << 8);
        u32 id = le32(cf + p + 4), len = le32(cf + p + 16), reg = 0;
        if (len > cf_sz - p - 20) goto out;
        if (type != 0xfe00 && type != 0xff00) {
            u32 a = 0;
            for (; a <= algs && alg[a].id != id; a++) {}
            if (a > algs) goto out;
            u32 base = (type == 5 || type == 0x11) ? alg[a].xm : alg[a].ym;
            u64 at = (u64)base * ((type == 5 || type == 6) ? 4 : 3);
            if (type == 0x11 || type == 0x12) at &= ~3ULL;
            at += off;
            if (at > 0xffffffffULL || !cs_region(type, (u32)at, len, TRUE, &reg)) goto out;
            if (!cs_memory(addr, reg, cf + p + 20, len)) goto out;
        }
        p = (p + 20 + len + 3) & ~3u;
    }
    umi_log("cs35l41: amp 0x%x firmware+tuning loaded ok\n", addr);
    ok = TRUE;
out:
    umi_free_fw(fw);
    umi_free_fw(cf);
    return ok;
}

static BOOLEAN cs_dsp_boot(u8 a)
{
    if (!cs_write(a, 0x00006000, 0x2004) ||
        !cs_update(a, 0x00002c0c, 0x1f, 3) ||
        !cs_update(a, 0x00004808, 0xff000755u, 0x10000200u) ||
        !cs_update(a, 0x00004840, 0x3f, 16) ||
        !cs_update(a, 0x00004820, 0x3f3f, a == 0x40 ? 1 : 0x100) ||
        !cs_write(a, 0x00004c40, 8) || !cs_write(a, 0x00004c44, 8) ||
        !cs_update(a, 0x00004800, 0x30000, 0x10000) ||
        !cs_update(a, 0x00002c04, 0x800, 0x800) ||
        !cs_update(a, 0x00002c04, 0x7e0, 0x1b << 5) ||
        !cs_update(a, 0x00002c04, 0x10, 0) ||
        !cs_update(a, 0x00002c04, 7, 0) ||
        !cs_update(a, 0x00002c04, 0x800, 0) ||
        !cs_update(a, 0x00002c04, 0x10010, 0x10010)) return FALSE;
    u32 xmb = 0, ymb = 0, xms = 0, yms = 0;
    if (!cs_read(a, 0x025e0038, &xmb) || !cs_read(a, 0x025e003c, &ymb) ||
        !cs_read(a, 0x025e0010, &xms) || !cs_read(a, 0x025e0018, &yms) ||
        !xmb || !ymb || (xms % xmb) || (yms % ymb)) return FALSE;
    const u32 dsp = 0x02b80000;
    for (u32 i = 0; i < 8; i++) {
        if (!cs_update(a, dsp + 0x80 + 8 * i, 0x1f, 1) ||
            !cs_update(a, dsp + 0x280 + 8 * i, 0x1f, 1)) return FALSE;
    }
    for (u32 i = 0; i < 6; i++)
        for (u32 j = 0; j < 3; j++)
            if (!cs_write(a, dsp + 0x45000 + 16 * i + 4 * j, 0)) return FALSE;
    for (u32 i = 0; i < 8; i++) {
        if (!cs_write(a, dsp + 0x45200 + 8 * i, 0xfc) ||
            !cs_write(a, dsp + 0x45400 + 8 * i, 0xfc) ||
            !cs_write(a, dsp + 0x45600 + 16 * i, 0xfc) ||
            !cs_write(a, dsp + 0x45604 + 16 * i, 0)) return FALSE;
    }
    if (!cs_write(a, dsp + 0x46020, 0xffffff) ||
        !cs_write(a, dsp + 0x46008, 0) ||
        !cs_write(a, dsp + 0x43140, 0x5555) ||
        !cs_write(a, dsp + 0x43140, 0xaaaa) ||
        !cs_write(a, dsp + 0x43000, (1u << (xms / xmb)) - 1) ||
        !cs_write(a, dsp + 0x43004, (1u << (yms / ymb)) - 1)) return FALSE;
    static const u32 restr[] = {
        0x43008, 0x4300c, 0x43014, 0x43018, 0x4301c, 0x43020, 0x43024, 0x4302c,
        0x43030, 0x43034, 0x43038, 0x4303c, 0x43044, 0x43048, 0x4304c, 0x43050, 0x43054, 0x4305c,
    };
    for (u32 i = 0; i < sizeof(restr) / sizeof(restr[0]); i++) {
        if (!cs_write(a, dsp + restr[i], 0)) return FALSE;
    }
    u32 xm = g_cspl_xm[a == 0x42];
    u32 cal_sz = 0;
    u8 *cal = umi_load_fw(a == 0x42 ? "cs35l41_cal_right.bin" : "cs35l41_cal.bin", &cal_sz);
    u32 res = (cal && cal_sz == 4) ? le32(cal) : 0;
    umi_free_fw(cal);
    g_unit_cal[a == 0x42] = (res >= 2000 && res <= 30000);
    if (!g_unit_cal[a == 0x42]) res = 8392;
    umi_log("cs35l41: amp 0x%x cal R=%u (%s)\n", a, res, g_unit_cal[a == 0x42] ? "unit" : "fallback");
    if (!xm || !cs_update(a, xm + 7 * 4, 0xffffff, res) ||
        !cs_update(a, xm + 9 * 4, 0xffffff, 1) ||
        !cs_update(a, xm + 10 * 4, 0xffffff, res + 1) ||
        !cs_write(a, 0x4c50, 0x28) || !cs_write(a, 0x4c54, 0x21) ||
        !cs_write(a, 0x4c58, 0x3a) || !cs_write(a, 0x4c5c, 0x3b)) return FALSE;
    if (!cs_write(a, dsp + 0x43140, 0) || !cs_write(a, 0x02bc1000, 1)) return FALSE;
    umi_sleep_ms(30);
    u32 pwr = 0, amp = 0;
    if (!cs_read(a, 0x2014, &pwr) || !cs_read(a, 0x2018, &amp) || (pwr & 1)) return FALSE;
    return cs_mailbox(a, 1, 1) && cs_mailbox(a, 2, 0);
}

void umi_speaker_quiesce(void)
{
    if (!g_tlmm_north) return;
    if (g_output_ready && !g_reset_asserted) {
        for (u32 i = 0; i < 2; i++) {
            u8 a = i ? 0x42 : 0x40;
            cs_write(a, 0x6000, 0x2004);
            cs_update(a, 0x2024, 0x10, 0x10);
            cs_update(a, 0x2014, 1, 0);
        }
        umi_wr32(g_tlmm_north, GPIO_IO(UMI_AMP_RESET), 0);
        umi_wr32(g_tlmm_north, GPIO_CFG(UMI_AMP_RESET), CFG_OUTPUT);
        g_reset_asserted = TRUE;
    }
    g_output_on = FALSE;
    g_output_ready = FALSE;
}

BOOLEAN umi_speaker_healthy(void)
{
    if (!g_output_ready || g_reset_asserted) return FALSE;
    for (u32 i = 0; i < 2; i++) {
        u8 a = i ? 0x42 : 0x40;
        u32 st = 0;
        if (!cs_read(a, 0x10010, &st)) return FALSE;
        /* Exact CS35L41 hardware fault mask (matches HackiOS): thermal/short/overcurrent/BST_SHORT */
        if (st & 0x800281c0u) {
            umi_log("cs35l41: amp 0x%x FAULT IRQ1_STATUS1=0x%x\n", a, st);
            umi_speaker_quiesce();
            return FALSE;
        }
        u32 dsp_err = 0;
        if (!cs_read(a, g_cspl_xm[i] + 12, &dsp_err) || dsp_err != 0) {
            umi_log("cs35l41: amp 0x%x CSPL error=0x%x\n", a, dsp_err);
            umi_speaker_quiesce();
            return FALSE;
        }
    }
    return TRUE;
}

BOOLEAN umi_speaker_prepare(void)
{
    if (!g_tlmm_north) {
        g_tlmm_north = (volatile u8 *)umi_map_io(UMI_TLMM_NORTH_PHYS, UMI_TLMM_SIZE, FALSE);
        if (!g_tlmm_north) return FALSE;
    }
    g_reset_cfg = umi_rd32(g_tlmm_north, GPIO_CFG(UMI_AMP_RESET));
    g_reset_io  = umi_rd32(g_tlmm_north, GPIO_IO(UMI_AMP_RESET));
    umi_wr32(g_tlmm_north, GPIO_IO(UMI_AMP_RESET), 0);
    umi_wr32(g_tlmm_north, GPIO_CFG(UMI_AMP_RESET), CFG_OUTPUT);
    umi_spin_us(2000);
    umi_wr32(g_tlmm_north, GPIO_IO(UMI_AMP_RESET), 2);
    umi_sleep_ms(10);
    for (u32 i = 0; i < 2; i++) {
        u8 a = i ? 0x42 : 0x40;
        u32 id = 0, rev = 0;
        if (!cs_read(a, 0, &id) || id != UMI_CS35L41_CHIP_ID ||
            !cs_read(a, 4, &rev) || rev != 0xb2 ||
            !cs_board_setup(a) || !cs_preload(a) || !cs_dsp_boot(a)) {
            umi_log("cs35l41: amp 0x%x prepare failed (id=0x%x rev=0x%x)\n", a, id, rev);
            umi_speaker_shutdown();
            return FALSE;
        }
    }
    g_output_on = FALSE;
    g_reset_asserted = FALSE;
    g_output_ready = TRUE;
    umi_log("cs35l41: both amps prepared and protected\n");
    return TRUE;
}

BOOLEAN umi_speaker_start(void)
{
    if (!g_output_ready || g_reset_asserted) return FALSE;
    if (g_output_on) return umi_speaker_healthy();
    for (u32 i = 0; i < 2; i++) {
        u8 a = i ? 0x42 : 0x40;
        if (!cs_update(a, 0x2024, 0x10, 0x10) ||
            !cs_write(a, 0x6000, 0x2004) ||
            !cs_write(a, 0x4c00, 0x32) ||
            !cs_update(a, 0x6c04, 0x3e0, 18u << 5) ||
            !cs_update(a, 0x201c, 0x100010, 0x100010) ||
            !cs_write(a, 0x10010, 0xffffffffu) ||
            !cs_write(a, 0x40, 0x55) || !cs_write(a, 0x40, 0xaa) ||
            !cs_write(a, 0x2084, 0x002f1aa0u) ||
            !cs_write(a, 0x40, 0xcc) || !cs_write(a, 0x40, 0x33) ||
            !cs_update(a, 0x2014, 1, 1)) goto fail;
        umi_sleep_ms(2);
        if (!cs_mailbox(a, 2, 0)) goto fail;
    }
    if (!umi_speaker_healthy()) goto fail;
    g_volume_atten = 320;
    for (u32 i = 0; i < 2; i++) {
        u8 a = i ? 0x42 : 0x40;
        if (!cs_update(a, 0x6000, 0x3fff, umi_cs35l41_volume_register(320)) ||
            !cs_update(a, 0x2024, 0x10, 0)) goto fail;
    }
    g_output_on = TRUE;
    umi_log("cs35l41: both amps started (-40 dB)\n");
    return TRUE;
fail:
    umi_speaker_quiesce();
    return FALSE;
}

BOOLEAN umi_speaker_volume(u32 below, u32 step_ms)
{
    u32 target = below > UMI_CS35L41_MAX_ATTENUATION ? UMI_CS35L41_MAX_ATTENUATION : below;
    if (target == 0) target = 1;
    if (!g_output_ready || !g_output_on || g_reset_asserted) return FALSE;
    for (;;) {
        u32 next = g_volume_atten;
        if (next > target) next = (next - target > 48) ? next - 48 : target;
        else if (next < target) next = (target - next > 48) ? next + 48 : target;
        else break;
        for (u32 i = 0; i < 2; i++) {
            u8 a = i ? 0x42 : 0x40;
            if (!cs_update(a, 0x6000, 0x3fff, umi_cs35l41_volume_register(next))) {
                umi_speaker_quiesce();
                return FALSE;
            }
        }
        g_volume_atten = next;
        if (step_ms) umi_sleep_ms(step_ms);
    }
    return umi_speaker_healthy();
}

void umi_speaker_stop(void)
{
    if (!g_output_on) return;
    umi_speaker_volume(UMI_CS35L41_MAX_ATTENUATION, 2);
    for (u32 i = 0; i < 2; i++) {
        u8 a = i ? 0x42 : 0x40;
        cs_update(a, 0x2024, 0x10, 0x10);
        cs_write(a, 0x6000, 0x2004);
        cs_update(a, 0x2014, 1, 0);
    }
    g_output_on = FALSE;
    umi_log("cs35l41: both amps stopped\n");
}

void umi_speaker_shutdown(void)
{
    umi_speaker_stop();
    if (g_tlmm_north) {
        umi_wr32(g_tlmm_north, GPIO_IO(UMI_AMP_RESET), 0);
        umi_spin_us(2000);
        umi_wr32(g_tlmm_north, GPIO_IO(UMI_AMP_RESET), g_reset_io & 2u);
        umi_wr32(g_tlmm_north, GPIO_CFG(UMI_AMP_RESET), g_reset_cfg);
        umi_unmap_io((void *)g_tlmm_north, UMI_TLMM_SIZE);
        g_tlmm_north = NULL;
    }
    g_output_ready = FALSE;
    g_reset_asserted = FALSE;
}
