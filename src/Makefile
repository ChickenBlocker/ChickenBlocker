# Build + install the ChickenBlocker enforcement daemon (C port of main.py) and the
# cb-pause interactive client.
#
# The binary name `ChickenBlocker` becomes the kernel `comm`, giving the BPF LSM
# signal blocker a precise match (instead of the broad "python3").
#
# Build:  make
# Install (as root):  make install
#
# Requires a C compiler only (no external libs). The daemon reads
# ./policies.json and ./userChrome.css from its working directory
# (set via WorkingDirectory= in ChickenBlocker.service).

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -pedantic
PREFIX  ?= /opt/ChickenBlocker
DAEMON  := ChickenBlocker
CLIENT  := cb-pause
TEST    := /tmp/chickenblocker-test-meeting
DDEST   := $(PREFIX)/$(DAEMON)
CDEST   := $(PREFIX)/$(CLIENT)
CTMPL   := $(PREFIX)/.cb-pause.tmpl
HASHDIR := /var/lib/ChickenBlocker
HASH    := $(HASHDIR)/cb_pause_hash

.PHONY: all check install clean

all: $(DAEMON) $(CLIENT)

$(DAEMON): ChickenBlocker.c
	$(CC) $(CFLAGS) -o $@ ChickenBlocker.c

$(CLIENT): cb-pause.c
	$(CC) $(CFLAGS) -o $@ cb-pause.c

$(TEST): ../tests/test_chickenblocker_meeting.c ChickenBlocker.c
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable -o $@ $<

check: $(TEST)
	$(TEST)

install: $(DAEMON) $(CLIENT)
	install -d -m0755 $(PREFIX)
	# The daemon is a +i-protected artifact once the BPF lockdown is applied
	# (bpf/install.sh sets +i on it; update.sh refreshes it via the gated
	# ChickenBlockUpd). A plain `install` cannot overwrite a +i file (unlink is
	# denied by the kernel), so skip it when locked and let the gated updater
	# handle the refresh. On a fresh install (not +i yet) this installs normally.
	@if lsattr $(DDEST) 2>/dev/null | cut -c1-22 | grep -q 'i'; then \
		echo "SKIP: $(DDEST) is immutable (+i); refresh via 'make update' (gated updater)"; \
	else \
		install -m0755 $(DAEMON) $(DDEST); \
	fi
	# cb-pause: install a root-only template (the daemon recreates the runnable
	# copy from it and deletes it after each answered question) + the runnable
	# copy itself.
	install -m0644 $(CLIENT) $(CTMPL)
	install -m0755 $(CLIENT) $(CDEST)
	# Trust anchor: sha256 of the genuine template, root-only. The daemon
	# verifies the running cb-pause binary's hash against this before trusting
	# a pause answer.
	install -d -m0700 $(HASHDIR)
	sha256sum $(CTMPL) | awk '{print $$1}' > $(HASH)
	chmod 0600 $(HASH)
	@echo "installed $(DDEST)"
	@echo "installed $(CDEST) (+ template $(CTMPL), hash $(HASH))"
	@echo "update ChickenBlocker.service ExecStart to: $(DDEST)"

clean:
	rm -f $(DAEMON) $(CLIENT) $(TEST)
