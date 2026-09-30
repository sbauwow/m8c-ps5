# m8c PS5 port — native elfldr payload (ps5-payload-sdk + pacbrew SDL2).
# Upstream m8c v1.7.10 sources staged in app/src (the PS4 port's copies);
# PS5 backends in port/. usb.c + usb_audio.c are REPLACED by port/usb_ps5.c
# + port/usbio_ps5.c; audio.c + serial.c self-exclude via -DUSE_LIBUSB.
#
# Toolchain notes (Manjaro):
#  - LLVM_CONFIG must point at the bundled shim BEFORE anything calls the
#    compiler wrappers: the wrappers resolve clang/ld.lld via llvm-config
#    --bindir, and theos clang-11 otherwise wins. The shim routes to
#    /usr/bin LLVM 21 (the console was built with 21; 22 emits them wrong).
#  - prospero-clang injects --sysroot and the -L paths itself; only SDL2's
#    HEADER dir needs an explicit -I (the -L side is automatic).

PS5_PAYLOAD_SDK := $(HOME)/ps5-jailbreak/refs/releases/sdk/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

export LLVM_CONFIG := $(HOME)/ps5-jailbreak/refs/releases/sdk/llvm-config-shim.sh

HB := $(HOME)/m8c-ps5/downloads/opt/ps5-payload-sdk/target/user/homebrew

PS5_HOST ?= 192.168.0.106
PS5_PORT ?= 9021

SRC_DIR   := app/src
PORT_DIR  := port
OBJDIR    := build
ELF       := m8c_ps5.elf

# Upstream sources minus the libusb backends this port replaces.
CFILES := $(filter-out usb.c usb_audio.c, $(notdir $(wildcard $(SRC_DIR)/*.c)))
CFILES += usb_ps5.c ps5_shims.c usbio_ps5.c ps5_usbd.c audio_native_ps5.c

OBJS := $(addprefix $(OBJDIR)/,$(CFILES:.c=.o))

CFLAGS := -Wall -g -O2 -std=gnu17 \
          -I$(HB)/include/SDL2 \
          -I$(SRC_DIR) -I$(PORT_DIR) \
          -DUSE_LIBUSB -DPS5

LDLIBS := -L$(HB)/lib -lSDL2 -lSceAudioOut -lSceUserService \
          -lSceVideoOut -lSceKeyboard -lSceImeDialog -lSceSystemService \
          -lScePad -liconv

_unused := $(shell mkdir -p $(OBJDIR))

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: $(PORT_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# prospero-deploy needs socat (not installed here); send_payload.py does the
# same job. Launch happens on the console; logs land in /data over FTP (2121).
run: $(ELF)
	python3 $(HOME)/ps5-jailbreak/scripts/send_payload.py $(PS5_HOST) $(ELF) $(PS5_PORT)
	@echo "launched; read results: curl ftp://$(PS5_HOST):2121/data/m8c.log"

clean:
	rm -rf $(OBJDIR) $(ELF)

.PHONY: all run clean
