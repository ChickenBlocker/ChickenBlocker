# Build the ChickenBlocker BPF LSM program + userspace loader + gated updater.
#
# Prereqs (install once, as root):
#   apt install clang llvm libbpf-dev bpftool linux-headers-$(uname -r) build-essential
#
# Then:  make            (generates vmlinux.h, builds .bpf.o, the loader, and
#                          the ChickenBlockUpd +i setter/clearer)
#        make install    (root; copies artifacts to /opt/ChickenBlocker/bpf + unit)
#
# Requires CONFIG_BPF_LSM=y, CONFIG_DEBUG_INFO_BTF=y, and "bpf" in
# /sys/kernel/security/lsm. The install script checks these.

# Prefer a plain `clang`; fall back to a versioned clang-NN if only that is
# installed (the libbpf BPF workflow needs clang - gcc cannot emit BPF bytecode).
CLANG       ?= $(shell for c in clang clang-19 clang-18 clang-17 clang-16 clang-15 clang-14; do command -v $$c >/dev/null 2>&1 && { echo $$c; break; }; done)
LLVM_STRIP  ?= llvm-strip
BPFTOOL     ?= bpftool
CC          ?= cc
CFLAGS      ?= -O2 -g -Wall
ARCH := $(shell uname -m | sed 's/x86_64/x86/; s/aarch64/arm64/; s/ppc64le/powerpc/; s/mips64/mips/')
LIBBPF_CFLAGS := $(shell pkg-config --cflags libbpf 2>/dev/null)
LIBBPF_LIBS   := $(shell pkg-config --libs libbpf 2>/dev/null || echo -lbpf -lelf -lz)

# Multiarch include dir for vmlinux-independent system headers if needed.
SYS_INC := /usr/include/$(shell uname -m)-linux-gnu

BPF_O    := ChickenBlocker_lsm.bpf.o
LOADER   := ChickenBlocker_loader
# Gated +i setter/clearer. The binary is named "ChickenBlockUpd" (exactly 15
# chars), which fits the kernel task comm cap (TASK_COMM_LEN=16 incl. NUL =
# 15 usable chars), so the runtime comm == the filename with NO truncation.
# The block_setflags_immutable BPF program matches that comm (UPDATE_COMM in
# ChickenBlocker_lsm.bpf.c), so only this binary can clear +i.
UPDATER  := ChickenBlockUpd

.PHONY: all clean install
all: vmlinux.h $(BPF_O) $(LOADER) $(UPDATER)

vmlinux.h:
	@echo "Generating vmlinux.h from /sys/kernel/btf/vmlinux ..."
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

$(BPF_O): ChickenBlocker_lsm.bpf.c vmlinux.h
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) \
		-I$(SYS_INC) -I. -I/usr/include \
		-Wall -Wno-unused-value -Wno-pointer-sign \
		-c ChickenBlocker_lsm.bpf.c -o $@
	-$(LLVM_STRIP) -g $@

$(LOADER): ChickenBlocker_loader.c
	$(CC) $(CFLAGS) $(LIBBPF_CFLAGS) ChickenBlocker_loader.c -o $@ $(LIBBPF_LIBS)

# No libbpf dependency; plain C. The name is significant (see UPDATER note).
$(UPDATER): ChickenBlockUpd.c
	$(CC) $(CFLAGS) ChickenBlockUpd.c -o $@

install: all
	@echo "Run bpf/install.sh as root instead (it sets up the systemd unit)."
	@false

clean:
	rm -f $(BPF_O) $(LOADER) $(UPDATER) vmlinux.h
