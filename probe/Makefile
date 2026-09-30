# m8c-ps5 phase-0 USB probe — headless ELF, results to /data/m8c_ps5_probe.log
PS5_PAYLOAD_SDK := $(HOME)/ps5-jailbreak/refs/releases/sdk/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

# llvm-config shim: Manjaro keeps LLVM tools in /usr/bin (no llvm21/ bindir);
# without it the wrappers chase theos clang-11 or a build-machine path.
export LLVM_CONFIG ?= $(HOME)/ps5-jailbreak/refs/releases/sdk/llvm-config-shim.sh

PS5_HOST ?= 192.168.0.106
PS5_PORT ?= 9021

ELF := m8c_ps5_probe.elf

CFLAGS := -Wall -g -O2

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^

# sends the ELF to elfldr (9021) and launches it; log lands in /data over FTP
run: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)
	@echo "launched; read results: curl ftp://$(PS5_HOST):2121/data/m8c_ps5_probe.log"

clean:
	rm -f $(ELF)
