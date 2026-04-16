# CLAUDE.md

## Project

REMU — R100 NPU System Emulator. QEMU-based functional emulator for the R100 (CR03 quad, 4-chiplet) NPU SoC. Runs unmodified firmware (q-sys, q-cp) and drivers (kmd, umd) on emulated hardware.

## Documentation

- [docs/architecture.md](docs/architecture.md) — System architecture, memory maps, device model design, FW source references
- [docs/roadmap.md](docs/roadmap.md) — Phased implementation plan (Phase 1: FW boot, Phase 2: host drivers, Phase 3: inference)

## Build

```
remu build              # or: cd cli && pip install -e . && remu build
remu run                # boot with FW images from images/
remu run --gdb          # wait for GDB on :1234
remu status             # show environment status
```

Manual build:
```
cd build/qemu && ninja -j$(nproc)
```

## Project layout

```
src/machine/          QEMU device models (symlinked into external/qemu/hw/arm/r100/)
src/include/          Shared headers (remu_addrmap.h)
cli/                  Click-based CLI tool
scripts/              Build and launch scripts
tests/                Test binaries and test scripts
docs/                 Architecture and roadmap docs
external/             Read-only: q-sys, q-cp, kmd, umd, qemu
images/               FW binaries (bl1.bin, bl31_cp0.bin, freertos_cp0.bin, ...)
build/qemu/           QEMU build output (gitignored)
```

## Code style

- Device models follow QEMU coding conventions: 4-space indent, snake_case, QOM type system
- Use `device_class_set_legacy_reset()` (not `dc->reset`) for QEMU 9.2.0+
- Use `qdev_prop_set_array()` with `QList` for array properties (e.g., GIC `redist-region-count`)
- Machine type names must end with `-machine` suffix (QEMU assertion requirement)
- All remu source uses `r100_` prefix; external repos keep their own naming (`rebel_h_*` in q-sys)

## Hardware context

- **Target**: CR03 quad (`PCI_ID 0x2030`, `ASIC_REBEL_QUAD`, 4 chiplets)
- **CPU**: 8x CA73 per chiplet (2 clusters x 4 cores) = 32 total
- **CHIPLET_OFFSET**: `0x2000000000` between chiplets
- **Boot**: TF-A BL1 → BL2 → BL31 → FreeRTOS (q-sys), then q-cp tasks
- **FW build flag**: `-p zebu` to skip PHY/QSPI hardware training
- **UART**: PL011 at `0x1FF9040000` (PERI0_UART0), 250MHz clock
- **GIC**: GICv3 distributor at `0x1FF3800000`, redistributor at `0x1FF3840000`
- **Timer**: 500MHz generic timer (`CORE_TIMER_FREQ`)

## Key external files

When modifying device models, cross-reference these FW/driver sources:

| What to check | File |
|----------------|------|
| SoC address map | `external/q-sys/.../autogen/g_sys_addrmap.h` |
| Platform defs | `external/q-sys/bootloader/cp/tf-a/plat/rebel/rebel_h/include/platform_def.h` |
| CMU polling patterns | `external/q-sys/bootloader/cp/tf-a/drivers/clk/clk_samsung/cmu.c` |
| PMU registers | `external/q-sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_pmu.c` |
| QSPI bridge protocol | `external/q-sys/bootloader/cp/tf-a/drivers/synopsys/qspi_bridge/qspi_bridge.c` |
| PCI BAR layout | `external/kmd/rebellions/rebel/rebel.h` |
| FW-host handshake | `external/kmd/rebellions/common/{fw_if.c,ring.c,queue.c}` |
