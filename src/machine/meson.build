# REMU - R100 NPU System Emulator
# Meson build file for QEMU device models
#
# This file is included from the QEMU source tree's hw/arm/meson.build.
# To integrate:
#   1. Symlink or copy src/machine/ into qemu/hw/arm/r100/
#   2. Add subdir('r100') to qemu/hw/arm/meson.build

arm_ss.add(files(
    'r100_soc.c',
    'r100_cmu.c',
    'r100_dma.c',
    'r100_pmu.c',
    'r100_smmu.c',
    'r100_sysreg.c',
    'r100_hbm.c',
    'r100_qspi.c',
    'r100_rbc.c',
))
