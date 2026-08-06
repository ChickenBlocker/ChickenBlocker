# ChickenBlocker

ChickenBlocker is a self-enforcing daemon that keeps a Firefox self-blocking setup
intact. It re-enables the leechblock-ng extension if it has been disabled, keeps
`/etc/firefox/policies/policies.json` and the profile's `userChrome.css` in sync
with its own copies, and restarts Firefox when anything changed. It only
relaunches Firefox if it was already running. The daemon is a single persistent
C binary (named `ChickenBlocker` so its kernel `comm` is unique). It checks
enforcement state about every 30 seconds; systemd restarts it only after a
failure or external termination.

Optional hardening layers make it hard to disable:
- AppArmor + immutable file lockdown (`make install-armor`) - blocks
  `systemctl disable`, edits and removal of the unit file.
- BPF LSM signal blocker (`make install-bpf-boot`) - denies SIGHUP/SIGINT/
  SIGQUIT/SIGKILL/SIGUSR1/SIGUSR2/SIGSTOP to the daemon even for root. SIGTERM
  remains available for systemd shutdown; external SIGTERM is force-restarted.

## SafeEyes layer (pomodoro + night block)

The daemon also enforces [SafeEyes](https://slgobinath.github.io/SafeEyes/)
(`sudo apt install safeeyes`) as a second blocking surface:

- **Auto-start + keep-alive**: if SafeEyes isn't running, the daemon starts it
  as the logged-in user; if the user kills it, it respawns on the next check.
- **DAY (08:00-23:59)**: a strict pomodoro schedule - 30 min work, 5 min break,
  30 min long break after 4 cycles. Breaks are un-skippable/un-postponable and
  the tray disable is off. Camera activity alone does not disable Safe Eyes.
  Only an eligible meeting from the dedicated calendar can open a pause.
  Work countdowns pause during sleep and after 2 minutes away; break countdowns
  use wall time, so sleeping or locking the screen does not pause a break.
- **NIGHT (00:00-08:00)**: a single strict 8h break blanks the screen for the
  whole night. Calendar events and camera activity are ignored at night.

Source configs are `safeeyes.json` (day) and `safeeyes-night.json` (night),
seeded into `~/.config/safeeyes/safeeyes.json` and applied via a quit+relaunch
at each 00:00 / 08:00 boundary. Camera detection uses `cb_av_check.py`
(`/proc/*/fd` -> `/dev/video*`).

Note: SafeEyes itself is not SIGKILL-immune in v1 (only `ChickenBlocker` is); it
is enforced by respawn, not by the BPF signal blocker.

Calendar-gated camera pauses require the distro `vdirsyncer`, `khal`, and OAuth
support packages. `vdirsyncer` performs the read-only local synchronization and
`khal` expands recurring events and normalizes their dates and timezones:

```bash
sudo apt install vdirsyncer khal python3-aiohttp-oauthlib
```

## Calendar setup

Calendar synchronization uses OAuth only. The Cloud project that owns the OAuth
client may belong to a different Google account from the corporate account that
authorizes calendar access.

### Create the OAuth client

1. Open [Google Cloud Console](https://console.cloud.google.com/), then create or
   select a project.
2. Open **Google Auth Platform** -> **Branding**. If it is not configured, click
   **Get started**.
3. Enter an app name such as `ChickenBlocker Calendar`, select a user-support
   email, and enter a developer-contact email.
4. Choose **External** when the project is owned by a personal account. Choose
   **Internal** only when the project belongs to the corporate Google Cloud
   organization.
5. For an External app, open **Audience** -> **Test users** -> **Add users**, then
   add the corporate Google account that will provide the calendar.
6. For an External app, open **Data Access** -> **Add or remove scopes** and add
   `https://www.googleapis.com/auth/calendar`, which is the scope requested by
   vdirsyncer's Google backend.
7. Open **APIs & Services** -> **Library**, search for **Google CalDAV API**, and
   enable it for the project.
8. Open **Google Auth Platform** -> **Clients** -> **Create client**. Select
   **Desktop app**, give it a name, and create it.
9. Keep the displayed client ID and client secret available for the setup script.

Google documents that External apps in **Testing** expire user authorization and
refresh tokens after seven days. Publishing the app to **In production** removes
that Testing-mode expiry, but a one-user unverified app can still display an
unverified-app warning and remains subject to Workspace administrator policy.
Google lists limited personal use as an exception from submitting the app for
verification. See Google's
[OAuth consent guide](https://developers.google.com/workspace/guides/configure-oauth-consent)
and [Audience documentation](https://support.google.com/cloud/answer/15549945).

### Authorize ChickenBlocker

Run the setup as the desktop user without `sudo`:

```bash
./configure-calendar.sh --oauth
```

The script prompts for the account email, Desktop OAuth client ID, and client
secret, then opens Google authorization in the browser. It synchronizes only the
specified account's primary calendar. Vdirsyncer's Google backend requests the
Google Calendar OAuth scope, but ChickenBlocker configures that storage as
read-only and never uploads changes. A Workspace administrator may need to allow
the OAuth client ID. Configuration and the refresh token are stored with mode
`0600` under `~/.config/chickenblocker`; no Google password is stored.
From 08:00 onward, ChickenBlocker invokes `vdirsyncer` and `khal` once in each
local wall-clock hour while the PC is awake. If the PC sleeps through a complete
hour, that hour is skipped rather than synchronized later; the current hour is
synchronized at the first available daemon pass after wake. Breaks and
calendar-authorized pauses do not shift this wall-clock schedule.

A calendar check on date `D` may make exact occurrences on `D+1` or later
eligible. It cannot make a new or rescheduled occurrence on `D` eligible. At an
eligible occurrence's start, Safe Eyes pauses for at most 120 seconds while
waiting for the camera. Camera confirmation keeps the pause active until the
camera has been absent for 120 seconds or the event ends. Microphone-only use
does not confirm a meeting. Suspending during confirmation consumes that attempt
and restores the ordinary work countdown after wake.

## Install

Run from the project directory. `make install` builds and installs the daemon,
assets, policies and the systemd unit, then offers to install the two hardening
layers interactively.

1. Build and install the daemon:
   ```
   make install
   ```
   When prompted, answer `y` to optionally install AppArmor lockdown and/or
   stage the BPF firstboot.

2. (For BPF) enable the BPF LSM in GRUB and stage the auto-loading firstboot:
   ```
   make grub
   make install-bpf-boot
   ```
   `make grub` adds `bpf` to the kernel `lsm=` list (auto-reverts on failure,
   no manual revert path). It requires a reboot to activate.

3. Reboot. On the next boot, the BPF firstboot attaches + pins the signal
   blocker, enables the persistent `ChickenBlocker-bpf.service`, then deletes
   itself - no installer script left in any system path.

4. (Optional, one-way) lock the unit file with AppArmor + immutable. Do this
   last, because afterward `make install` can no longer overwrite
   `ChickenBlocker.service`:
   ```
   make install-armor
   ```

## Upgrade from the previous name

Remove the legacy daemon and its stale BPF process matcher before installing
ChickenBlocker. The migration script preserves policies, SafeEyes data,
AppArmor profiles, and user configuration:

```
sudo ./uninstall-previous.sh
make install-core
sudo ./update.sh
sudo reboot
```

## BPF toolchain

The BPF layer needs the build toolchain. `make install` (or `make
install-bpf-boot`) auto-installs any missing packages via apt:

```
sudo apt install clang bpftool libbpf-dev linux-headers-$(uname -r) build-essential
```

`clang` is required (the BPF C is compiled to BPF bytecode; gcc cannot emit
that). A versioned `clang-NN` (e.g. `clang-18`) is accepted automatically - the
build falls back to it if the plain `clang` symlink is absent. `bpftool` and
`libbpf` (libbpf-dev) are also required; `linux-headers-$(uname -r)` and
`build-essential` are needed for the loader.

## Other targets

- `make` / `make all` - build daemon + BPF
- `make daemon` / `make bpf` - build one
- `make check` - smoke-test the daemon binary (no root, no install)
- `make clean` - remove all generated artifacts
- `make grub-revert` is intentionally NOT provided (would disable BPF).

## Local test (no service, no root)

```
make daemon
./src/ChickenBlocker                 # real run
CHICKENBLOCKER_DRY_RUN=1 ./src/ChickenBlocker   # preview, no kills/writes
```
