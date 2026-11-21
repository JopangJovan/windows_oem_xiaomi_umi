# SM8250 / QCA6390 Windows 11 ARM64 Wi-Fi & Internet Fix

## 1. Fix QCA6390 not starting (Code 56 / freeze on driver start)
On Qualcomm SM8250, `APPS_SMMU` (`0x15000000`) has `SCR0.USFCFG = 1` (unmatched Stream IDs fault), and QHEE (EL2) **blocks direct `S2CR.TYPE = 01` (Bypass)** writes (turning them into `0x00020000` Fault).

In UEFI (`umi_pcie.c`), install a **Stage-1-disabled Context Bank identity bypass** for QCA6390's Stream ID (`0x1c01`), plus `PARF_NO_SNOOP_OVERRIDE`:

```c
/* 1. In PCIe0 PARF init: */
wr(p, PARF + 0x3d4, 0x0000000au); /* PARF_NO_SNOOP_OVERRIDE */

/* 2. After programming PARF BDF-to-SID (RID 0x0100 -> SID 0x1c01):
 * Find first free SMR (free_idx) and first free Context Bank (free_cb) in APPS_SMMU (0x15000000),
 * then program a Stage-1-disabled context bank (accepted by QHEE):
 */
uint32_t cb_off = (num_pages + free_cb) * page_sz;
wr(p, SMMU + cb_off, 0u);                          /* CB_SCTLR: M=0 (identity bypass) */
wr(p, SMMU + page_sz + 0x800u + 4u * free_cb, 1u); /* CBA2R: VA64 */
wr(p, SMMU + page_sz + 4u * free_cb, 0x00210000u); /* CBAR: Stage 1 + Stage 2 bypass, ISH */
wr(p, SMMU + 0xc00u + 4u * free_idx, free_cb);     /* S2CR: TYPE=00 (Translate), CBNDX=free_cb */
wr(p, SMMU + 0x800u + 4u * free_idx, 0x80001c01u); /* SMR: VALID=1, MASK=0, ID=0x1c01 */
```

## 2. Fix "Connected, no internet" (DHCP timeout / TX packets dropped)
With a Stage-1-disabled SMMU context bank, `CBAR` does not force Normal Write-Back Cacheable memory attributes, so QCA6390 DMA does not snoop dirty CPU L1/L2 cache lines. If ACPI advertises `PCI0` as cache-coherent (`_CCA = 1`), Windows skips CPU D-cache flushes for TX `NET_BUFFER_LIST` packets—so Wi-Fi associates and receives RX packets, but TX packets (DHCP/ARP/ICMP) never reach the router.

### ACPI changes (see `dsdt-noncoherent.patch` and `iort-noncoherent.patch`)
1. **DSDT (`Device (PCI0)`)**: Change `_CCA` from `Return (One)` to `Return (Zero)`:
   ```asl
   Method (_CCA, 0, NotSerialized)
   {
       Return (Zero)
   }
   ```
2. **IORT (PCI Root Complex, Segment 0)**: Set `Cache Coherency` to `0x00000000` and `Memory Flags` (`Coherency`) to `0x00`.

After rebuilding the UEFI image with these ACPI changes and booting Windows 11, Windows explicitly cleans/invalidates CPU caches for TX/RX DMA and both DHCP and full internet work immediately.
