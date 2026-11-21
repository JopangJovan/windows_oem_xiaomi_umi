/* Enumeration subset adapted from the owner-provided HackiOS UMI port. */
/* Xiaomi Mi 10 PCIe0 bootstrap. Register values and PHY data are from the
 * merged UMI stock DT and Kona register definitions. This diagnostic stops
 * with QCA6390 memory decoding enabled and bus mastering disabled.
 */
#include "umi_pcie.h"
#include "umi_pcie_status.h"
#include "umi_pcie_phy.h"

#define GCC  0x00100000u
#define GPIO 0x0f900000u /* Kona NORTH tile: TLMM base + 0x900000. */
#define PARF 0x01c00000u
#define PHY  0x01c06000u
#define SMMU 0x15000000u
#define DBI  0x60000000u
#define ATU  0x60001000u
#define CONF 0x60100000u
#define BAR  0x60300000u
#define WINDOW_END 0x63ffffffu
#define BIT(n) (1u << (n))
#define TRY(expr) do { error = (expr); if (error) goto fail; } while (0)

static uint32_t rd(struct umi_pcie *p, uint32_t addr)
{
	return p->io->read32(p->io->context, addr);
}
static void wr(struct umi_pcie *p, uint32_t addr, uint32_t value)
{
	p->io->write32(p->io->context, addr, value);
}
static void pause_us(struct umi_pcie *p, unsigned us)
{
	p->io->delay_us(p->io->context, us);
}
static void report(struct umi_pcie *p, const char *name, uint32_t value)
{
	if (p->io->report) p->io->report(p->io->context, name, value);
}
static int update(struct umi_pcie *p, uint32_t addr, uint32_t clear, uint32_t set)
{
	uint32_t value = rd(p, addr);
	if (value == UINT32_MAX) return EIO;
	wr(p, addr, (value & ~clear) | set);
	return 0;
}
static int wait_bits(struct umi_pcie *p, uint32_t addr, uint32_t mask,
    uint32_t wanted, unsigned timeout_us)
{
	uint32_t value = 0;
	for (unsigned us = 0; us < timeout_us; us += 10) {
		value = rd(p, addr);
		if (value == UINT32_MAX) return EIO;
		if ((value & mask) == wanted) return 0;
		pause_us(p, 10);
	}
	report(p, "poll timeout address", addr);
	report(p, "poll timeout value", value);
	return ETIMEDOUT;
}
static int gpio_output(struct umi_pcie *p, unsigned pin, bool high, uint32_t config)
{
	/* Set output latch before output-enable/mux to avoid a reset glitch. */
	int error = update(p, GPIO + pin * 0x1000 + 4, BIT(1), high ? BIT(1) : 0);
	if (error) return error;
	return update(p, GPIO + pin * 0x1000, 0x3ff, config | BIT(9));
}
static int clock_source(struct umi_pcie *p, uint32_t cmd, uint32_t config)
{
	int error = update(p, cmd + 4, 0x371f, config);
	if (error) return error;
	error = update(p, cmd, 0, 1);
	return error ? error : wait_bits(p, cmd, 1, 0, 10000);
}
static int branch(struct umi_pcie *p, uint32_t offset)
{
	int error = update(p, GCC + offset, 0, 1);
	return error ? error : wait_bits(p, GCC + offset, BIT(31), 0, 10000);
}
static int clocks(struct umi_pcie *p)
{
	int error;
	/* GDSC status is checked before accessing the PCIe controller/PHY. */
	TRY(update(p, GCC + 0x6b004, 7, 0));
	pause_us(p, 1);
	TRY(wait_bits(p, GCC + 0x6b004, BIT(31), BIT(31), 10000));
	TRY(clock_source(p, GCC + 0x6b038, 1)); /* AUX: 19.2 MHz XO. */
	/* Android uses GPLL0 divided by six for the 100 MHz PHY refgen. */
	TRY(update(p, GCC + 0x52018, 0, BIT(0)));
	TRY(clock_source(p, GCC + 0x6f014, (1u << 8) | 11u));
	report(p, "PHY refgen clock config", rd(p, GCC + 0x6f018));
	TRY(branch(p, 0x8c004)); /* QCA6390 reference clock/LDO. */
	TRY(branch(p, 0x6f02c));
	TRY(branch(p, 0x9000c));
	TRY(branch(p, 0x8d058));
	/* The AXI and PIPE clocks must not retain memory/peripheral state. */
	TRY(update(p, GCC + 0x6b01c, 0x6000, 0));
	/* Force diagnostic AHB/SLV clocks on. In HWCG mode the halt bit can
	 * assert merely because the bus is idle, so it is not a readiness test. */
	TRY(update(p, GCC + 0x6b014, 0x6002, 0));
	TRY(update(p, GCC + 0x6b024, BIT(1), 0));
	TRY(update(p, GCC + 0x6b02c, 0x6000, 0));
	TRY(update(p, GCC + 0x52008, 0, 0x2f)); /* All PCIe0 votes except PIPE. */
	static const uint32_t gates[] = {0x6b010, 0x6b014, 0x6b01c, 0x6b024, 0x6b028};
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++)
		TRY(wait_bits(p, GCC + gates[i], BIT(31), 0, 10000));
	static const uint32_t resets[] = {0x6b000, 0x6c01c};
	for (unsigned i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
		TRY(update(p, GCC + resets[i], 0, 1));
		pause_us(p, 1000);
		TRY(update(p, GCC + resets[i], 1, 0));
	}
	return 0;
fail:
	return error;
}
static int outbound(struct umi_pcie *p, unsigned index, uint32_t type,
    uint32_t start, uint32_t end, uint32_t target, uint32_t flags)
{
	uint32_t a = ATU + index * 0x200;
	wr(p, a + 4, 0);
	wr(p, a, type);
	wr(p, a + 8, start);
	wr(p, a + 12, 0);
	wr(p, a + 16, end);
	wr(p, a + 20, target);
	wr(p, a + 24, 0);
	wr(p, a + 4, BIT(31) | flags);
	return wait_bits(p, a + 4, BIT(31) | flags, BIT(31) | flags, 1000);
}

void umi_pcie_quiesce(struct umi_pcie *p)
{
	if (!p || !p->io) return;
	if (p->msi_cap) {
		uint32_t cap = rd(p, CONF + p->msi_cap);
		if (cap != UINT32_MAX) wr(p, CONF + p->msi_cap, cap & ~BIT(16));
		p->msi_cap = 0;
	}
	/* Stop protected firmware DMA before asserting reset. Enumeration alone
	 * never enables bus mastering. Shared power/clock votes remain held. */
	if (p->dma_enabled) {
		uint32_t command = rd(p, CONF + 4);
		if (command != UINT32_MAX) wr(p, CONF + 4, (command & 0xffff) & ~4u);
		p->dma_enabled = false;
	}
	if (p->enumerated) wr(p, DBI + 4, 2);
	(void)gpio_output(p, 79, false, 1);
	pause_us(p, 1000); /* PERST propagation before the SMMU can be restored. */
	(void)gpio_output(p, 20, false, 3 | (7u << 6));
	p->enumerated = false;
}

int umi_pcie_enumerate(struct umi_pcie *p, const struct umi_pcie_io *io)
{
	if (!p || !io || !io->read32 || !io->write32 || !io->power_on || !io->delay_us)
		return EINVAL;
	if (p->io) return EBUSY;
	p->io = io;
	int error;
	TRY(gpio_output(p, 79, false, 1));
	pause_us(p, 1000);
	TRY(io->power_on(io->context));
	/* UMI stock pinctrl: CLKREQ mux1/pull-up; WAKE input/pull-up. */
	TRY(update(p, GPIO + 80 * 0x1000, 0x3ff, 7));
	TRY(update(p, GPIO + 81 * 0x1000, 0x3ff, 3));
	TRY(gpio_output(p, 20, true, 3 | (7u << 6)));
	pause_us(p, 1000);
	TRY(clocks(p));
	report(p, "power and clocks ready", rd(p, GCC + 0x6b004));
	wr(p, PARF + 0x1000, 4); /* Root complex. */
	TRY(update(p, PARF + 0x40, 1, 0));
	wr(p, PARF + 0x350, 0);
	wr(p, PARF, 0x365e);
	TRY(update(p, PARF + 0x174, 0, BIT(4)));
	wr(p, PARF + 0x22c, 0); /* No unhandled PCIe interrupts during enumeration. */
	wr(p, PARF + 0x358, 0x04000000);
	wr(p, PARF + 0x3d4, 0x0000000au); /* PARF_NO_SNOOP_OVERRIDE: force snoop on RD/WR for dma-coherent */
	report(p, "PCIe No Snoop override", rd(p, PARF + 0x3d4));
	TRY(update(p, PARF + 0x1a8, 0, BIT(31)));
	for (unsigned i = 0; i < sizeof(umi_pcie_phy) / sizeof(umi_pcie_phy[0]); i++) {
		wr(p, PHY + umi_pcie_phy[i].offset, umi_pcie_phy[i].value);
		if (umi_pcie_phy[i].delay_us) pause_us(p, umi_pcie_phy[i].delay_us);
	}
	pause_us(p, 1000);
	TRY(update(p, GCC + 0x52008, 0, BIT(4))); /* PIPE: vendor skips halt polling. */
	TRY(wait_bits(p, PHY + 0x814, BIT(6), 0, 100000));
	report(p, "PHY ready", rd(p, PHY + 0x814));
	pause_us(p, 10000); /* UMI DT ep-latency=10 ms. */
	TRY(gpio_output(p, 79, true, 1));
	pause_us(p, 1000);
	TRY(update(p, DBI + 0x80c, 0x1f00, 0x100));
	wr(p, DBI + 0x8a8, 0x20);
	TRY(update(p, DBI + 0x890, BIT(0), 0));
	TRY(update(p, DBI + 0x8bc, 0, BIT(0)));
	/* The vendor default is FF0000. Windows needs a PCI-to-PCI bridge. */
	TRY(update(p, DBI + 0x08, 0xffffff00u, 0x06040000u));
	report(p, "root port class and revision", rd(p, DBI + 0x08));
	wr(p, DBI + 0x154, 0x55555555);
	TRY(update(p, DBI + 0x8bc, BIT(0), 0));
	TRY(update(p, DBI + 0xa0, 0xf, 2)); /* Gen2 x1: stock UMI link rate. */
	/* Write only the control half of the PCIe capability (upper half is W1C). */
	uint32_t control = rd(p, DBI + 0x78);
	if (control == UINT32_MAX) { error = EIO; goto fail; }
	wr(p, DBI + 0x78, (control & 0xffff) & ~0x7000u);
	TRY(update(p, PARF + 0x1b0, 0, BIT(8)));
	TRY(wait_bits(p, DBI + 0xf28, BIT(10), BIT(10), 100000));
	TRY(wait_bits(p, DBI + 0x80, BIT(29), BIT(29), 100000));
	report(p, "link status", rd(p, DBI + 0x80));
	wr(p, DBI + 0x18, 0x00010100); /* Primary 0, secondary/subordinate 1. */
	/* Seed the root port BAR separately from the downstream window,
	 * matching the live Android allocation for the bridge. */
	uint32_t root_bar = rd(p, DBI + 0x10);
	if (root_bar == UINT32_MAX || (root_bar & 7) != 4) {
		error = EIO;
		goto fail;
	}
	wr(p, DBI + 0x10, 0x60700000u | (root_bar & 15));
	wr(p, DBI + 0x14, 0);
	wr(p, DBI + 0x1c, 0x000000f0); /* Unused I/O window: base above limit. */
	wr(p, DBI + 0x24, 0x0000fff0); /* Disable unused prefetch window. */
	wr(p, DBI + 0x28, 0);
	wr(p, DBI + 0x2c, 0);
	report(p, "root port BAR0", rd(p, DBI + 0x10));
	wr(p, DBI + 0x20, 0x60406030); /* Leave the root port BAR outside the bridge window. */
	wr(p, DBI + 0xb40, 0x14); /* AUX 19.2 MHz. */
	/* Bus 1 occupies a full MiB in MCFG. CFG shift extracts BDF from
	 * each ECAM address instead of aliasing every request to 01:00.0. */
	TRY(outbound(p, 0, 4, CONF, CONF + 0xfffff, 0, BIT(28)));
	report(p, "ECAM CFG0 region control", rd(p, ATU + 4));
	uint32_t id = 0;
	for (unsigned i = 0; i < 1000; i++) {
		id = rd(p, CONF);
		if (id == 0x110117cb) break;
		pause_us(p, 1000);
	}
	report(p, "endpoint PCI ID", id);
	if (id != 0x110117cb) { error = ENODEV; goto fail; }
	uint32_t command = rd(p, CONF + 4), bar = rd(p, CONF + 0x10);
	if (command == UINT32_MAX || bar == UINT32_MAX || (bar & 1) ||
	    ((bar >> 1) & 3) == 1 || ((bar >> 1) & 3) == 3) {
		error = EIO; goto fail;
	}
	/* QCA6390's known 1 MiB BAR; never enable DMA before installing its SMMU. */
	wr(p, CONF + 4, ((command & 0xffff) & ~7u) | BIT(10));
	if (rd(p, CONF + 4) & 4) { error = EIO; goto fail; }
	/* The SID table can contain uninitialized values after reset.
	 * Initialize this owned, single-endpoint controller like the stock driver,
	 * after checking QCA6390 identity and disabling endpoint bus mastering. */
	for (unsigned i = 0; i < 256; i++) {
		wr(p, PARF + 0x2000 + 4 * i, 0);
		if (rd(p, PARF + 0x2000 + 4 * i) != 0) { error = EIO; goto fail; }
	}
	report(p, "BDF-to-SID table initialized, DMA disabled", 256);
	wr(p, CONF + 0x10, BAR | (bar & 15));
	if ((bar & 6) == 4) wr(p, CONF + 0x14, 0);
	TRY(outbound(p, 1, 0, BAR, WINDOW_END, BAR, 0));
	wr(p, DBI + 4, 2); /* Root bridge memory decode only. */
	wr(p, CONF + 4, ((command & 0xffff) & ~7u) | 2 | BIT(10));
	report(p, "endpoint command", rd(p, CONF + 4));
	report(p, "endpoint BAR0", rd(p, CONF + 0x10));
	if ((rd(p, CONF + 4) & 7) != 2 || (rd(p, CONF + 0x10) & ~15u) != BAR) {
		error = EIO; goto fail;
	}
	uint32_t version = rd(p, BAR + 8);
	report(p, "MHI version (DMA disabled)", version);
	if (version != 0x01000000) {
		report(p, "MHI register length", rd(p, BAR));
		report(p, "MHI control", rd(p, BAR + 0x38));
		report(p, "MHICFG", rd(p, BAR + 0x10));
		report(p, "channel doorbells", rd(p, BAR + 0x18));
		report(p, "event doorbells", rd(p, BAR + 0x20));
		report(p, "BHI offset", rd(p, BAR + 0x28));
		report(p, "BHIE offset", rd(p, BAR + 0x2c));
		if (rd(p, BAR + 0x28) == 0x100) {
			report(p, "BHI major version", rd(p, BAR + 0x104));
			report(p, "BHI execution environment", rd(p, BAR + 0x128));
		}
	}
	/* Captured on real UMI PBL: MHIVER has a placeholder, BHI is readable. */
	bool cold_pbl = version == 0xdeadbeef && rd(p, BAR) == 0x100 &&
	    rd(p, BAR + 0x18) == 0x300 && rd(p, BAR + 0x20) == 0x700 &&
	    rd(p, BAR + 0x28) == 0x100 && rd(p, BAR + 0x2c) == 0x224 &&
	    rd(p, BAR + 0x104) == 1 && rd(p, BAR + 0x128) == 0;
	if (version != 0x01000000 && !cold_pbl) { error = ENODEV; goto fail; }
	if (cold_pbl) report(p, "QCA6390 cold PBL signature validated", 1);
	p->enumerated = true;
	return 0;
fail:
	p->failed = true;
	report(p, "enumeration failed errno", (uint32_t)error);
	umi_pcie_quiesce(p);
	return error;
}


/* Program the UMI DT route (RID 0x0100 -> SID 0x1c01) with DMA off. */
int umi_pcie_prepare_stream(struct umi_pcie *p)
{
 if (!p || !p->enumerated || p->failed) return ENODEV;
 uint32_t command = rd(p, CONF + 4);
 if (command == UINT32_MAX || (command & 7) != 2) return EIO;
 uint8_t hash = 0;
 const uint8_t requester[] = {1, 0};
 for (unsigned i = 0; i < sizeof(requester); ++i) {
  hash ^= requester[i];
  for (unsigned bit = 0; bit < 8; ++bit)
   hash = (hash << 1) ^ ((hash & 0x80) ? 7 : 0);
 }
 uint32_t route = PARF + 0x2000 + 4 * hash;
 uint32_t old = rd(p, route);
 if (old != 0 && old != 0x01000100) return EBUSY;
 wr(p, route, 0x01000100);
 if (rd(p, route) != 0x01000100) return EIO;
 int error = update(p, PARF + 0x2c00, 1, 0);
 if (error) return error;
 /* Let Windows enable endpoint DMA after it configures the SMMU.
  * The endpoint command stays at memory decoding only until then. */
 wr(p, DBI + 4, BIT(1) | BIT(2));
 uint32_t bridge_command = rd(p, DBI + 4);
 report(p, "root port command at handoff", bridge_command);
 if (bridge_command == UINT32_MAX || (bridge_command & 7) != 6) return EIO;
 report(p, "PCIe requester stream", 0x1c01);
 /* Inspect APPS_SMMU and install a Stage-1-disabled (identity bypass) context for SID 0x1c01. */
 uint32_t scr0 = rd(p, SMMU + 0x000);
 uint32_t idr0 = rd(p, SMMU + 0x020);
 uint32_t idr1 = rd(p, SMMU + 0x024);
 uint32_t matches = idr0 & 0xffu;
 uint32_t num_cb = idr1 & 0xffu;
 uint32_t page_sz = (idr1 >> 31) ? 0x10000u : 0x1000u;
 uint32_t num_pages = 2u << ((idr1 >> 28) & 7u);
 report(p, "SMMU SCR0", scr0);
 report(p, "SMMU NUMSMRG", matches);
 report(p, "SMMU NUMCB", num_cb);
 report(p, "SMMU SGFSR", rd(p, SMMU + 0x048));
 bool used_cb[128] = { false };
 uint32_t free_idx = UINT32_MAX;
 bool covered = false;
 for (uint32_t n = 0; n < matches && n < 128u; ++n) {
  uint32_t smr = rd(p, SMMU + 0x800 + 4 * n);
  uint32_t s2cr = rd(p, SMMU + 0xc00 + 4 * n);
  if (smr & BIT(31)) {
   if ((s2cr & 0xffu) < 128u) used_cb[s2cr & 0xffu] = true;
   uint32_t id = smr & 0x7fffu;
   uint32_t mask = (smr >> 16) & 0x7fffu;
   if (((id ^ 0x1c01u) & ~mask) == 0) {
    covered = true;
    report(p, "SMMU existing SMR idx", n);
    report(p, "SMMU existing SMR val", smr);
    report(p, "SMMU existing S2CR val", s2cr);
   }
  } else if (free_idx == UINT32_MAX) {
   free_idx = n;
  }
 }
 uint32_t free_cb = UINT32_MAX;
 for (uint32_t cb = 0; cb < num_cb && cb < 128u; ++cb) {
  uint32_t cb_off = (num_pages + cb) * page_sz;
  if (cb_off + 0x1000u > 0x000d0000u) break;
  if (!used_cb[cb] && (rd(p, SMMU + cb_off) & 1u) == 0) {
   free_cb = cb;
   break;
  }
 }
 if (!covered && free_idx != UINT32_MAX && free_cb != UINT32_MAX) {
  uint32_t cb_off = (num_pages + free_cb) * page_sz;
  wr(p, SMMU + cb_off, 0u);                                /* CB_SCTLR: M=0 (identity bypass) */
  wr(p, SMMU + page_sz + 0x800u + 4u * free_cb, 1u);       /* CBA2R: VA64 */
  wr(p, SMMU + page_sz + 4u * free_cb, 0x0f210000u);       /* CBAR: S1+S2 bypass, ISH, Normal WB-WB */
  wr(p, SMMU + 0xc00u + 4u * free_idx, free_cb);           /* S2CR: TYPE=00 (Translate), CBNDX=free_cb */
  wr(p, SMMU + 0x800u + 4u * free_idx, 0x80001c01u);       /* SMR: VALID=1, MASK=0, ID=0x1c01 */
  report(p, "SMMU bypass SMR idx", free_idx);
  report(p, "SMMU bypass CB idx", free_cb);
  report(p, "SMMU bypass SMR readback", rd(p, SMMU + 0x800u + 4u * free_idx));
  report(p, "SMMU bypass S2CR readback", rd(p, SMMU + 0xc00u + 4u * free_idx));
  report(p, "SMMU bypass CBAR readback", rd(p, SMMU + page_sz + 4u * free_cb));
  report(p, "SMMU bypass SCTLR readback", rd(p, SMMU + cb_off));
 }
 return 0;
}
