# REMU - R100 NPU System Emulator
# Top-level Makefile

.PHONY: all build clean run-fw help

all: build

build:
	@./scripts/build-qemu.sh

clean:
	@./scripts/build-qemu.sh clean

run-fw:
	@./scripts/run-fw.sh

run-fw-gdb:
	@./scripts/run-fw.sh --gdb

help:
	@echo "REMU - R100 NPU System Emulator"
	@echo ""
	@echo "Targets:"
	@echo "  build       Build QEMU with R100 device models"
	@echo "  clean       Remove build artifacts"
	@echo "  run-fw      Boot firmware on emulated R100 SoC"
	@echo "  run-fw-gdb  Boot firmware and wait for GDB attach"
	@echo "  help        Show this message"
	@echo ""
	@echo "Setup:"
	@echo "  1. Add QEMU as submodule:  git submodule add https://gitlab.com/qemu-project/qemu.git external/qemu"
	@echo "  2. Build:                  make build"
	@echo "  3. Place FW images in:     images/"
	@echo "  4. Run:                    make run-fw"
