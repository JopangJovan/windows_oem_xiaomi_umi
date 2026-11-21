# UMI side keys

Recovery confirms qpnp_pon on PM8150 SID0, peripheral 0x08, and gpio-keys.
The live DT identifies power KEY_POWER(116), volume-down KEY_VOLUMEDOWN(114),
and volume-up KEY_VOLUMEUP(115), PM8150 GPIO6 active low.

Owner-provided reference:
/home/jopvan/HackiOS-UMI/hakios-umi-upstream/devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_keys.c

PMIC arbiter core 0xc440000 (0x2000 bytes), owner config 0xc40a000 (0x1000),
EE0 observer 0xe600000 (0x10000). Enumerate APID mapping at core+0x900+4*n,
PPID=(mapping>>8)&0xfff, owner=(config+0x700+4*n)&7. Accept v5/v6,
channel count core+4 & 0x7ff, bounded to 512. Prefer owner0.
PON PPID0x008: status0810 bit0 power, bit1 volume-down.
PM8150 GPIO6 PPID0x0c5: statusc110 bit0 low means volume-up.
Verify GPIO subtypec105 in10..13, modec140 direction0 or2, enablec146 bit7.
Observer offset APID*0x80: write command (1<<27)|((addr&255)<<4), poll status+8
at most2ms, donebit0/errorbits1..3, byte at+0x18. No PMIC configuration writes.
Use software debounce and suppress initial held state until release.

Windows current DSDT SPMI QCOM250B UID1 claims0xc400000 length0x2800000.
This encompasses the observer and core resources. Resolve ownership before
creating an ACPI resource for the driver; do not overlap two active owners.
Recovery has devmem but no /dev/mem, so physical MMIO reads are unavailable
through that utility. Use existing kernel interfaces or a dedicated UEFI probe.

Planned HID collections: Consumer Control volume increment/decrement and
Generic Desktop System Control power-down. Windows power action depends on
power policy. Polling alone does not provide wake from sleep; wake needs its
own interrupt/power integration and must not be claimed from active-state input.

The known working touchscreen artifacts are preserved at
../Touch/known-working-0.1.0.2/; its runtime driver has not been modified for keys.

## Recovery input capture (2026-11-15)

Recovery enumerates `/dev/input/event4` (`gpio-keys`, KEY_VOLUMEUP) and
`/dev/input/event1` (`qpnp_pon`, KEY_POWER and KEY_VOLUMEDOWN). A live getevent
capture recorded repeated volume-up down/up transitions. The owner reports
pressing power and volume-down during captures, but neither produced an
EV_KEY event on any input device. One earlier PMIC poll showed PON RT status
0x04 and no IRQ changes. A later live poll, while the owner pressed power and
volume-down, observed PON status transitions 0x04->0x05 and 0x04->0x06 and
corresponding increases in both `pon_kpdpwr_status` and `pon_resin_status` IRQ
counters. GPIO6 status at 0xc510 stayed 0x01 during that poll; volume-up was
not reported as pressed during it. This confirms the PON signals reach the
PMIC and supports the existing mapping: PON status bit0 is power and bit1 is
volume-down. Linux getevent did not previously report those keys, so recovery
input reporting may still need separate investigation. The Windows driver
polls the same PON status bits; its HID output path still needs a direct event
test because the prior Raw Input monitor produced no usable log.
