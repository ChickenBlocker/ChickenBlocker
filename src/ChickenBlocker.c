// SPDX-License-Identifier: GPL-2.0
//
// ChickenBlocker - the ChickenBlocker enforcement daemon (C port of main.py).
//
// A unique binary name gives a unique kernel `comm` ("ChickenBlocker") so the
// BPF LSM signal blocker and any AppArmor rule can match THIS service
// precisely instead of the broad "python3".
//
// Behaviour mirrors main.py:
//   * re-enable the leechblock-ng Firefox extension if it has been disabled
//     (rewrites <profile>/extensions.json)
//   * keep /etc/firefox/policies/policies.json in sync with ./policies.json
//   * keep <profile>/chrome/userChrome.css in sync with ./userChrome.css
//   * when anything changed: kill firefox, drop addonStartup.json.lz4, relaunch
//     firefox as the first logged-in user
//   * repeat enforcement in one persistent process at roughly 30s intervals
//
// Env:
//   CHICKENBLOCKER_DRY_RUN=1   print actions, perform no writes/kills/launch
//
// Build: make          Install: make install  (to /opt/ChickenBlocker/ChickenBlocker)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <dirent.h>
#include <signal.h>
#include <fcntl.h>

// --- generic JSON model -----------------------------------------------------
// Strings are stored OPAQUE: the bytes between the outer quotes (including any
// backslash escape sequences) are kept verbatim and re-emitted verbatim. This
// avoids decoding/encoding escapes (and the corruption risk that brings). We
// only ever need to: compare an "id" string to a plain literal, and set two
// boolean fields. The id literal has no escapes, so raw bytes compare equal.

typedef struct Node Node;
struct Node {
	int type;            // 0 obj, 1 arr, 2 str, 3 bare
	char *raw;            // str: content w/o quotes ; bare: verbatim text
	size_t raw_len;
	Node **vals;         // arr: elements ; obj: values
	char **keys;         // obj only: key raw content w/o quotes
	size_t n, cap;
};

static const char *P, *END;

static void skipws(void) { while (P < END && isspace((unsigned char)*P)) P++; }

static Node *newnode(int t) { Node *n = calloc(1, sizeof *n); n->type = t; return n; }

static void arr_push(Node *a, Node *v) {
	if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->vals = realloc(a->vals, a->cap * sizeof(Node *)); }
	a->vals[a->n++] = v;
}
static void obj_push(Node *o, char *k, Node *v) {
	if (o->n == o->cap) {
		o->cap = o->cap ? o->cap * 2 : 8;
		o->vals = realloc(o->vals, o->cap * sizeof(Node *));
		o->keys = realloc(o->keys, o->cap * sizeof(char *));
	}
	o->keys[o->n] = k; o->vals[o->n] = v; o->n++;
}

static char *read_string(size_t *outlen) {
	P++;                       // opening quote
	const char *s = P;
	while (P < END && *P != '"') {
		if (*P == '\\') { P++; if (P < END) P++; } else P++;
	}
	size_t n = P - s;
	char *r = malloc(n + 1); memcpy(r, s, n); r[n] = 0;
	if (P < END) P++;          // closing quote
	if (outlen) *outlen = n;
	return r;
}

static char *read_bare(size_t *outlen) {
	const char *s = P;
	while (P < END && !isspace((unsigned char)*P) && *P != ',' && *P != '}' &&
	       *P != ']' && *P != ':' && *P != '"')
		P++;
	size_t n = P - s;
	char *r = malloc(n + 1); memcpy(r, s, n); r[n] = 0;
	if (outlen) *outlen = n;
	return r;
}

static Node *parse_value(void);

static Node *parse_array(void) {
	P++;                       // [
	Node *a = newnode(1);
	skipws();
	if (P < END && *P == ']') { P++; return a; }
	for (;;) {
		arr_push(a, parse_value());
		skipws();
		if (P >= END) break;
		if (*P == ',') { P++; skipws(); continue; }
		if (*P == ']') { P++; break; }
		break;
	}
	return a;
}

static Node *parse_object(void) {
	P++;                       // {
	Node *o = newnode(0);
	skipws();
	if (P < END && *P == '}') { P++; return o; }
	for (;;) {
		skipws();
		if (P >= END || *P != '"') break;
		size_t kl; char *k = read_string(&kl);
		skipws();
		if (P < END && *P == ':') P++;
		skipws();
		obj_push(o, k, parse_value());
		skipws();
		if (P >= END) break;
		if (*P == ',') { P++; continue; }
		if (*P == '}') { P++; break; }
		break;
	}
	return o;
}

static Node *parse_value(void) {
	skipws();
	if (P >= END) return newnode(3);
	char c = *P;
	if (c == '"') { size_t l; char *r = read_string(&l); Node *n = newnode(2); n->raw = r; n->raw_len = l; return n; }
	if (c == '{') return parse_object();
	if (c == '[') return parse_array();
	size_t l; char *r = read_bare(&l); Node *n = newnode(3); n->raw = r; n->raw_len = l; return n;
}

static void ser(FILE *f, Node *n) {
	switch (n->type) {
	case 0:
		fputc('{', f);
		for (size_t i = 0; i < n->n; i++) {
			if (i) fputc(',', f);
			fputc('"', f); fputs(n->keys[i], f); fputc('"', f);
			fputc(':', f);
			ser(f, n->vals[i]);
		}
		fputc('}', f);
		break;
	case 1:
		fputc('[', f);
		for (size_t i = 0; i < n->n; i++) { if (i) fputc(',', f); ser(f, n->vals[i]); }
		fputc(']', f);
		break;
	case 2:
		fputc('"', f); fwrite(n->raw, 1, n->raw_len, f); fputc('"', f);
		break;
	default:
		fwrite(n->raw, 1, n->raw_len, f);
	}
}

static void free_node(Node *n) {
	if (!n) return;
	for (size_t i = 0; i < n->n; i++) {
		free(n->keys ? n->keys[i] : NULL);
		free_node(n->vals[i]);
	}
	free(n->keys);
	free(n->vals);
	free(n->raw);
	free(n);
}

static Node *obj_get(Node *o, const char *key) {
	if (o->type != 0) return NULL;
	for (size_t i = 0; i < o->n; i++)
		if (strcmp(o->keys[i], key) == 0) return o->vals[i];
	return NULL;
}

static void set_bare(Node *o, const char *key, const char *val) {
	Node *v = obj_get(o, key);
	if (!v) return;
	v->type = 3;
	free(v->raw);
	v->raw = strdup(val);
	v->raw_len = strlen(val);
}

// --- file helpers -----------------------------------------------------------

static char *read_file(const char *path, size_t *outlen) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) { fclose(f); return NULL; }
	char *b = malloc(sz + 1);
	size_t rd = fread(b, 1, sz, f);
	fclose(f);
	b[rd] = 0;
	if (outlen) *outlen = rd;
	return b;
}

static int files_equal(const char *a, const char *b) {
	size_t la, lb;
	char *ba = read_file(a, &la);
	if (!ba) return 0;
	char *bb = read_file(b, &lb);
	int eq = (la == lb && memcmp(ba, bb, la) == 0);
	free(ba); free(bb);
	return eq;
}

static int copy_file(const char *src, const char *dst) {
	size_t n;
	char *buf = read_file(src, &n);
	if (!buf) return -1;
	FILE *f = fopen(dst, "wb");
	if (!f) { free(buf); return -1; }
	fwrite(buf, 1, n, f);
	fclose(f);
	free(buf);
	chmod(dst, 0644);
	return 0;
}

// mtime of a file (0 if absent). Used to gate Vivaldi restarts so a persistent
// "extension disabled" detection cannot kill+relaunch the browser every cycle.
static time_t file_mtime(const char *path) {
	struct stat st;
	return (stat(path, &st) == 0) ? st.st_mtime : 0;
}

// Create/refresh a file's mtime to now.
static void touch_file(const char *path) {
	FILE *f = fopen(path, "w");
	if (f) { fprintf(f, "1\n"); fclose(f); }
}

// --- user / profile discovery ----------------------------------------------

static int first_user(char *name, size_t nlen) {
	FILE *p = popen("loginctl list-users --no-legend", "r");
	if (!p) return -1;
	char line[256];
	if (!fgets(line, sizeof line, p)) { pclose(p); return -1; }
	pclose(p);
	char uid[64], user[128];
	if (sscanf(line, "%63s %127s", uid, user) != 2) return -1;
	snprintf(name, nlen, "%s", user);
	return 0;
}

static int find_profile_path(const char *firefox_path, char *out, size_t outsz) {
	char ini[1100];
	snprintf(ini, sizeof ini, "%s/profiles.ini", firefox_path);
	FILE *f = fopen(ini, "r");
	if (!f) return -1;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, "Path=", 5) == 0) {
			char *v = line + 5;
			size_t l = strlen(v);
			while (l && (v[l - 1] == '\n' || v[l - 1] == '\r')) v[--l] = 0;
			fclose(f);
			snprintf(out, outsz, "%s/%s", firefox_path, v);
			return 0;
		}
	}
	fclose(f);
	return -1;
}

// --- firefox launch ---------------------------------------------------------

// NOTE on AppArmor: we MUST enumerate /proc and send signals directly via the
// kill() syscall from THIS process, NOT via `system("pkill ...")` / `pgrep`.
// ChickenBlocker runs under the "unconfined" AppArmor label. snap Firefox
// (profile snap.firefox.firefox) allows `signal (receive) peer=unconfined`,
// so a direct kill() from here reaches it. But system() spawns /bin/sh which
// transitions to the confined "shell-bpf" profile; pkill/pgrp would then run
// under "shell-bpf", which snap Firefox does NOT allow to signal -> EPERM
// ("pkill: killing pid ... failed: Permission denied"). Keeping the signal
// in-process preserves the unconfined label.

static int proc_comm_eq(pid_t pid, const char *name) {
	char path[64];
	snprintf(path, sizeof path, "/proc/%d/comm", pid);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	char buf[64];
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0) return 0;
	buf[n] = 0;
	if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
	return strcmp(buf, name) == 0;
}

// Send SIGTERM to every process whose comm == name. Returns the number of
// signals delivered, or -1 on a kill() error other than ESRCH (process gone
// between enumerate and signal, which we just skip).
static int proc_term_all(const char *name) {
	DIR *d = opendir("/proc");
	if (!d) return -1;
	pid_t me = getpid();
	int n = 0, err = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_type != DT_DIR) continue;
		char *e;
		long id = strtol(de->d_name, &e, 10);
		if (*e || id <= 0) continue;
		pid_t pid = (pid_t)id;
		if (pid == me) continue;
		if (!proc_comm_eq(pid, name)) continue;
		if (kill(pid, SIGTERM) == 0) { n++; continue; }
		if (errno == ESRCH) continue;     // raced away; treat as gone
		err = 1;
	}
	closedir(d);
	return err ? -1 : n;
}

// Is any process whose comm == name currently running? (exact match, mirrors
// `pgrep -x`.) Direct /proc scan so we never leave the unconfined label.
static int proc_running(const char *name) {
	DIR *d = opendir("/proc");
	if (!d) return 0;
	struct dirent *de;
	int found = 0;
	while ((de = readdir(d))) {
		if (de->d_type != DT_DIR) continue;
		char *e;
		long id = strtol(de->d_name, &e, 10);
		if (*e || id <= 0) continue;
		if (proc_comm_eq((pid_t)id, name)) { found = 1; break; }
	}
	closedir(d);
	return found;
}

static int firefox_running(void) { return proc_running("firefox"); }

static void launch_firefox(struct passwd *pw) {
	pid_t pid = fork();
	if (pid != 0) return;
	setsid();
	(void)freopen("/dev/null", "r", stdin);
	(void)freopen("/dev/null", "w", stdout);
	(void)freopen("/dev/null", "w", stderr);
	char buf[512];
	setenv("HOME", pw->pw_dir, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("DISPLAY", ":0", 1);
	snprintf(buf, sizeof buf, "unix:path=/run/user/%d/bus", pw->pw_uid);
	setenv("DBUS_SESSION_BUS_ADDRESS", buf, 1);
	snprintf(buf, sizeof buf, "/run/user/%d", pw->pw_uid);
	setenv("XDG_RUNTIME_DIR", buf, 1);
	snprintf(buf, sizeof buf, "%s/.Xauthority", pw->pw_dir);
	setenv("XAUTHORITY", buf, 1);
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/snap/bin", 1);
	initgroups(pw->pw_name, pw->pw_gid);
	(void)setgid(pw->pw_gid);
	(void)setuid(pw->pw_uid);
	execv("/snap/bin/firefox", (char *[]){"/snap/bin/firefox", NULL});
	_exit(127);
}

// Is a process named "vivaldi-bin" currently running? (pgrep -x exact match on comm)
static int vivaldi_running(void) { return proc_running("vivaldi-bin"); }

static ssize_t read_proc_blob(pid_t pid, const char *name, char *buf, size_t bufsz) {
	char path[64];
	snprintf(path, sizeof path, "/proc/%d/%s", pid, name);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, buf, bufsz);
	close(fd);
	return n;
}

// /proc/<pid>/{cmdline,environ} are NUL-separated lists. Vivaldi accepts
// profile roots from command-line switches and Chromium environment variables;
// managed policy does not reliably override --user-data-dir in Vivaldi.
static int nul_list_has_switch(const char *buf, size_t n, const char *key) {
	size_t klen = strlen(key);
	for (size_t off = 0; off + klen <= n; off++) {
		if (off > 0 && buf[off - 1] != '\0' &&
		    !isspace((unsigned char)buf[off - 1]))
			continue;
		if (memcmp(buf + off, key, klen) != 0) continue;
		if (off + klen == n || buf[off + klen] == '\0' ||
		    buf[off + klen] == '=' || isspace((unsigned char)buf[off + klen]))
			return 1;
	}
	return 0;
}

static int nul_env_path_differs(const char *buf, size_t n,
		const char *key, const char *expected) {
	size_t klen = strlen(key);
	for (size_t off = 0; off < n;) {
		size_t len = strnlen(buf + off, n - off);
		if (len > klen && buf[off + klen] == '=' &&
		    memcmp(buf + off, key, klen) == 0) {
			const char *value = buf + off + klen + 1;
			size_t vlen = len - klen - 1;
			return strlen(expected) != vlen || memcmp(value, expected, vlen) != 0;
		}
		off += len + 1;
	}
	return 0;
}

static int vivaldi_process_uses_unmanaged_profile(pid_t pid, const char *home) {
	char buf[65536];
	ssize_t n = read_proc_blob(pid, "cmdline", buf, sizeof buf);
	if (n > 0 &&
	    (nul_list_has_switch(buf, (size_t)n, "--user-data-dir") ||
	     nul_list_has_switch(buf, (size_t)n, "--profile-directory")))
		return 1;

	n = read_proc_blob(pid, "environ", buf, sizeof buf);
	if (n <= 0) return 0;
	char config[1024], vivaldi[1024];
	snprintf(config, sizeof config, "%s/.config", home);
	snprintf(vivaldi, sizeof vivaldi, "%s/.config/vivaldi", home);
	return nul_env_path_differs(buf, (size_t)n, "CHROME_USER_DATA_DIR", vivaldi) ||
	       nul_env_path_differs(buf, (size_t)n, "CHROME_CONFIG_HOME", config) ||
	       nul_env_path_differs(buf, (size_t)n, "XDG_CONFIG_HOME", config);
}

static int vivaldi_unmanaged_profile_running(const char *home) {
	DIR *d = opendir("/proc");
	if (!d) return 0;
	int found = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_type != DT_DIR) continue;
		char *e;
		long id = strtol(de->d_name, &e, 10);
		if (*e || id <= 0 || !proc_comm_eq((pid_t)id, "vivaldi-bin")) continue;
		if (vivaldi_process_uses_unmanaged_profile((pid_t)id, home)) {
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

// Send SIGTERM (directly, in-process -> keeps the unconfined AppArmor label
// so snap Firefox's `signal (receive) peer=unconfined` allows it) to all
// processes whose comm == name, then wait until none remain (or timeout_s
// elapses). Waiting is required: returning before the target has actually
// exited and released its profile / LevelDB write lock raced the subsequent
// relaunch (Vivaldi aborted on a held SingletonLock) and the LeechBlock
// LevelDB sync (plyvel open failed on the held lock). Returns 0 if all gone,
// -1 on a kill error or timeout. Safe to call when nothing matches (no
// signals sent, proc_running immediately reports gone).
static int term_and_wait(const char *name, int timeout_s) {
	if (proc_term_all(name) < 0) return -1;
	for (int i = 0; i < timeout_s * 4; i++) {
		if (!proc_running(name)) return 0;
		usleep(250 * 1000);
	}
	return -1;                        // still alive after timeout
}

// Launch vivaldi-stable as the first logged-in user. Mirrors launch_firefox but
// for the .deb Vivaldi (Chromium-based) install; comm becomes "vivaldi-bin".
static void launch_vivaldi(struct passwd *pw) {
	pid_t pid = fork();
	if (pid != 0) return;
	setsid();
	(void)freopen("/dev/null", "r", stdin);
	(void)freopen("/dev/null", "w", stdout);
	(void)freopen("/dev/null", "w", stderr);
	char buf[512];
	setenv("HOME", pw->pw_dir, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("DISPLAY", ":0", 1);
	snprintf(buf, sizeof buf, "unix:path=/run/user/%d/bus", pw->pw_uid);
	setenv("DBUS_SESSION_BUS_ADDRESS", buf, 1);
	snprintf(buf, sizeof buf, "/run/user/%d", pw->pw_uid);
	setenv("XDG_RUNTIME_DIR", buf, 1);
	snprintf(buf, sizeof buf, "%s/.Xauthority", pw->pw_dir);
	setenv("XAUTHORITY", buf, 1);
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/snap/bin", 1);
	initgroups(pw->pw_name, pw->pw_gid);
	(void)setgid(pw->pw_gid);
	(void)setuid(pw->pw_uid);
	execv("/usr/bin/vivaldi-stable", (char *[]){"/usr/bin/vivaldi-stable", NULL});
	_exit(127);
}

static void enforce_vivaldi_managed_profile(struct passwd *pw, int dry) {
	if (!vivaldi_unmanaged_profile_running(pw->pw_dir)) return;
	printf("VIVALDI UNMANAGED PROFILE: terminating alternate profile\n");
	if (dry) return;
	if (term_and_wait("vivaldi-bin", 10) == 0)
		launch_vivaldi(pw);
	else
		printf("vivaldi unmanaged-profile kill failed; retrying next cycle\n");
}

// --- SafeEyes: pomodoro + night block + AV-aware pause ---------------------
//
// SafeEyes (apt: safeeyes, comm "safeeyes") is enforced as a second layer:
//   * auto-started and kept alive (respawned within ~30s if killed)
//   * DAY (08:00-23:59): a strict pomodoro schedule (30 work / 5 break, long
//     30-min break after 4 cycles), timed ENTIRELY BY THE DAEMON (SafeEyes
//     only renders overlays we impose; its own scheduler is neutered).
//     Breaks are un-skippable/un-postponable; the ONLY escape is an active
//     camera/mic call, which disables SafeEyes for the call's duration (then
//     a fresh work interval starts). Suspend is NOT work time: mid-break it
//     consumes the break; during work it PAUSES the countdown -- and a sleep
//     LONGER than the time left to the interval's end SKIPS the pending
//     break entirely.
//   * NIGHT (00:00-08:00): a single strict 8h break that blanks the screen
//     for the whole night, imposed for exactly the seconds left to 08:00 and
//     re-imposed whenever the overlay is missing. AV check is SKIPPED at
//     night - no call lifts it.
//
// The active config variant is seeded into ~/.config/safeeyes/safeeyes.json
// (as the user) only at a mode boundary or on drift (with its scheduler
// intervals neutered, see SE_NEUTERED_INTERVAL), then SafeEyes is quit and
// relaunched so it reloads (no live-reload). The pomodoro CLOCK itself is
// owned by the daemon (se_work_end / se_cycle below), not by SafeEyes; the
// vendored copy is a heavily patched overlay renderer. State (current mode,
// work deadline, cycle count, whether we paused it for a call) persists in
// /var/lib/ChickenBlocker/se_state.

#define SE_STATE_FILE    "/var/lib/ChickenBlocker/se_state"
#define SE_BIN           "/usr/bin/safeeyes"
// VENDORED SafeEyes: the apt `safeeyes` package is shadowed by a patched copy
// deployed at /opt/ChickenBlocker/safeeyes/ (break_screen.py keyboard grab removed
// so xfce's screen locker can engage during a break). /usr/bin/safeeyes still
// resolves the console_scripts entry point from apt's .egg-info, then does
// `import safeeyes` which PYTHONPATH=/opt/ChickenBlocker redirects to our copy.
// Set on BOTH launch paths (systemd-run --setenv here, setup_user_env below)
// so the main launch and the one-shot -d/-e/-q/-t actions all see the patched
// package. Source of truth is vendor/safeeyes/ in the repo; every
// ChickenBlocker modification is marked inline with "VENDORED PATCH
// (ChickenBlocker)" comments there.
#define SE_PYTHONPATH    "/opt/ChickenBlocker"
#define CB_AV_CHECK      "/opt/ChickenBlocker/cb_av_check.py"
#define CB_BREAK_CHECK   "/opt/ChickenBlocker/cb_break_check.py"
#define CB_CALENDAR_SYNC "/opt/ChickenBlocker/cb_calendar_sync.py"
#define CALENDAR_REQUEST_PREFIX "/tmp/chickenblocker-calendar-sync-"
#define SE_MEETINGS_FILE "/var/lib/ChickenBlocker/se_meetings"
#define SE_MEETINGS_TMP  "/var/lib/ChickenBlocker/.se_meetings.tmp"
#define SE_MAX_MEETINGS  512
#define SE_CONFIRM_SECS  120
#define NIGHT_START_HOUR 0      // night = [00:00, 08:00). Start is FIXED at
#define NIGHT_END_HOUR   8      // midnight — do not shift it; no date override.
// Compile-time pin: the night block must always start at 00:00. Editing
// NIGHT_START_HOUR breaks the build, forcing a deliberate, visible change
// instead of a silent hour bump or a one-night date override.
_Static_assert(NIGHT_START_HOUR == 0, "night must always start at 00:00");
#define RESUME_THRESHOLD     30   // fallback wall-clock gap treated as suspend
#define SE_FORCED_FLAG   "/var/lib/ChickenBlocker/se_forced_by_update"
// Scheduler neutering: the seeded live config gets these (minute) intervals so
// SafeEyes' built-in scheduler can NEVER fire a natural break. The daemon owns
// all timing and imposes overlays explicitly (--impose-short/-long SECS).
#define SE_NEUTERED_INTERVAL 10080   // minutes = 7 days
// Start-work gate hold reset: if the gate (user away / machine suspended) was
// held at least this long, pressing Start begins a FRESH work interval instead
// of restoring the pre-gate remainder (the user effectively rested). Mirrors
// the old cb_startwork idle_gate_reset_seconds.
#define SE_GATE_RESET_SECS 1800LL
// Sentinel written by the cb_startwork plugin while its "Start work" dialog is
// showing (core.stop() was called, scheduler paused). The daemon must NOT
// quit+relaunch SafeEyes while this exists -- doing so kills the process
// holding the dialog and abandons the "press to start" gate. Written/removed
// by the plugin (runs as the user) in /tmp (user-writable).
#define SE_STARTWORK_FLAG "/tmp/cb_startwork_dialog"

struct SafeEyesState {
	int se_strict;        // -1 unknown, 0 day, 1 night (mode currently seeded)
	int se_paused_by_us;  // 0/1: we disabled SE for a call (only day)
	int se_break_active;  // 0/1: a SafeEyes break overlay was up at the last
	                      // cycle. Used on day-resume to re-impose the time
	                      // LEFT if the machine was suspended mid-break (the
	                      // sleep counts toward the break; without this,
	                      // closing the lid mid-break could escape it since
	                      // the daemon-imposed overlay's frozen countdown
	                      // only self-heals once the machine is awake).
	int se_break_type;    // -1 unknown, 0 short, 1 long: type of the break that
	                      // was in progress, so on resume we know the ORIGINAL
	                      // duration (short 5min / long 30min) to compute the
	                      // time LEFT and re-impose only the remainder.
	time_t se_break_start; // wall-clock when the current break was first
	                       // detected (cycle granularity, ~30s). On resume:
	                       // elapsed = suspend_time - se_break_start.
	time_t se_work_end;   // DAEMON-OWNED pomodoro clock: wall-clock deadline
	                      // of the current WORK interval (0 = disarmed: a
	                      // break/gate/meeting holds it). SafeEyes' own
	                      // scheduler is neutered (huge intervals) and only
	                      // shows overlays the daemon imposes via
	                      // `safeeyes --impose-short/-long <secs>`.
	int se_cycle;         // 0..3: work cycles completed since the last LONG
	                      // break. The next imposed break is LONG iff
	                      // se_cycle == 3 (the 4th); incremented when a break
	                      // completes OR is skipped/satisfied by sleep.
	char se_pomodoro_date[11]; // local YYYY-MM-DD owning the work/cycle state
	time_t se_gate_start; // wall-clock when the cb_startwork "Start work"
	                      // gate flag was first seen for the current gate
	                      // (0 = no gate pending).
	long long se_work_left_at_gate; // work seconds remaining when an idle
	                      // gate froze the deadline (-1 = gate began at a
	                      // break end -> fresh interval on Start). If the
	                      // gate was held >= SE_GATE_RESET_SECS (away or
	                      // suspended), Start arms a FRESH interval.
	time_t se_last_cycle; // wall-clock of the previous cycle (resume detection)
	int se_boot_mono_valid; // 0/1: se_boot_mono has been sampled. The valid
	                        // cumulative-suspend value is 0 before first sleep.
	time_t se_boot_mono;  // CLOCK_BOOTTIME - CLOCK_MONOTONIC at the last cycle
	                      // = cumulative seconds spent suspended since boot.
	                      // Robust suspend detector: an increase between cycles
	                      // means a suspend happened, EVEN IF the previous cycle
	                      // was frozen (not killed) by the kernel freezer and
	                      // completed post-resume (which masks the wall-clock gap
	                      // because se_last_cycle gets rewritten at resume time).
	char se_boot_id[37];  // kernel boot ID for distinguishing a reboot from two
	                      // boots with the same cumulative suspend duration.
	unsigned long se_src_hash; // djb2 hash of the source config last seeded
	int se_neutered;       // 0/1: the LIVE config was seeded with neutered
	                       // scheduler intervals (SE_NEUTERED_INTERVAL). A
	                       // state file from an older daemon lacks this (0),
	                       // forcing one re-seed on upgrade so a stale
	                       // un-neutered live config (e.g. the old night
	                       // config's 1-min interval) can never fire a
	                       // natural break.
	char se_cal_hour[16]; // last attempted local calendar hour (YYYY-MM-DDTHH)
	int se_meeting_phase; // 0 idle, 1 waiting for camera, 2 camera confirmed
	char se_meeting_id[65];
	time_t se_meeting_end;
	time_t se_confirm_deadline;
	time_t se_camera_last_seen;
	int se_vt_masked;   // 0/1: VT logins masked+stopped (night VT guard active)
};

struct MeetingOccurrence {
	char id[65];
	time_t start;
	time_t end;
	int attempted;
	int seen;
};

struct MeetingLedger {
	struct MeetingOccurrence entries[SE_MAX_MEETINGS];
	size_t n;
	int version;
};

// Set the session env (X11 + D-Bus) for a child that will drop to the user.
// Mirrors the env block in launch_firefox (DISPLAY=:0 matches this X11 box).
static void setup_user_env(struct passwd *pw) {
	char buf[512];
	setenv("HOME", pw->pw_dir, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("DISPLAY", ":0", 1);
	snprintf(buf, sizeof buf, "unix:path=/run/user/%d/bus", pw->pw_uid);
	setenv("DBUS_SESSION_BUS_ADDRESS", buf, 1);
	snprintf(buf, sizeof buf, "/run/user/%d", pw->pw_uid);
	setenv("XDG_RUNTIME_DIR", buf, 1);
	snprintf(buf, sizeof buf, "%s/.Xauthority", pw->pw_dir);
	setenv("XAUTHORITY", buf, 1);
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/snap/bin", 1);
	// Shadow the apt safeeyes package with the vendored patched copy. See
	// SE_PYTHONPATH above: /opt/ChickenBlocker is the PARENT of the safeeyes/
	// package dir, so `import safeeyes` resolves to /opt/ChickenBlocker/safeeyes/.
	setenv("PYTHONPATH", SE_PYTHONPATH, 1);
}

// Fork + exec `path argv` as the logged-in user. If wait_s > 0, wait up to
// that many seconds for the child to exit (one-shot GApplication actions like
// `safeeyes -d/-e/-q` connect to the running instance and exit quickly); if
// wait_s == 0, return immediately after the fork (detached launch).
// Returns 0 if the child exited 0 (or forked off when not waiting), -1 on
// exec/fork failure or non-zero/timeout exit.
static int exec_as_user(struct passwd *pw, const char *path, char *const argv[], int wait_s) {
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		setsid();
		(void)freopen("/dev/null", "r", stdin);
		(void)freopen("/dev/null", "w", stdout);
		(void)freopen("/dev/null", "w", stderr);
		setup_user_env(pw);
		initgroups(pw->pw_name, pw->pw_gid);
		(void)setgid(pw->pw_gid);
		(void)setuid(pw->pw_uid);
		execv(path, argv);
		_exit(127);
	}
	if (wait_s <= 0) return 0;
	for (int i = 0; i < wait_s * 4; i++) {
		int status = 0;
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
		if (r < 0) return -1;
		usleep(250 * 1000);
	}
	// Timed out: leave it (best-effort). Caller may follow up (e.g. term).
	return -1;
}

static int safeeyes_running(void) { return proc_running("safeeyes"); }

// Start SafeEyes detached as the user (no args -> GApplication launch).
//
// Launch via `systemd-run` into its own cgroup, not a plain fork+exec. This
// keeps Safe Eyes alive across a daemon failure/update and prevents
// KillMode=control-group from coupling the two services. If the transient unit
// already exists and is active, systemd-run fails, but this is only called when
// proc_running reports Safe Eyes down.
static void launch_safeeyes(struct passwd *pw) {
	char uid[32], gid[32];
	snprintf(uid, sizeof uid, "%u", pw->pw_uid);
	snprintf(gid, sizeof gid, "%u", pw->pw_gid);
	// Each --setenv must be a single KEY=VALUE argv string; splitting the
	// value into a separate argv entry makes systemd-run treat the value as
	// the command to execute ("Failed to find executable /home/example").
	char e_home[600], e_user[128], e_log[128], e_dbus[600], e_xrt[160],
	     e_xauth[700], e_path[160], e_disp[32], e_pypath[64];
	snprintf(e_disp,  sizeof e_disp,  "DISPLAY=%s", ":0");
	snprintf(e_home,  sizeof e_home,  "HOME=%s", pw->pw_dir);
	snprintf(e_user,  sizeof e_user,  "USER=%s", pw->pw_name);
	snprintf(e_log,   sizeof e_log,   "LOGNAME=%s", pw->pw_name);
	snprintf(e_dbus,  sizeof e_dbus,  "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%d/bus", pw->pw_uid);
	snprintf(e_xrt,   sizeof e_xrt,   "XDG_RUNTIME_DIR=/run/user/%d", pw->pw_uid);
	snprintf(e_xauth, sizeof e_xauth, "XAUTHORITY=%s/.Xauthority", pw->pw_dir);
	snprintf(e_path,  sizeof e_path,  "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/snap/bin");
	snprintf(e_pypath,sizeof e_pypath, "PYTHONPATH=%s", SE_PYTHONPATH);
	char *argv[] = {
		(char *)"systemd-run",
		"--unit=ChickenBlocker-safeeyes",
		"--collect",                 // auto-remove the transient unit when it
		                             // exits, so the next launch can reuse the
		                             // name (otherwise "unit already loaded" ->1)
		"--uid", uid,
		"--gid", gid,
		"--setenv", e_disp,
		"--setenv", e_home,
		"--setenv", e_user,
		"--setenv", e_log,
		"--setenv", e_dbus,
		"--setenv", e_xrt,
		"--setenv", e_xauth,
		"--setenv", e_path,
		"--setenv", e_pypath,
		(char *)SE_BIN,
		NULL
	};
	// Run systemd-run AS ROOT (ChickenBlocker is root); --uid/--gid make the
	// transient unit drop to the user. system systemd-run as non-root would
	// need polkit, so we must NOT setuid here. Fork+exec directly (no
	// privilege drop), wait briefly for systemd-run to exit (it returns once
	// the unit is queued).
	pid_t pid = fork();
	if (pid < 0) { printf("SAFEEYES: fork failed\n"); return; }
	if (pid == 0) {
		(void)freopen("/dev/null", "r", stdin);
		// NOTE: stdout/stderr left connected to the daemon's journal stream
		// so any systemd-run error is visible in the journal (was /dev/null,
		// hiding the real failure cause).
		execvp("systemd-run", argv);
		perror("SAFEEYES: execvp systemd-run");
		_exit(127);
	}
	for (int i = 0; i < 6 * 4; i++) {
		int status = 0;
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
				printf("SAFEEYES: systemd-run exited %d\n", WEXITSTATUS(status));
			return;
		}
		if (r < 0) return;
		usleep(250 * 1000);
	}
	printf("SAFEEYES: systemd-run timed out\n");
}

// Run a one-shot `safeeyes <arg>` as the user and wait briefly.
static int safeeyes_cmd(struct passwd *pw, const char *arg) {
	char *argv[] = { (char *)SE_BIN, (char *)arg, NULL };
	return exec_as_user(pw, SE_BIN, argv, 6);
}

// Quit the running SafeEyes (-q) and, if it doesn't exit, SIGTERM it directly.
static void safeeyes_quit(struct passwd *pw, int dry) {
	if (dry) { printf("SAFEEYES: would quit\n"); return; }
	safeeyes_cmd(pw, "-q");
	for (int i = 0; i < 5 * 4 && safeeyes_running(); i++) usleep(250 * 1000);
	if (safeeyes_running()) {
		printf("SAFEEYES: -q didn't exit; SIGTERM\n");
		term_and_wait("safeeyes", 5);
	}
}

// Impose a break overlay of the given type and duration via the vendored
// SafeEyes CLI (--impose-short/-long SECS). The daemon owns ALL break timing;
// SafeEyes' own scheduler is neutered and only renders overlays on command.
// Retries across ~24s: right after a (re)launch the GApplication may not have
// its scheduler wait armed yet, so the CLI exits non-zero until ready.
// Returns 0 if the impose was honoured (or a break was already up), -1 if not.
static int safeeyes_impose(struct passwd *pw, int dry, int is_long, long long secs) {
	if (dry) {
		printf("SAFEEYES: would impose %s break (%llds)\n",
			is_long ? "long" : "short", secs);
		return 0;
	}
	// Wait for SafeEyes to be running (after a relaunch).
	for (int i = 0; i < 8 * 4 && !safeeyes_running(); i++) usleep(250 * 1000);
	if (!safeeyes_running()) {
		printf("SAFEEYES: impose failed; safeeyes not running\n");
		return -1;
	}
	char secs_str[24];
	snprintf(secs_str, sizeof secs_str, "%lld", secs);
	for (int i = 0; i < 10 && safeeyes_running(); i++) {
		char *argv[] = {
			(char *)SE_BIN,
			is_long ? (char *)"--impose-long" : (char *)"--impose-short",
			secs_str,
			NULL
		};
		if (i == 0)
			printf("SAFEEYES: imposing %s break (%llds)\n",
				is_long ? "long" : "short", secs);
		if (exec_as_user(pw, SE_BIN, argv, 8) == 0)
			return 0;
		usleep(2000 * 1000);
	}
	printf("SAFEEYES: impose not accepted\n");
	return -1;
}

// Copy src to dst and chown to the user (so ~/.config/safeeyes/safeeyes.json
// is user-owned, matching a normal SafeEyes install).
static int copy_file_user(const char *src, const char *dst, struct passwd *pw) {
	size_t n;
	char *buf = read_file(src, &n);
	if (!buf) return -1;
	FILE *f = fopen(dst, "wb");
	if (!f) { free(buf); return -1; }
	fwrite(buf, 1, n, f);
	fclose(f);
	free(buf);
	chmod(dst, 0644);
	if (pw) chown(dst, pw->pw_uid, pw->pw_gid);
	return 0;
}

// --- Ctrl+Alt+Esc "kill cursor" + logout guard (XFCE) ------------------------
// Impulsive escape vectors during a SafeEyes break, none of which are signals
// or are covered by the BPF signal gate:
//   * XFCE binds <Primary><Alt>Escape to the `xkill` command (cross cursor);
//     the click makes the X SERVER close the target client's display
//     connection, so SafeEyes dies via its X-error/exit() path.
//   * <Primary><Shift>Escape opens xfce4-taskmanager, which can SIGKILL/
//     SIGSTOP safeeyes from its window list.
//   * whisker-menu / panel "Log Out" exec xfce4-session-logout; ending the
//     session tears safeeyes down with it (the daemon relaunches it, but the
//     overlay blinks out for ~30s and reappears over the greeter).
// Counters, re-asserted each cycle (re-applied within ~30s if re-added or an
// apt upgrade restores a binary mode):
//   1. chmod 0000 the launcher binaries (root's access() X_OK probe succeeds
//      iff any exec bit is set, so this is idempotent),
//   2. drop the two xfce4-keyboard-shortcuts bindings via xfconf-query as the
//      user (xfwm4 applies xfconf changes live; an absent property just makes
//      xfconf-query fail and is ignored).
static const char *const killguard_binaries[] = {
	"/usr/bin/xkill",                 // Ctrl+Alt+Esc kill cursor
	"/usr/bin/xfce4-taskmanager",     // Ctrl+Alt+Shift+Esc / menu: SIGKILL UI
	"/usr/bin/xfce4-session-logout",  // whisker-menu / panel Log Out
};

static const char *const killguard_bindings[] = {
	"/commands/custom/<Primary><Alt>Escape",     // -> xkill
	"/commands/custom/<Primary><Shift>Escape",   // -> xfce4-taskmanager
	"/commands/custom/<Primary><Alt>Delete",     // -> xfce4-session-logout
};

static void de_killguard(struct passwd *pw, int dry, int lock_power) {
	for (size_t i = 0; i < sizeof killguard_binaries / sizeof *killguard_binaries;
			i++) {
		const char *path = killguard_binaries[i];
		if (access(path, F_OK) != 0 || access(path, X_OK) != 0)
			continue;
		if (dry) {
			printf("KILLGUARD: would chmod 0000 %s\n", path);
		} else if (chmod(path, 0000) == 0) {
			printf("KILLGUARD: %s disabled (mode 0000)\n", path);
		} else {
			printf("KILLGUARD: chmod %s failed: %s\n", path, strerror(errno));
		}
	}
	if (dry) {
		printf("KILLGUARD: would unbind Ctrl+Alt+Esc / Ctrl+Alt+Shift+Esc / "
			"Ctrl+Alt+Del\n");
	} else {
		for (size_t i = 0;
				i < sizeof killguard_bindings / sizeof *killguard_bindings; i++) {
			char *argv[] = {
				(char *)"/usr/bin/xfconf-query",
				(char *)"-c", (char *)"xfce4-keyboard-shortcuts",
				(char *)"-p", (char *)killguard_bindings[i],
				(char *)"-r", NULL
			};
			(void)exec_as_user(pw, "/usr/bin/xfconf-query", argv, 4);
		}
	}
	// Power button: scoped to breaks (lock_power). XFPM reads its properties
	// under the "/xfce4-power-manager/" prefix inside the channel; the stock
	// value is 3 ("ask": pop the shutdown dialog). A break pins it to 0
	// (Nothing) so the power key can't open the logout dialog while an
	// overlay is up, and a day work interval restores 3. --create re-creates
	// the property if a future xfpm reset drops it.
	char val[8];
	snprintf(val, sizeof val, "%d", lock_power ? 0 : 3);
	if (dry) {
		printf("KILLGUARD: power-button-action = %s\n", val);
		return;
	}
	char *pwr[] = {
		(char *)"/usr/bin/xfconf-query",
		(char *)"-c", (char *)"xfce4-power-manager",
		(char *)"-p", (char *)"/xfce4-power-manager/power-button-action",
		(char *)"-t", (char *)"uint", (char *)"-s", val,
		(char *)"--create", NULL
	};
	(void)exec_as_user(pw, "/usr/bin/xfconf-query", pwr, 4);
}

// --- Night VT-login lockdown (see .kilo/plans/vt_lockdown_night.md) ----------
static int se_read_state(struct SafeEyesState *st);
static void se_write_state(const struct SafeEyesState *st);
// Ctrl+Alt+F1..F6 must not offer a login prompt during the night block
// ([00:00, 08:00)), and a console shell left open before midnight must not
// survive it. Masking `autovt@.service` is the load-bearing part: logind
// spawns it on demand when an empty VT is switched to, so stopping gettys
// alone is undone by one keypress. Stopping `getty@ttyN` also kills any
// already-logged-in session in that unit's cgroup. The X session is never
// touched: it lives in lightdm's cgroup on its own VT, and getty@tty7 is not
// among the stopped units.
//
// The night actions are RE-ASSERTED EVERY CYCLE (~30s, tighter than the 10 min
// the lockdown was specced for): a manual `systemctl unmask` at night is
// reverted within one cycle, and there is no pre-made unmask helper around.
// All calls are short D-Bus roundtrips — a few ms of CPU per cycle. The mask
// symlink persists in /etc across reboots; se_vt_masked tracks whether a day
// restore is owed, while the live systemd state is also checked so masks made
// before a daemon update/restart are recovered.
static int vt_mask_status(const char *status) {
	return !strcmp(status, "masked") || !strcmp(status, "masked-runtime");
}

// Return 1 when the template is masked, 0 when it is not, and -1 when its
// state could not be read. `systemctl is-enabled` intentionally exits nonzero
// for a masked unit, so its text output, not pclose's status, is authoritative.
static int autovt_is_masked(void) {
	FILE *p = popen("systemctl is-enabled autovt@.service 2>/dev/null", "r");
	if (!p) return -1;
	char status[64];
	int got = fgets(status, sizeof status, p) != NULL;
	(void)pclose(p);
	if (!got) return -1;
	char *nl = strchr(status, '\n');
	if (nl) *nl = 0;
	return vt_mask_status(status);
}

static void vt_night_guard(int want_strict, int dry) {
	struct SafeEyesState st;
	se_read_state(&st);
	if (want_strict) {
		if (dry) {
			printf("VTGUARD: mask+stop autovt@/getty@tty1-6\n");
			return;
		}
		if (system("systemctl mask --now autovt@.service >/dev/null 2>&1") != 0) {
			printf("VTGUARD: mask autovt@ failed; retry next cycle\n");
			return;
		}
		for (int t = 1; t <= 6; t++) {
			char cmd[80];
			snprintf(cmd, sizeof cmd,
				"systemctl stop getty@tty%d.service >/dev/null 2>&1", t);
			if (system(cmd) != 0)
				printf("VTGUARD: stop getty@tty%d failed\n", t);
		}
		if (!st.se_vt_masked) {
			st.se_vt_masked = 1;
			se_write_state(&st);
			printf("VTGUARD: VT logins locked out (night, re-asserted each cycle)\n");
		}
	} else {
		int masked = autovt_is_masked();
		if (!st.se_vt_masked && masked != 1) return;
		if (dry) {
			printf("VTGUARD: would unmask autovt@ + start getty@tty1\n");
			return;
		}
		if (system("systemctl unmask autovt@.service >/dev/null 2>&1") != 0) {
			printf("VTGUARD: unmask autovt@ failed\n");
			return;
		}
		if (system("systemctl start getty@tty1.service >/dev/null 2>&1") != 0) {
			printf("VTGUARD: start getty@tty1 failed\n");
			if (!st.se_vt_masked) {
				st.se_vt_masked = 1;
				se_write_state(&st);
			}
			return;
		}
		st.se_vt_masked = 0;
		se_write_state(&st);
		printf("VTGUARD: VT logins restored (day)\n");
	}
}

static int se_read_state(struct SafeEyesState *st) {
	st->se_strict = -1;
	st->se_paused_by_us = 0;
	st->se_break_active = 0;
	st->se_break_type = -1;
	st->se_break_start = 0;
	st->se_work_end = 0;
	st->se_cycle = 0;
	st->se_pomodoro_date[0] = 0;
	st->se_gate_start = 0;
	st->se_work_left_at_gate = -1;
	st->se_last_cycle = 0;
	st->se_boot_mono_valid = 0;
	st->se_boot_mono = 0;
	st->se_boot_id[0] = 0;
	st->se_src_hash = 0;
	st->se_neutered = 0;
	st->se_cal_hour[0] = 0;
	st->se_meeting_phase = 0;
	st->se_meeting_id[0] = 0;
	st->se_meeting_end = 0;
	st->se_confirm_deadline = 0;
	st->se_camera_last_seen = 0;
	st->se_vt_masked = 0;
	FILE *f = fopen(SE_STATE_FILE, "r");
	if (!f) return -1;
	char line[128];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0; char *v = eq + 1;
		size_t l = strlen(v); while (l && (v[l-1]=='\n'||v[l-1]=='\r')) v[--l]=0;
		if (!strcmp(line, "se_strict")) st->se_strict = atoi(v);
		else if (!strcmp(line, "se_paused_by_us")) st->se_paused_by_us = atoi(v);
		else if (!strcmp(line, "se_break_active")) st->se_break_active = atoi(v);
		else if (!strcmp(line, "se_break_type")) st->se_break_type = atoi(v);
		else if (!strcmp(line, "se_break_start")) st->se_break_start = (time_t)atoll(v);
		else if (!strcmp(line, "se_work_end")) st->se_work_end = (time_t)atoll(v);
		else if (!strcmp(line, "se_cycle")) st->se_cycle = atoi(v) & 3;
		else if (!strcmp(line, "se_pomodoro_date")) snprintf(st->se_pomodoro_date, sizeof st->se_pomodoro_date, "%s", v);
		else if (!strcmp(line, "se_gate_start")) st->se_gate_start = (time_t)atoll(v);
		else if (!strcmp(line, "se_work_left_at_gate")) st->se_work_left_at_gate = atoll(v);
		else if (!strcmp(line, "se_last_cycle")) st->se_last_cycle = (time_t)atoll(v);
		else if (!strcmp(line, "se_boot_mono_valid")) st->se_boot_mono_valid = atoi(v);
		else if (!strcmp(line, "se_boot_mono")) st->se_boot_mono = (time_t)atoll(v);
		else if (!strcmp(line, "se_boot_id")) snprintf(st->se_boot_id, sizeof st->se_boot_id, "%s", v);
		else if (!strcmp(line, "se_src_hash")) st->se_src_hash = strtoul(v, NULL, 10);
		else if (!strcmp(line, "se_neutered")) st->se_neutered = atoi(v);
		else if (!strcmp(line, "se_cal_hour")) snprintf(st->se_cal_hour, sizeof st->se_cal_hour, "%s", v);
		else if (!strcmp(line, "se_meeting_phase")) st->se_meeting_phase = atoi(v);
		else if (!strcmp(line, "se_meeting_id")) snprintf(st->se_meeting_id, sizeof st->se_meeting_id, "%s", v);
		else if (!strcmp(line, "se_meeting_end")) st->se_meeting_end = (time_t)atoll(v);
		else if (!strcmp(line, "se_confirm_deadline")) st->se_confirm_deadline = (time_t)atoll(v);
		else if (!strcmp(line, "se_camera_last_seen")) st->se_camera_last_seen = (time_t)atoll(v);
		else if (!strcmp(line, "se_vt_masked")) st->se_vt_masked = atoi(v);
	}
	fclose(f);
	return 0;
}

static void se_write_state(const struct SafeEyesState *st) {
	mkdir("/var/lib/ChickenBlocker", 0700);
	FILE *f = fopen(SE_STATE_FILE, "w");
	if (!f) return;
	chmod(SE_STATE_FILE, 0600);
	fprintf(f, "se_strict=%d\n", st->se_strict);
	fprintf(f, "se_paused_by_us=%d\n", st->se_paused_by_us);
	fprintf(f, "se_break_active=%d\n", st->se_break_active);
	fprintf(f, "se_break_type=%d\n", st->se_break_type);
	fprintf(f, "se_break_start=%lld\n", (long long)st->se_break_start);
	fprintf(f, "se_work_end=%lld\n", (long long)st->se_work_end);
	fprintf(f, "se_cycle=%d\n", st->se_cycle);
	fprintf(f, "se_pomodoro_date=%s\n", st->se_pomodoro_date);
	fprintf(f, "se_gate_start=%lld\n", (long long)st->se_gate_start);
	fprintf(f, "se_work_left_at_gate=%lld\n", st->se_work_left_at_gate);
	fprintf(f, "se_last_cycle=%lld\n", (long long)st->se_last_cycle);
	fprintf(f, "se_boot_mono_valid=%d\n", st->se_boot_mono_valid);
	fprintf(f, "se_boot_mono=%lld\n", (long long)st->se_boot_mono);
	fprintf(f, "se_boot_id=%s\n", st->se_boot_id);
	fprintf(f, "se_src_hash=%lu\n", st->se_src_hash);
	fprintf(f, "se_neutered=%d\n", st->se_neutered);
	fprintf(f, "se_cal_hour=%s\n", st->se_cal_hour);
	fprintf(f, "se_meeting_phase=%d\n", st->se_meeting_phase);
	fprintf(f, "se_meeting_id=%s\n", st->se_meeting_id);
	fprintf(f, "se_meeting_end=%lld\n", (long long)st->se_meeting_end);
	fprintf(f, "se_confirm_deadline=%lld\n", (long long)st->se_confirm_deadline);
	fprintf(f, "se_camera_last_seen=%lld\n", (long long)st->se_camera_last_seen);
	fprintf(f, "se_vt_masked=%d\n", st->se_vt_masked);
	fclose(f);
}

// Run cb_av_check.py as the user (pw-dump needs the user's PipeWire socket).
// Returns 1 if camera or mic is active, 0 if idle, -1 on error.
static int av_active(struct passwd *pw, int *cam, int *mic) {
	*cam = *mic = 0;
	int pfd[2];
	if (pipe(pfd) != 0) return -1;
	pid_t pid = fork();
	if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[1]);
		(void)freopen("/dev/null", "r", stdin);
		(void)freopen("/dev/null", "w", stderr);
		setup_user_env(pw);
		initgroups(pw->pw_name, pw->pw_gid);
		(void)setgid(pw->pw_gid);
		(void)setuid(pw->pw_uid);
		execlp("python3", "python3", CB_AV_CHECK, (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	char buf[64]; ssize_t n = read(pfd[0], buf, sizeof buf - 1);
	close(pfd[0]);
	int status = 0; (void)waitpid(pid, &status, 0);
	if (n <= 0) return -1;
	buf[n] = 0;
	char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
	if (!strcmp(buf, "CAMERA")) { *cam = 1; return 1; }
	if (!strcmp(buf, "MIC"))    { *mic = 1; return 1; }
	if (!strcmp(buf, "IDLE"))   { return 0; }
	return -1;
}

static int occurrence_id_valid(const char *id) {
	if (strlen(id) != 64) return 0;
	for (const char *p = id; *p; p++)
		if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f')) return 0;
	return 1;
}

static void local_date_for(time_t value, char out[16]) {
	struct tm tm;
	localtime_r(&value, &tm);
	strftime(out, 16, "%Y-%m-%d", &tm);
}

static int meeting_ledger_find(const struct MeetingLedger *ledger, const char *id) {
	for (size_t i = 0; i < ledger->n; i++)
		if (!strcmp(ledger->entries[i].id, id)) return (int)i;
	return -1;
}

static int meeting_ledger_find_unseen_time(const struct MeetingLedger *ledger,
		time_t start, time_t end) {
	for (size_t i = 0; i < ledger->n; i++)
		if (!ledger->entries[i].seen && ledger->entries[i].start == start
				&& ledger->entries[i].end == end)
			return (int)i;
	return -1;
}

static int meeting_occurrence_parse(const char *line,
		struct MeetingOccurrence *event) {
	char id[65], extra;
	long long start, end;
	int attempted;
	if (sscanf(line, "event=%64[0-9a-f],%lld,%lld,%d %c",
			id, &start, &end, &attempted, &extra) != 4
			|| !occurrence_id_valid(id) || end <= start)
		return -1;
	snprintf(event->id, sizeof event->id, "%s", id);
	event->start = (time_t)start;
	event->end = (time_t)end;
	event->attempted = attempted ? 1 : 0;
	event->seen = 0;
	return 0;
}

static int meeting_ledger_read(struct MeetingLedger *ledger) {
	memset(ledger, 0, sizeof *ledger);
	FILE *f = fopen(SE_MEETINGS_FILE, "r");
	if (!f) return -1;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "version=%d", &ledger->version) == 1) continue;
		struct MeetingOccurrence event;
		if (meeting_occurrence_parse(line, &event) != 0
				|| ledger->n >= SE_MAX_MEETINGS)
			continue;
		ledger->entries[ledger->n++] = event;
	}
	fclose(f);
	return 0;
}

static int meeting_ledger_write(const struct MeetingLedger *ledger) {
	mkdir("/var/lib/ChickenBlocker", 0700);
	FILE *f = fopen(SE_MEETINGS_TMP, "w");
	if (!f) return -1;
	chmod(SE_MEETINGS_TMP, 0600);
	fprintf(f, "version=2\n");
	for (size_t i = 0; i < ledger->n; i++) {
		const struct MeetingOccurrence *event = &ledger->entries[i];
		fprintf(f, "event=%s,%lld,%lld,%d\n", event->id,
			(long long)event->start, (long long)event->end, event->attempted);
	}
	if (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0) {
		unlink(SE_MEETINGS_TMP);
		return -1;
	}
	if (rename(SE_MEETINGS_TMP, SE_MEETINGS_FILE) != 0) {
		unlink(SE_MEETINGS_TMP);
		return -1;
	}
	return 0;
}

// Run the calendar wrapper as the desktop user. It emits only:
//   OK
//   EVENT <sha256> <start_epoch> <end_epoch>
static int calendar_snapshot(struct passwd *pw, struct MeetingLedger *snapshot) {
	memset(snapshot, 0, sizeof *snapshot);
	int pfd[2];
	if (pipe(pfd) != 0) return -1;
	pid_t pid = fork();
	if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[1]);
		(void)freopen("/dev/null", "r", stdin);
		(void)freopen("/dev/null", "w", stderr);
		setup_user_env(pw);
		initgroups(pw->pw_name, pw->pw_gid);
		(void)setgid(pw->pw_gid);
		(void)setuid(pw->pw_uid);
		execlp("python3", "python3", CB_CALENDAR_SYNC, (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	FILE *f = fdopen(pfd[0], "r");
	if (!f) { close(pfd[0]); return -1; }
	char line[256];
	int valid = 1, saw_ok = 0;
	while (fgets(line, sizeof line, f)) {
		char *nl = strchr(line, '\n'); if (nl) *nl = 0;
		if (!saw_ok) {
			if (strcmp(line, "OK") != 0) valid = 0;
			saw_ok = 1;
			continue;
		}
		char id[65], extra;
		long long start, end;
		if (sscanf(line, "EVENT %64[0-9a-f] %lld %lld %c",
				id, &start, &end, &extra) != 3
				|| !occurrence_id_valid(id) || end <= start
				|| snapshot->n >= SE_MAX_MEETINGS) {
			valid = 0;
			continue;
		}
		struct MeetingOccurrence *event = &snapshot->entries[snapshot->n++];
		snprintf(event->id, sizeof event->id, "%s", id);
		event->start = (time_t)start;
		event->end = (time_t)end;
	}
	fclose(f);
	int status = 0;
	if (waitpid(pid, &status, 0) != pid) return -1;
	if (!saw_ok || !valid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

static int meeting_ledger_reconcile(struct passwd *pw, struct MeetingLedger *ledger,
		const char *today) {
	struct MeetingLedger snapshot;
	if (calendar_snapshot(pw, &snapshot) != 0) return -1;
	for (size_t i = 0; i < ledger->n; i++) ledger->entries[i].seen = 0;
	int changed = 0;
	int migrate_legacy = ledger->version < 2;
	for (size_t i = 0; i < snapshot.n; i++) {
		struct MeetingOccurrence *incoming = &snapshot.entries[i];
		int found = meeting_ledger_find(ledger, incoming->id);
		if (found >= 0) {
			ledger->entries[found].seen = 1;
			continue;
		}
		if (migrate_legacy) {
			found = meeting_ledger_find_unseen_time(ledger, incoming->start,
				incoming->end);
			if (found >= 0) {
				snprintf(ledger->entries[found].id,
					sizeof ledger->entries[found].id, "%s", incoming->id);
				ledger->entries[found].seen = 1;
				changed = 1;
				printf("CALENDAR: migrated occurrence identity %.12s\n",
					incoming->id);
				continue;
			}
		}
		char event_date[16];
		local_date_for(incoming->start, event_date);
		if (strcmp(event_date, today) <= 0) continue;
		if (ledger->n >= SE_MAX_MEETINGS) return -1;
		struct MeetingOccurrence *event = &ledger->entries[ledger->n++];
		*event = *incoming;
		event->seen = 1;
		event->attempted = 0;
		changed = 1;
		printf("CALENDAR: recorded future occurrence %.12s for %s\n",
			event->id, event_date);
	}
	for (size_t i = 0; i < ledger->n;) {
		if (ledger->entries[i].seen) { i++; continue; }
		printf("CALENDAR: removed absent/cancelled occurrence %.12s\n",
			ledger->entries[i].id);
		ledger->entries[i] = ledger->entries[ledger->n - 1];
		ledger->n--;
		changed = 1;
	}
	if (migrate_legacy) {
		ledger->version = 2;
		changed = 1;
	}
	if (changed && meeting_ledger_write(ledger) != 0) return -1;
	printf("CALENDAR: synchronized %zu eligible occurrences\n", ledger->n);
	return 0;
}

static void meeting_state_clear(struct SafeEyesState *st) {
	st->se_meeting_phase = 0;
	st->se_meeting_id[0] = 0;
	st->se_meeting_end = 0;
	st->se_confirm_deadline = 0;
	st->se_camera_last_seen = 0;
}

// Run cb_break_check.py as the user (Wnck needs the user's X session).
// Returns the break status:
//    0 = IDLE (no break overlay)
//    1 = BREAK_SHORT (short break up)
//    2 = BREAK_LONG  (long break up)
//    3 = BREAK       (break up, type unknown -- treat as short)
//   -1 = error / unavailable
static int break_status(struct passwd *pw, int dry) {
	if (dry) return -1;
	int pfd[2];
	if (pipe(pfd) != 0) return -1;
	pid_t pid = fork();
	if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[1]);
		(void)freopen("/dev/null", "r", stdin);
		(void)freopen("/dev/null", "w", stderr);
		setup_user_env(pw);
		initgroups(pw->pw_name, pw->pw_gid);
		(void)setgid(pw->pw_gid);
		(void)setuid(pw->pw_uid);
		execlp("python3", "python3", CB_BREAK_CHECK, (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	char buf[64]; ssize_t n = read(pfd[0], buf, sizeof buf - 1);
	close(pfd[0]);
	int status = 0; (void)waitpid(pid, &status, 0);
	if (n <= 0) return -1;
	buf[n] = 0;
	char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
	if (!strcmp(buf, "IDLE"))       return 0;
	if (!strcmp(buf, "BREAK_SHORT")) return 1;
	if (!strcmp(buf, "BREAK_LONG"))  return 2;
	if (!strcmp(buf, "BREAK"))       return 3;
	return -1;
}

// Read an integer value following `"key":` in a JSON file (mirror of
// patch_json_int, but read-only). Used to get the real short/long break
// durations from the SOURCE day config to compute time-left on resume.
// Returns the value, or def on miss/parse error.
static long long get_json_int(const char *path, const char *key, long long def) {
	size_t n;
	char *buf = read_file(path, &n);
	if (!buf) return def;
	char needle[128];
	int nlen = snprintf(needle, sizeof needle, "\"%s\":", key);
	char *p = strstr(buf, needle);
	if (!p) { free(buf); return def; }
	p += nlen;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	long long v = strtoll(p, NULL, 10);
	free(buf);
	return v;
}

// Seconds remaining until the next 08:00 local (night -> morning boundary).
static long long secs_until_morning(time_t now) {
	struct tm morning; localtime_r(&now, &morning);
	morning.tm_hour = NIGHT_END_HOUR;
	morning.tm_min = 0;
	morning.tm_sec = 0;
	time_t m = mktime(&morning);
	if (m <= now) m += 24 * 3600;   // guard: only happens outside the night window
	return (long long)(m - now);
}

// Adjust a WORK deadline after a resume from suspend. Suspend time is NOT
// work time: a sleep SHORTER than the work time that was left when the
// machine went down just PAUSES the countdown (deadline pushed by the sleep
// duration -- the user still owes every remaining minute). A sleep LONGER
// than the time left means the user rested more than the pending break was
// worth: the break is skipped (cycle advances, *skip set) and a fresh
// interval starts at wake. `now` is the wake-side cycle time; the sleep
// start is approximated as now - slept (cycle granularity, ~30s).
static time_t se_work_resume_adjust(time_t work_end, time_t now,
		long long slept, long long work_interval, int *skip) {
	time_t sleep_start = now - (time_t)slept;
	long long left_at_sleep = (long long)(work_end - sleep_start);
	if (slept >= left_at_sleep) {
		*skip = 1;
		return now + (time_t)work_interval;
	}
	*skip = 0;
	return work_end + (time_t)slept;
}

// Advance the pomodoro cycle counter after a COMPLETED break. A LONG break
// ends the 4-cycle set (the count restarts); a SHORT one moves to the next
// cycle. Manual breaks (tray "Take a break now") count exactly like
// daemon-imposed ones.
static int se_cycle_after_break(int cycle, int was_long) {
	if (was_long) return 0;
	return (cycle + 1) & 3;
}

static int se_begin_pomodoro_day(struct SafeEyesState *st, const char *today) {
	if (strlen(today) != 10) return 0;
	if (!strcmp(st->se_pomodoro_date, today)) return 0;
	memcpy(st->se_pomodoro_date, today, 11);
	st->se_break_active = 0;
	st->se_break_type = -1;
	st->se_break_start = 0;
	st->se_work_end = 0;
	st->se_cycle = 0;
	st->se_gate_start = 0;
	st->se_work_left_at_gate = -1;
	return 1;
}

// Publish the REAL (daemon-owned) schedule for display: the vendored
// SafeEyes tray icon reads this file each refresh and shows it
// instead of its own (neutered, meaningless) scheduler times. Refreshed
// every cycle; the tray treats it as stale after 5 minutes without an
// update. next_break <= 0 means "not scheduled yet" (Start-work gate,
// disarmed); interval 0 hides the X/4 line (night).
// Written atomically (tmp file + rename) so a reader never sees a torn
// file, and the rename itself is the event the tray's Gio.FileMonitor
// refreshes on. (Overridable so the test harness can publish to a scratch
// path.)
#ifndef SE_SCHEDULE_FILE
#define SE_SCHEDULE_FILE "/tmp/cb_se_schedule"
#endif
static void se_write_schedule(const struct SafeEyesState *st,
		int want_strict, time_t now) {
	long long next_brk = 0, next_long = 0;
	int interval = (st->se_cycle & 3) + 1;
	long long work_interval =
		get_json_int("safeeyes.json", "short_break_interval", 30) * 60;
	long long short_dur =
		get_json_int("safeeyes.json", "short_break_duration", 300);
	long long long_dur =
		get_json_int("safeeyes.json", "long_break_duration", 1800);
	int c = st->se_cycle & 3;

	if (want_strict) {
		next_brk = next_long = (long long)now + secs_until_morning(now);
		interval = 0;
	} else if (st->se_break_active) {
		// A break is up: the next break comes one work interval after this
		// one ends (the Start-work gate makes it later; label-grade truth).
		long long orig = st->se_break_type == 1 ? long_dur : short_dur;
		long long end = st->se_break_start > 0
			? (long long)st->se_break_start + orig
			: (long long)now + orig;
		if (end < now) end = now;
		next_brk = end + work_interval;
		next_long = (c == 3)
			? end + 4 * work_interval           // the long one is running
			: next_brk + (3 - c) * (work_interval + short_dur);
	} else if (st->se_work_end > 0) {
		next_brk = (long long)st->se_work_end;
		next_long = (c == 3)
			? next_brk                          // the next break IS the long one
			: next_brk + (3 - c) * (work_interval + short_dur);
	}
	// else: gate/disarmed -> next_break 0 (tray shows "waiting"), interval
	// still shows where we are in the 4-cycle set.

	char tmp_path[sizeof(SE_SCHEDULE_FILE) + 8];
	snprintf(tmp_path, sizeof tmp_path, "%s.tmp", SE_SCHEDULE_FILE);
	FILE *f = fopen(tmp_path, "w");
	if (!f) return;
	fprintf(f, "night=%d\n", want_strict ? 1 : 0);
	fprintf(f, "next_break=%lld\n", next_brk);
	fprintf(f, "next_long=%lld\n", next_long);
	fprintf(f, "interval=%d\n", interval);
	fprintf(f, "total=4\n");
	if (fclose(f) != 0) {
		unlink(tmp_path);
		return;
	}
	chmod(tmp_path, 0644);
	if (rename(tmp_path, SE_SCHEDULE_FILE) != 0) {
		unlink(tmp_path);
		return;
	}
}

// Night block window: [00:00, 08:00). The start is pinned to midnight by
// NIGHT_START_HOUR (and a _Static_assert) — there is no date/hour override,
// so night mode can never be pushed later. It always begins at 00:00.
static int night_mode_active(const struct tm *tm) {
	return tm->tm_hour >= NIGHT_START_HOUR && tm->tm_hour < NIGHT_END_HOUR;
}

// CLOCK_BOOTTIME - CLOCK_MONOTONIC = cumulative seconds suspended since boot
// (BOOTTIME advances during suspend, MONOTONIC does not). Persisted each cycle
// in se_boot_mono; an increase between cycles = a suspend happened, independent
// of whether the cycle was frozen or killed. This is the robust resume detector
// -- the wall-clock se_last_cycle gap is masked when the freezer freezes the
// cycle mid-run and it completes post-resume (rewriting se_last_cycle).
static int bootmono_now(time_t *value) {
	struct timespec boot, mono;
	if (clock_gettime(CLOCK_BOOTTIME, &boot) != 0
			|| clock_gettime(CLOCK_MONOTONIC, &mono) != 0)
		return -1;
	long long d = (long long)boot.tv_sec - (long long)mono.tv_sec;
	if (d < 0) d = 0;
	*value = (time_t)d;
	return 0;
}

static int boot_id_now(char out[37]) {
	FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
	if (!f) return -1;
	if (!fgets(out, 37, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	size_t len = strlen(out);
	while (len && (out[len - 1] == '\n' || out[len - 1] == '\r'))
		out[--len] = 0;
	return len == 36 ? 0 : -1;
}

// djb2 hash of a file's contents. Used to detect when the SOURCE config
// (safeeyes.json / safeeyes-night.json in the daemon CWD) has been updated
// since the last seed, so a config edit applies without waiting for the next
// 00:00/08:00 boundary. We hash the SOURCE (stable, not rewritten by SafeEyes),
// never the live ~/.config copy (which SafeEyes normalizes each load).
static unsigned long hash_file(const char *path) {
	size_t n;
	char *buf = read_file(path, &n);
	if (!buf) return 0;
	unsigned long h = 5381;
	for (size_t i = 0; i < n; i++) h = ((h << 5) + h) + (unsigned char)buf[i];
	free(buf);
	return h;
}

// Replace the integer value following `"key":` in a JSON file (SafeEyes'
// config is machine-normalized, so the key is unique and followed by a
// bare number). Used to set short_break_duration to the exact seconds left
// until 08:00 instead of a fixed 28800. Returns 0 on success.
static int patch_json_int(const char *path, const char *key, long long val) {
	size_t n;
	char *buf = read_file(path, &n);
	if (!buf) return -1;
	char needle[128];
	int nlen = snprintf(needle, sizeof needle, "\"%s\":", key);
	char *p = strstr(buf, needle);
	if (!p) { free(buf); return -1; }
	p += nlen;
	while (*p == ' ' || *p == '\t') p++;
	char *num_start = p;
	if (*p == '-') p++;
	while (*p >= '0' && *p <= '9') p++;
	char *num_end = p;
	char valstr[32];
	int vlen = snprintf(valstr, sizeof valstr, "%lld", val);
	size_t head = (size_t)(num_start - buf);
	size_t tail = n - (size_t)(num_end - buf);
	size_t newn = head + (size_t)vlen + tail;
	char *out = malloc(newn + 1);
	if (!out) { free(buf); return -1; }
	memcpy(out, buf, head);
	memcpy(out + head, valstr, (size_t)vlen);
	memcpy(out + head + (size_t)vlen, num_end, tail);
	out[newn] = 0;
	FILE *f = fopen(path, "wb");
	if (!f) { free(buf); free(out); return -1; }
	fwrite(out, 1, newn, f);
	fclose(f);
	free(buf); free(out);
	return 0;
}

static void local_hour_for(time_t value, char out[16]) {
	struct tm tm;
	localtime_r(&value, &tm);
	strftime(out, 16, "%Y-%m-%dT%H", &tm);
}

static int calendar_sync_due(const struct SafeEyesState *st, time_t now) {
	struct tm tm;
	localtime_r(&now, &tm);
	if (tm.tm_hour < NIGHT_END_HOUR) return 0;
	char hour[16];
	local_hour_for(now, hour);
	return strcmp(st->se_cal_hour, hour) != 0;
}

static int calendar_configured(struct passwd *pw) {
	char vdir[1024], khal[1024];
	snprintf(vdir, sizeof vdir, "%s/.config/chickenblocker/vdirsyncer.conf",
		pw->pw_dir);
	snprintf(khal, sizeof khal, "%s/.config/chickenblocker/khal.conf",
		pw->pw_dir);
	return access(vdir, R_OK) == 0 && access(khal, R_OK) == 0;
}

static int calendar_sync_requested(struct passwd *pw, int consume) {
	char path[128];
	snprintf(path, sizeof path, "%s%u", CALENDAR_REQUEST_PREFIX, pw->pw_uid);
	struct stat st;
	if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)
			|| st.st_uid != pw->pw_uid || (st.st_mode & 0077) != 0)
		return 0;
	if (consume && unlink(path) != 0) return 0;
	return 1;
}

static void calendar_schedule_run(struct passwd *pw, int dry,
		struct SafeEyesState *st, struct MeetingLedger *ledger) {
	time_t now = time(NULL);
	char today[16];
	local_date_for(now, today);
	int configured = calendar_configured(pw);
	if (!configured) {
		if (!dry) se_write_state(st);
		return;
	}

	int requested = calendar_sync_requested(pw, !dry);
	if (requested || calendar_sync_due(st, now)) {
		local_hour_for(now, st->se_cal_hour);
		if (dry) {
			printf("DRY RUN: %scalendar sync for hour %s\n",
				requested ? "manually requested " : "", st->se_cal_hour);
		} else if (meeting_ledger_reconcile(pw, ledger, today) != 0) {
			fprintf(stderr, "ChickenBlocker: %scalendar sync for hour %s failed\n",
				requested ? "manually requested " : "", st->se_cal_hour);
		}
	}
	if (!dry) se_write_state(st);
}

static int meeting_is_current(const struct MeetingOccurrence *event, time_t now) {
	return event->start <= now && now < event->end;
}

static int camera_confirms_meeting(int detected, int cam) {
	return detected == 1 && cam;
}

static void meeting_resume(struct passwd *pw, int dry, struct SafeEyesState *st,
		const char *command, const char *reason) {
	printf("SAFE EYES: ending calendar pause (%s)\n", reason);
	int rc = dry ? 0 : safeeyes_cmd(pw, command);
	meeting_state_clear(st);
	if (dry || rc == 0) st->se_paused_by_us = 0;
	if (!dry) se_write_state(st);
}

static void calendar_meeting_run(struct passwd *pw, int dry, int want_strict,
		int resumed, struct SafeEyesState *st) {
	struct MeetingLedger ledger;
	meeting_ledger_read(&ledger);
	calendar_schedule_run(pw, dry, st, &ledger);
	time_t now = time(NULL);
	int ledger_dirty = 0;

	for (size_t i = 0; i < ledger.n;) {
		if (ledger.entries[i].end > now) { i++; continue; }
		ledger.entries[i] = ledger.entries[ledger.n - 1];
		ledger.n--;
		ledger_dirty = 1;
	}

	if (want_strict) {
		if (st->se_paused_by_us) {
			int rc = dry ? 0 : safeeyes_cmd(pw, "-e");
			if (rc == 0) st->se_paused_by_us = 0;
		}
		meeting_state_clear(st);
		if (!dry) {
			if (ledger_dirty) meeting_ledger_write(&ledger);
			se_write_state(st);
		}
		return;
	}

	// Migrate a pause created by the old unconditional AV behavior.
	if (st->se_meeting_phase == 0 && st->se_paused_by_us)
		meeting_resume(pw, dry, st, "-e", "legacy unscheduled AV pause");

	if (st->se_meeting_phase != 0
			&& meeting_ledger_find(&ledger, st->se_meeting_id) < 0) {
		const struct MeetingOccurrence *replacement = NULL;
		for (size_t i = 0; i < ledger.n; i++) {
			if (meeting_is_current(&ledger.entries[i], now)
					&& (!replacement || ledger.entries[i].end > replacement->end))
				replacement = &ledger.entries[i];
		}
		if (replacement) {
			snprintf(st->se_meeting_id, sizeof st->se_meeting_id, "%s",
				replacement->id);
			st->se_meeting_end = replacement->end;
		} else {
			meeting_resume(pw, dry, st, "-e", "occurrence cancelled or changed");
		}
	}

	int cancel_for_suspend = resumed && st->se_meeting_phase == 1;
	if (cancel_for_suspend) {
		for (size_t i = 0; i < ledger.n; i++) {
			if (meeting_is_current(&ledger.entries[i], now)
					&& !ledger.entries[i].attempted) {
				ledger.entries[i].attempted = 1;
				ledger_dirty = 1;
			}
		}
		meeting_resume(pw, dry, st, "--meeting-resume-suspend",
			"computer suspended before camera confirmation");
	}

	const struct MeetingOccurrence *first = NULL;
	const struct MeetingOccurrence *anchor = NULL;
	time_t overlap_end = st->se_meeting_end;
	for (size_t i = 0; i < ledger.n; i++) {
		struct MeetingOccurrence *event = &ledger.entries[i];
		if (!meeting_is_current(event, now) || event->attempted) continue;
		event->attempted = 1;
		ledger_dirty = 1;
		if (!first) first = event;
		if (event->end > overlap_end) {
			overlap_end = event->end;
			anchor = event;
		}
	}

	if (first && st->se_meeting_phase != 0) {
		st->se_meeting_end = overlap_end;
		if (anchor)
			snprintf(st->se_meeting_id, sizeof st->se_meeting_id, "%s", anchor->id);
		printf("SAFE EYES: overlapping eligible occurrence extends pause to %lld\n",
			(long long)overlap_end);
	} else if (first && !cancel_for_suspend) {
		printf("SAFE EYES: eligible occurrence %.12s; waiting %ds for camera\n",
			first->id, SE_CONFIRM_SECS);
		if (dry || safeeyes_cmd(pw, "--meeting-pause") == 0) {
			st->se_meeting_phase = 1;
			const struct MeetingOccurrence *selected = anchor ? anchor : first;
			snprintf(st->se_meeting_id, sizeof st->se_meeting_id, "%s", selected->id);
			st->se_meeting_end = overlap_end;
			st->se_confirm_deadline = now + SE_CONFIRM_SECS;
			st->se_camera_last_seen = 0;
			st->se_paused_by_us = 1;
		}
	}

	if (!dry && ledger_dirty && meeting_ledger_write(&ledger) != 0)
		fprintf(stderr, "ChickenBlocker: could not persist calendar occurrence state\n");

	if (st->se_meeting_phase == 0) {
		if (!dry) se_write_state(st);
		return;
	}
	if (now >= st->se_meeting_end) {
		meeting_resume(pw, dry, st, "-e", "calendar occurrence ended");
		return;
	}

	// Idempotent while disabled; also catches a freshly respawned Safe Eyes.
	if (!dry) (void)safeeyes_cmd(pw, "--meeting-pause");
	int cam = 0, mic = 0;
	int detected = av_active(pw, &cam, &mic);
	(void)mic; // Microphone-only activity is intentionally not confirmation.
	if (camera_confirms_meeting(detected, cam)) {
		st->se_camera_last_seen = now;
		if (st->se_meeting_phase == 1) {
			st->se_meeting_phase = 2;
			printf("SAFE EYES: camera confirmed occurrence %.12s\n", st->se_meeting_id);
		}
	}
	if (st->se_meeting_phase == 1 && now >= st->se_confirm_deadline) {
		meeting_resume(pw, dry, st, "-e", "camera confirmation timed out");
		return;
	}
	if (st->se_meeting_phase == 2 && st->se_camera_last_seen > 0
			&& now - st->se_camera_last_seen >= SE_CONFIRM_SECS) {
		meeting_resume(pw, dry, st, "-e", "camera inactive for 120 seconds");
		return;
	}
	if (!dry) se_write_state(st);
}

// Main per-cycle SafeEyes enforcement. Called from main under do_normal (so it
// is skipped during a pause-challenge / unblock window, like the browser sync).
static void safeeyes_run(struct passwd *pw, int dry, struct SafeEyesState *st) {
	time_t now = time(NULL);
	struct tm tm; localtime_r(&now, &tm);
	int want_strict = night_mode_active(&tm);
	const char *desired_src = want_strict ? "safeeyes-night.json" : "safeeyes.json";

	char se_dst[1100];
	snprintf(se_dst, sizeof se_dst, "%s/.config/safeeyes/safeeyes.json", pw->pw_dir);

	// DAEMON-OWNED TIMING: SafeEyes is a dumb overlay renderer here. Its own
	// scheduler is neutered (intervals patched to SE_NEUTERED_INTERVAL at
	// seed time below), so it NEVER fires a natural break; overlays appear
	// only via `safeeyes --impose-short/-long SECS` from this function. The
	// pomodoro clock lives in se_state (se_work_end / se_cycle) and survives
	// daemon restarts, `make update` relaunches, and SafeEyes being killed.

	// --- Resume detection (BEFORE the Start-work gate early-return) --------
	//
	// (1) boottime-monotonic delta (PRIMARY): CLOCK_BOOTTIME advances during
	//     suspend, CLOCK_MONOTONIC does not, so their difference = cumulative
	//     seconds suspended since boot. An increase since last cycle means a
	//     suspend happened. This is robust against the kernel freezer: when
	//     the cycle is frozen mid-run and completes post-resume, the
	//     wall-clock se_last_cycle gets rewritten at resume time (masking the
	//     gap below), but the boot-mono delta still reveals the suspend.
	//     Sampling must also run while the Start-work gate holds (its branch
	//     returns early): a suspend across the gate must be absorbed here, or
	//     the first cycle after Start would re-detect the same suspend and
	//     misapply the skip rule to the just-armed work interval.
	//
	// (2) kernel boot ID + wall-clock gap: detects reboot reliably even when
	//     two boots happen to have the same cumulative suspend duration.
	//
	// (3) wall-clock gap (FALLBACK): used when the boottime signal is
	//     unavailable or reset and no definitive boot-ID transition exists.
	int resumed = 0;
	long long slept = 0;   // seconds spent suspended since the last cycle
	time_t bm = 0;
	int bm_available = (bootmono_now(&bm) == 0);
	char boot_id[37] = {0};
	int boot_id_available = (boot_id_now(boot_id) == 0);
	int boot_id_needs_persist = boot_id_available
		&& strcmp(boot_id, st->se_boot_id) != 0;
	int rebooted = boot_id_available && st->se_boot_id[0]
		&& boot_id_needs_persist;
	time_t prev_bm = st->se_boot_mono;
	int prev_bm_valid = st->se_boot_mono_valid;
	long long suspend_secs = bm_available
		? (long long)(bm - prev_bm) : 0;
	if (rebooted && st->se_last_cycle != 0) {
		slept = (long long)(now - st->se_last_cycle);
		if (slept < 0) slept = 0;
		resumed = 1;
		printf("SAFEEYES: reboot detected (offline %llds)\n", slept);
	} else if (bm_available && prev_bm_valid && suspend_secs > 2) {
		resumed = 1;
		slept = suspend_secs;
		printf("SAFEEYES: resume detected (suspend %llds)\n", suspend_secs);
	}
	// Persist the new cumulative suspend counter NOW. The end-of-cycle state
	// write re-reads state from disk (a separate local), so an in-memory-only
	// update here would be lost -- and if a suspend happened, NOT persisting
	// would leave the old (pre-suspend) counter on disk, causing the NEXT
	// cycle to re-detect the same suspend forever. Persist validity
	// separately: zero is the real cumulative-suspend value before the first
	// sleep and must not be confused with an uninitialized sample.
	if (bm_available) {
		st->se_boot_mono = bm;
		st->se_boot_mono_valid = 1;
	}
	if (boot_id_available)
		snprintf(st->se_boot_id, sizeof st->se_boot_id, "%s", boot_id);
	if (!dry && ((!prev_bm_valid && bm_available)
			|| (bm_available && bm != prev_bm) || boot_id_needs_persist))
		se_write_state(st);
	int rt_fallback = (!bm_available
		|| (prev_bm_valid && bm_available && bm < prev_bm));
	if (!resumed && rt_fallback && st->se_last_cycle != 0) {
		long long rtgap = (long long)(now - st->se_last_cycle);
		if (rtgap > RESUME_THRESHOLD) {
			resumed = 1;
			slept = rtgap;
			printf("SAFEEYES: resume detected (rt gap %llds)\n", rtgap);
		}
	}

	// Calendar ownership is persisted independently of resume detection. This
	// also resets stale state when the daemon was updated/restarted after the
	// wake was already processed by an older version.
	char pomodoro_today[16];
	local_date_for(now, pomodoro_today);
	if (!dry && se_begin_pomodoro_day(st, pomodoro_today)) {
		printf("SAFEEYES: starting pomodoro day %s at cycle 1/4\n",
			pomodoro_today);
		se_write_state(st);
	}

	// --- Start-work gate ----------------------------------------------------
	// If the cb_startwork plugin is showing its "Start work" dialog, leave
	// the SafeEyes process alone (any quit/relaunch/impose here would kill
	// the process holding the dialog or fight its paused core). The daemon
	// instead FREEZES its work deadline: an idle gate that fired mid-work
	// preserves the remainder; a gate after a break ends starts fresh. On
	// Start (flag clears) the deadline is re-armed below. The independent
	// hourly calendar sync and keep-alive still run.
	int startwork_dialog_up = (access(SE_STARTWORK_FLAG, F_OK) == 0);
	if (startwork_dialog_up) {
		printf("SAFEEYES: cb_startwork dialog up; freezing work clock\n");
		if (!dry && st->se_work_end > 0) {
			// Idle gate mid-work: remember how much work time was left.
			st->se_work_left_at_gate = (long long)(st->se_work_end - now);
			if (st->se_work_left_at_gate < 0) st->se_work_left_at_gate = 0;
			st->se_work_end = 0;
			printf("SAFEEYES: gate froze work clock (%llds left)\n",
				st->se_work_left_at_gate);
		}
		if (!dry && st->se_gate_start == 0) {
			st->se_gate_start = now;
			se_write_state(st);
		}
		struct MeetingLedger ledger;
		meeting_ledger_read(&ledger);
		calendar_schedule_run(pw, dry, st, &ledger);
		if (!dry) {
			se_write_state(st);
			se_write_schedule(st, want_strict, now);
		}
		if (!safeeyes_running()) {
			printf("SAFEEYES: not running; launching\n");
			if (!dry) launch_safeeyes(pw);
		}
		return;
	}
	// The gate just released (Start pressed): re-arm the work deadline.
	if (st->se_gate_start > 0) {
		long long held = (long long)(now - st->se_gate_start);
		int fresh = (st->se_work_left_at_gate < 0
			|| held >= SE_GATE_RESET_SECS);
		long long gate_interval =
			get_json_int("safeeyes.json", "short_break_interval", 30) * 60;
		printf("SAFEEYES: start-work gate released after %llds; arming %s "
			"work interval (%llds)\n", held, fresh ? "fresh" : "remaining",
			fresh ? gate_interval : st->se_work_left_at_gate);
		if (!dry) {
			st->se_work_end = now + (fresh
				? gate_interval : st->se_work_left_at_gate);
			st->se_gate_start = 0;
			st->se_work_left_at_gate = -1;
			se_write_state(st);
		}
	}

	// Ensure the config dir exists (user-owned).
	char secfgdir[1100];
	snprintf(secfgdir, sizeof secfgdir, "%s/.config/safeeyes", pw->pw_dir);
	if (access(secfgdir, F_OK) != 0) {
		char cmd[1300];
		snprintf(cmd, sizeof cmd, "install -d -m0755 -o %u -g %u '%s'",
			pw->pw_uid, pw->pw_gid, secfgdir);
		if (!dry) system(cmd);
	}

	// Older state files stored this field as CLOCK_MONOTONIC seconds. Convert
	// it once to the wall-clock start, including any suspend since the break.
	if (st->se_break_start > 0 && st->se_break_start < 1000000000) {
		struct timespec boot;
		if (clock_gettime(CLOCK_BOOTTIME, &boot) == 0
				&& boot.tv_sec >= st->se_break_start) {
			st->se_break_start = now - (boot.tv_sec - st->se_break_start);
			printf("SAFEEYES: migrated active break start to wall clock (%lld)\n",
				(long long)st->se_break_start);
			se_write_state(st);
		}
	}

	// Scheme constants from the SOURCE day config (edit safeeyes.json to
	// change the schedule; applies within ~30s).
	long long work_interval =
		get_json_int("safeeyes.json", "short_break_interval", 30) * 60;
	long long short_dur =
		get_json_int("safeeyes.json", "short_break_duration", 300);
	long long long_dur =
		get_json_int("safeeyes.json", "long_break_duration", 1800);

	// (Re)seed the config at a mode boundary, on first run, or when the
	// SOURCE config changed. The live config's intervals are patched to
	// SE_NEUTERED_INTERVAL so SafeEyes' built-in scheduler can never fire a
	// natural break -- every overlay is imposed by the daemon with an exact
	// duration, so no quit+relaunch dance is needed to apply one and no
	// relaunch can lose work-interval progress (the clock is ours).
	unsigned long src_hash = hash_file(desired_src);
	int src_drift = (src_hash != 0 && src_hash != st->se_src_hash);
	int boundary = (want_strict != st->se_strict);
	int relaunched = 0;    // we quit+relaunched SafeEyes this cycle
	int just_imposed = 0;  // we imposed an overlay this cycle; skip re-polling
	                       // so a not-yet-rendered overlay isn't misread

	// update.sh may have quit SafeEyes itself (to load a redeployed vendored
	// package). There is nothing to absorb beyond the hash anymore -- the
	// daemon imposes whatever overlay is missing on its next action below --
	// but keep the flag cleanup so update.sh's contract holds.
	int need_neuter = !st->se_neutered;  // upgrade from a pre-neuter daemon
	int forced_by_update = (access(SE_FORCED_FLAG, F_OK) == 0);
	if (forced_by_update && !need_neuter) {
		printf("SAFEEYES: update.sh forced flag absorbed (hash %lu)\n",
			src_hash);
		st->se_src_hash = src_hash;
		st->se_strict = want_strict;
		if (want_strict) st->se_paused_by_us = 0;
		se_write_state(st);
		src_drift = 0;
		unlink(SE_FORCED_FLAG);
	} else if (forced_by_update) {
		// Legacy flag from the old update.sh, but our live config predates
		// scheduler neutering: still (re)seed below rather than absorb.
		unlink(SE_FORCED_FLAG);
	}

	if (src_drift)
		printf("SAFEEYES: source config changed (hash %lu->%lu); re-seeding\n",
			st->se_src_hash, src_hash);

	if (src_drift || boundary || need_neuter) {
		printf("SAFEEYES: %s config (mode %s->%s%s)\n",
			st->se_strict < 0 ? "initial seed" : "boundary swap",
			st->se_strict < 0 ? "?" : (st->se_strict ? "night" : "day"),
			want_strict ? "night" : "day",
			need_neuter ? " + scheduler neutering" : "");
		if (!dry) {
			copy_file_user(desired_src, se_dst, pw);
			patch_json_int(se_dst, "short_break_interval", SE_NEUTERED_INTERVAL);
			patch_json_int(se_dst, "long_break_interval", SE_NEUTERED_INTERVAL);
			st->se_neutered = 1;
			if (safeeyes_running()) {
				safeeyes_quit(pw, dry);
				relaunched = 1;
			}
		}
		st->se_strict = want_strict;
		st->se_src_hash = src_hash;
		if (want_strict) {
			// Entering night: any day pomodoro bookkeeping is void.
			st->se_paused_by_us = 0;
			st->se_break_active = 0;
			st->se_break_type = -1;
			st->se_break_start = 0;
			st->se_work_end = 0;
			st->se_gate_start = 0;
			st->se_work_left_at_gate = -1;
			st->se_cycle = 0;
		}
		se_write_state(st);
	}

	// Keep-alive / initial launch.
	int se_was_dead = !safeeyes_running();
	if (se_was_dead) {
		printf("SAFEEYES: not running; launching\n");
		if (!dry) launch_safeeyes(pw);
	}

	if (want_strict) {
		// --- NIGHT: one continuous block until 08:00 -----------------------
		// The daemon owns the duration: impose secs-until-morning explicitly
		// whenever the overlay is not up (first launch, resume from suspend,
		// a killed/lost overlay). No config duration patching, no
		// quit+relaunch on resume -- the impose carries the exact remainder.
		calendar_meeting_run(pw, dry, want_strict, resumed, st);
		if (dry) {
			printf("DRY RUN: night; would ensure overlay (%llds to 08:00)\n",
				secs_until_morning(now));
			return;
		}
		se_write_schedule(st, 1, now);
		if (relaunched || !safeeyes_running()) {
			// Fresh instance (or none): no overlay possible; impose directly.
			long long left = secs_until_morning(now);
			if (safeeyes_impose(pw, dry, 1, left) == 0)
				just_imposed = 1;
			return;
		}
		int nbs = break_status(pw, dry);
		if (nbs == 1 || nbs == 2 || nbs == 3)
			return;  // overlay already up
		if (nbs == -1)
			return;  // can't tell (X down?); retry next cycle
		long long left = secs_until_morning(now);
		printf("SAFEEYES: night overlay down; re-imposing (%llds to 08:00)\n",
			left);
		if (safeeyes_impose(pw, dry, 1, left) == 0)
			just_imposed = 1;
		return;
	}

	// --- DAY ----------------------------------------------------------------
	// Restore a break that was interrupted, so it cannot be escaped:
	//   * suspended mid-break -- the sleep counts toward the break; the
	//     core's wall-clock countdown also ends a surviving overlay by
	//     itself, but if the overlay was closed (or SE quit) we re-impose;
	//   * SafeEyes died or we relaunched it ourselves (killed process, seed
	//     relaunch on drift/boundary) while a break was up.
	// In all cases re-impose the time LEFT from the original wall-clock
	// deadline. If the interruption outlasted the break, it is satisfied:
	// advance the cycle and let the work clock re-arm below (the plugin
	// skips its Start dialog after a suspend, so no gate).
	if (!dry && st->se_break_active && (resumed || se_was_dead || relaunched)) {
		long long orig_dur = st->se_break_type == 1 ? long_dur : short_dur;
		long long elapsed = st->se_break_start > 0
			? (long long)(now - st->se_break_start) : orig_dur;
		if (elapsed < 0) elapsed = 0;
		long long time_left = orig_dur - elapsed;
		printf("SAFEEYES: day resume; break was active (type %s, orig %llds, "
			"elapsed %llds, left %llds)\n",
			st->se_break_type == 1 ? "long" : "short",
			orig_dur, elapsed, time_left);
		if (time_left > 0) {
			// No relaunch needed: impose the remainder over the (frozen or
			// closed) overlay. Keep the ORIGINAL break_start so a second
			// suspend during this break recomputes from the true deadline.
			if (safeeyes_impose(pw, dry, st->se_break_type == 1,
					time_left) == 0)
				just_imposed = 1;
			else
				printf("SAFEEYES: re-impose failed; retrying next cycle\n");
		} else {
			printf("SAFEEYES: break already elapsed; satisfied by sleep\n");
			int was_long = (st->se_break_type == 1);
			st->se_break_active = 0;
			st->se_break_type = -1;
			st->se_break_start = 0;
			st->se_cycle = se_cycle_after_break(st->se_cycle, was_long);
			se_write_state(st);
		}
	}

	// Resume during a WORK interval: suspend time is NOT work time. A short
	// sleep PAUSES the countdown (the deadline is pushed by the sleep
	// duration -- the remaining work is fully owed after wake). A sleep
	// LONGER than the work time that was left when the machine went down
	// ignores the pending break entirely: the cycle advances and a fresh
	// interval starts at wake (you rested longer than the break was worth).
	if (!dry && resumed && !st->se_break_active && st->se_work_end > 0) {
		int skip_pending = 0;
		time_t new_end = se_work_resume_adjust(st->se_work_end, now,
			slept, work_interval, &skip_pending);
		if (skip_pending) {
			printf("SAFEEYES: suspend %llds was longer than the work time "
				"left; skipping the pending %s break\n", slept,
				(st->se_cycle & 3) == 3 ? "long" : "short");
			st->se_cycle = (st->se_cycle + 1) & 3;
		} else {
			printf("SAFEEYES: suspend %llds during work; deadline pushed "
				"(work resumes where it left off)\n", slept);
		}
		st->se_work_end = new_end;
		se_write_state(st);
	}

	// Overlay polling (skip when we just relaunched or imposed: Wnck can
	// briefly miss a newly-created overlay, and a fresh instance has none).
	int bs = -1;
	if (!dry && safeeyes_running() && !relaunched && !just_imposed)
		bs = break_status(pw, dry);

	// Track overlay transitions so the cycle counter advances when a break
	// completes. On a 0->1 transition record the wall-clock start + type
	// (preferring the live classification, falling back to the type we
	// imposed) so a suspend mid-break can recompute the time left.
	int break_ended_this_cycle = 0;
	if (!dry && safeeyes_running() && !relaunched && !just_imposed) {
		if (bs == 1 || bs == 2 || bs == 3) {  // a break is up
			if (!st->se_break_active) {  // 0->1 transition: record start+type
				st->se_break_start = now;
				if (bs == 2) st->se_break_type = 1;
				else if (bs == 1) st->se_break_type = 0;
				else if (st->se_break_type < 0) st->se_break_type = 0;
				st->se_break_active = 1;
				// A break we did NOT impose this cycle (the tray's manual
				// "Take a break now") replaces the current work phase, just
				// like an imposed one: breaks bracket work intervals in the
				// scheme, so its end + Start gate leads into a FRESH
				// interval rather than a stale remainder. It still counts
				// as a taken break (se_cycle advances on its end below).
				if (st->se_work_end > 0) {
					printf("SAFEEYES: break not imposed by us (manual?); "
						"disarming work clock\n");
					st->se_work_end = 0;
				}
				printf("SAFEEYES: break started (type %s) at %lld\n",
					st->se_break_type == 1 ? "long" : "short",
					(long long)now);
				se_write_state(st);
			}
		} else if (bs == 0) {  // IDLE
			if (st->se_break_active) {
				int was_long = (st->se_break_type == 1);
				st->se_break_active = 0;
				st->se_break_type = -1;
				st->se_break_start = 0;
				st->se_cycle = se_cycle_after_break(st->se_cycle, was_long);
				break_ended_this_cycle = 1;
				printf("SAFEEYES: break ended; %d cycle(s) until long\n",
					3 - (st->se_cycle & 3));
				se_write_state(st);
				// Do NOT arm work here: the cb_startwork Start gate is about
				// to appear (within ~1s). The gate branch freezes/arms on
				// release; if the plugin fails to show it, the next cycle
				// arms a fresh interval below.
			}
		}
	}

	// A calendar occurrence may interrupt an active daytime break, but
	// calendar synchronization itself starts only from a normal work interval.
	calendar_meeting_run(pw, dry, want_strict, resumed, st);

	// While a meeting pause (or a legacy AV pause) holds SafeEyes disabled,
	// the work clock is disarmed; when the pause ends, a FRESH interval is
	// armed below (matching the old disable/enable reset behavior).
	int meeting_hold = (st->se_meeting_phase != 0 || st->se_paused_by_us);
	if (meeting_hold && st->se_work_end > 0) {
		printf("SAFEEYES: meeting pause; disarming work clock\n");
		st->se_work_end = 0;
		st->se_work_left_at_gate = -1;
		se_write_state(st);
	}

	// Work-interval decisions.
	if (!dry && !st->se_break_active && !meeting_hold
			&& !startwork_dialog_up && !relaunched && !just_imposed
			&& !break_ended_this_cycle) {
		if (st->se_work_end == 0) {
			// Disarmed with no gate, no overlay, no meeting: arm a FRESH
			// interval. Covers: first run, daemon restart with lost fields,
			// a break that ended without a Start gate (suspend-skip path),
			// and the end of a meeting pause.
			st->se_work_end = now + work_interval;
			printf("SAFEEYES: arming fresh work interval (%llds, %d cycle(s) "
				"until long)\n", work_interval, 3 - (st->se_cycle & 3));
			se_write_state(st);
		} else if (now >= st->se_work_end) {
			// Work interval over: impose the due break (type by cycle
			// count: every 4th is the long one). Suspend handling happens
			// in the resume blocks above (skip/shift), so a deadline that
			// is past while awake means the break is simply due.
			int is_long = (st->se_cycle & 3) == 3;
			long long dur = is_long ? long_dur : short_dur;
			printf("SAFEEYES: work interval over; imposing %s break "
				"(%llds)\n", is_long ? "long" : "short", dur);
			if (safeeyes_impose(pw, dry, is_long, dur) == 0) {
				st->se_break_active = 1;  // optimistic: suspend within
				                          // ~30s still recomputes left
				st->se_break_type = is_long;
				st->se_break_start = now;
				st->se_work_end = 0;
				just_imposed = 1;
				se_write_state(st);
			}
		}
	}

	// Publish the real schedule for the tray (see se_write_schedule).
	if (!dry) se_write_schedule(st, want_strict, now);
}

// --- pause / math challenge ------------------------------------------------
// The user runs /opt/ChickenBlocker/cb-pause in a terminal to request a temporary
// unblock. It is a single-use client: each invocation shows ONE math question
// (with a live countdown), reads the answer, sends it to the daemon over an
// abstract unix socket, prints correct/wrong, then exits; the daemon deletes
// and recreates the binary so the user must run it again for the next question
// (forgetting to re-run within the window => challenge restarts). After 5
// correct, the daemon swaps in the *-unblocked.json policies for 5 minutes,
// then restores blocking and re-checks LeechBlock block sets.
//
// Anti-cheat: the daemon only trusts a connection whose peer, via SO_PEERCRED,
// has uid == the logged-in user AND whose /proc/<pid>/exe is the genuine
// /opt/ChickenBlocker/cb-pause binary AND whose on-disk hash matches the stored
// digest (/var/lib/ChickenBlocker/cb_pause_hash). A ruby/python solver or a copied
// binary fails this check. The expected answer + daily count live in a
// root-only state file the user cannot read.
//
//   phase 0 = idle, 1 = challenge, 2 = unblocked window
//
// Any failed attempt (wrong answer, timeout, abandoned question) drops to
// phase 0 and sets a COOLDOWN_SECS lockout; HELLO while the lockout is active
// returns COOLDOWN <secs> instead of starting a new challenge.
//
// Env CHICKENBLOCKER_DRY_RUN=1 prints actions, performs no writes/kills/launches.

#define STATE_DIR        "/var/lib/ChickenBlocker"
#define STATE_FILE       "/var/lib/ChickenBlocker/state"
#define CB_PAUSE_HASH    "/var/lib/ChickenBlocker/cb_pause_hash"
#define CB_PAUSE_BIN     "/opt/ChickenBlocker/cb-pause"
#define CB_PAUSE_TMPL    "/opt/ChickenBlocker/.cb-pause.tmpl"
#define PAUSE_SOCK_NAME  "ChickenBlocker-pause"      // abstract unix socket
#define MAX_PAUSES_DAY   5
#define QUESTIONS_NEEDED 5
#define QUESTION_SECS    60
#define NEXT_Q_DELAY_SECS 60   // after a correct answer, the next question is
                               // NOT openable for this long; HELLO before then
                               // returns WAIT <secs> instead of a question
#define OPEN_WINDOW_SECS  60   // window (starting NEXT_Q_DELAY_SECS after a
                               // correct answer) during which the user must open
                               // cb-pause for the next question
#define GRACE_SECS       10     // extra time the daemon accepts (poll latency)
#define UNBLOCK_SECS     300
#define COOLDOWN_SECS    300    // lockout after any failed cb-pause attempt
#define CYCLE_BUDGET     28     // seconds between enforcement passes
#define NUM_SETS         6
#define LB_FF_ID         "leechblockng@proginosko.com"
#define OPND_MAX         99   // math operands are 0..OPND_MAX (2 digits)

// Vivaldi restart cooldown. A persistent LeechBlock "disabled" detection or a
// Firefox->Vivaldi rule drift could otherwise kill+relaunch Vivaldi every
// ~28s cycle, making the browser unusable. After any Vivaldi restart we touch
// this file and refuse another restart (for either reason) until it expires.
#define VV_RESTART_TS      "/var/lib/ChickenBlocker/vv_restart_ts"
#define VV_RESTART_COOLDOWN 300

struct PauseState {
	int phase;
	int count;
	char date[16];
	int qnum;
	char nonce[64];
	char qtext[128];
	double expected;
	time_t qstart;       // when the current question became available (open
	                    // window starts here, = previous guess end / challenge
	                    // start); user must open cb-pause within OPEN_WINDOW_SECS
	time_t open_time;    // when the user first opened cb-pause for this question
	                    // (0 until then); the answer deadline = open_time+QUESTION_SECS
	time_t unblock_end;
	time_t cooldown_end;
	char grantmsg[160];
	int ff_active[NUM_SETS];   // -1 unknown, else 0/1 snapshot before unlock
};

static void today_str(char *out, size_t n) {
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, n, "%Y-%m-%d", &tm);
}

static int read_state(struct PauseState *s) {
	memset(s, 0, sizeof *s);
	for (int i = 0; i < NUM_SETS; i++) s->ff_active[i] = -1;
	FILE *f = fopen(STATE_FILE, "r");
	if (!f) return -1;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *v = eq + 1;
		size_t l = strlen(v);
		while (l && (v[l - 1] == '\n' || v[l - 1] == '\r')) v[--l] = 0;
		if (!strcmp(line, "phase")) s->phase = atoi(v);
		else if (!strcmp(line, "count")) s->count = atoi(v);
		else if (!strcmp(line, "date")) snprintf(s->date, sizeof s->date, "%s", v);
		else if (!strcmp(line, "qnum")) s->qnum = atoi(v);
		else if (!strcmp(line, "nonce")) snprintf(s->nonce, sizeof s->nonce, "%s", v);
		else if (!strcmp(line, "qtext")) snprintf(s->qtext, sizeof s->qtext, "%s", v);
		else if (!strcmp(line, "expected")) s->expected = atof(v);
		else if (!strcmp(line, "qstart")) s->qstart = (time_t)strtoll(v, NULL, 10);
		else if (!strcmp(line, "open_time")) s->open_time = (time_t)strtoll(v, NULL, 10);
		else if (!strcmp(line, "unblock_end")) s->unblock_end = (time_t)strtoll(v, NULL, 10);
		else if (!strcmp(line, "cooldown_end")) s->cooldown_end = (time_t)strtoll(v, NULL, 10);
		else if (!strcmp(line, "grantmsg")) snprintf(s->grantmsg, sizeof s->grantmsg, "%s", v);
		else if (!strcmp(line, "ff_active")) {
			int idx = 0; char *p = v, *t;
			while (p && idx < NUM_SETS) { t = strchr(p, ','); if (t) *t = 0; s->ff_active[idx++] = atoi(p); p = t ? t + 1 : NULL; }
		}
	}
	fclose(f);
	return 0;
}

static void write_state(const struct PauseState *s) {
	mkdir(STATE_DIR, 0700);
	FILE *f = fopen(STATE_FILE, "w");
	if (!f) return;
	chmod(STATE_FILE, 0600);
	fprintf(f, "phase=%d\n", s->phase);
	fprintf(f, "count=%d\n", s->count);
	fprintf(f, "date=%s\n", s->date);
	fprintf(f, "qnum=%d\n", s->qnum);
	fprintf(f, "nonce=%s\n", s->nonce);
	fprintf(f, "qtext=%s\n", s->qtext);
	fprintf(f, "expected=%.6f\n", s->expected);
	fprintf(f, "qstart=%lld\n", (long long)s->qstart);
	fprintf(f, "open_time=%lld\n", (long long)s->open_time);
	fprintf(f, "unblock_end=%lld\n", (long long)s->unblock_end);
	fprintf(f, "cooldown_end=%lld\n", (long long)s->cooldown_end);
	fprintf(f, "grantmsg=%s\n", s->grantmsg);
	fprintf(f, "ff_active=");
	for (int i = 0; i < NUM_SETS; i++) fprintf(f, "%s%d", i ? "," : "", s->ff_active[i]);
	fprintf(f, "\n");
	fclose(f);
}

static void gen_question(char *qtext, size_t n, double *expected) {
	int op = rand() % 4;
	int a, b;
	if (op == 3) {
		if (rand() % 2 == 0) {
			// exact integer division: pick divisor b in 1..OPND_MAX, then
			// dividend a as a multiple of b with a <= OPND_MAX (whole answer).
			b = 1 + (rand() % OPND_MAX);
			int qmax = OPND_MAX / b;
			int q = (qmax > 0) ? (rand() % (qmax + 1)) : 0;
			a = q * b;
		} else {
			// "nice" terminating fraction: denominator 2, 4 or 8, so the result
			// is a multiple of 1/8 (.125 steps) with at most 3 decimals and no
			// irregular/non-terminating fractions (e.g. 7/8=.875, 5/4=1.25,
			// 3/8=.375). a may be a multiple of b -> still valid (integer).
			int small[] = {2, 4, 8};
			b = small[rand() % 3];
			a = rand() % (OPND_MAX + 1);
		}
	} else {
		a = rand() % (OPND_MAX + 1);
		b = rand() % (OPND_MAX + 1);
	}
	char opc;
	double r;
	switch (op) {
	case 0: opc = '+'; r = (double)a + b; break;
	case 1: opc = '-'; r = (double)a - b; break;
	case 2: opc = '*'; r = (double)a * b; break;
	default: opc = '/'; r = (double)a / (double)b; break;
	}
	snprintf(qtext, n, "%d%c%d = ?", a, opc, b);
	*expected = r;
}

// Round to 2 decimals as a comparable integer (handles . and , answers).
static long long round2ll(double x) {
	double y = x * 100.0;
	long long r = (long long)y;
	double frac = y - (double)r;
	if (frac >= 0.5) r++;
	else if (frac <= -0.5) r--;
	return r;
}

// Extract the LAST numeric token from buf after removing the known question
// text. Decimal separator may be '.' or ','. Returns 1 and sets *out, else 0.
static int parse_answer(const char *buf, const char *qtext, double *out) {
	size_t ql = strlen(qtext);
	char *tmp = malloc(strlen(buf) + 1);
	size_t tl = 0;
	const char *p = buf;
	while (*p) {
		if (ql && strncmp(p, qtext, ql) == 0) p += ql;
		else tmp[tl++] = *p++;
	}
	tmp[tl] = 0;
	double last = 0;
	int found = 0;
	for (char *q = tmp; *q; q++) {
		char *ns = NULL;
		if (isdigit((unsigned char)*q)) ns = q;
		else if (*q == '-' && isdigit((unsigned char)q[1])) ns = q;
		if (!ns) continue;
		char *e = ns;
		if (*e == '-') e++;
		while (isdigit((unsigned char)*e)) e++;
		if (*e == '.' || *e == ',') { e++; while (isdigit((unsigned char)*e)) e++; }
		char nb[80]; size_t ll = 0;
		for (char *c = ns; c < e && ll < sizeof nb - 1; c++) nb[ll++] = (*c == ',') ? '.' : *c;
		nb[ll] = 0;
		char *endp;
		double val = strtod(nb, &endp);
		if (endp == nb + ll) { last = val; found = 1; }
		q = e - 1;   // loop's q++ advances past token
	}
	free(tmp);
	if (found) *out = last;
	return found;
}

// Read LeechBlock activeBlock1..6 from Firefox's storage-sync DB via the python
// helper. Returns 0 on success (fills out), -1 on failure.
static int firefox_activeblock(const char *sqlite_path, int out[NUM_SETS]) {
	char cmd[2048];
	snprintf(cmd, sizeof cmd, "/usr/bin/python3 /opt/ChickenBlocker/cb_ff_activeblock.py '%s'",
		sqlite_path);
	FILE *p = popen(cmd, "r");
	if (!p) return -1;
	char line[256];
	int got = (fgets(line, sizeof line, p) != NULL);
	pclose(p);
	if (!got) return -1;
	int idx = 0; char *t = line, *c;
	while (t && idx < NUM_SETS) { c = strchr(t, ','); if (c) *c = 0; out[idx++] = atoi(t); t = c ? c + 1 : NULL; }
	return 0;
}

static int firefox_set_activeblock(const char *sqlite_path, const int vals[NUM_SETS]) {
	char csv[64]; size_t p2 = 0;
	for (int i = 0; i < NUM_SETS; i++) p2 += snprintf(csv + p2, sizeof csv - p2, "%s%d", i ? "," : "", vals[i]);
	char cmd[2200];
	snprintf(cmd, sizeof cmd, "/usr/bin/python3 /opt/ChickenBlocker/cb_ff_activeblock.py '%s' %s",
		sqlite_path, csv);
	return system(cmd);
}

// Read which LeechBlock block sets are non-empty (sites{n} non-empty) from
// Firefox's storage-sync DB via the python helper. Returns 0 on success.
static int firefox_nonempty(const char *sqlite_path, int out[NUM_SETS]) {
	char cmd[2048];
	snprintf(cmd, sizeof cmd, "/usr/bin/python3 /opt/ChickenBlocker/cb_ff_activeblock.py '%s' --nonempty",
		sqlite_path);
	FILE *p = popen(cmd, "r");
	if (!p) return -1;
	char line[256];
	int got = (fgets(line, sizeof line, p) != NULL);
	pclose(p);
	if (!got) return -1;
	int idx = 0; char *t = line, *c;
	while (t && idx < NUM_SETS) { c = strchr(t, ','); if (c) *c = 0; out[idx++] = atoi(t); t = c ? c + 1 : NULL; }
	return 0;
}

// Mirror LeechBlock config from Firefox's storage-sync DB into Vivaldi's
// chrome.storage.local LevelDB via the python helper. Vivaldi MUST be stopped
// (LevelDB write lock). Returns 0 on success. Prints the helper's SYNCED/
// CHANGED <n> line on stdout.
static int vivaldi_sync_from_firefox(const char *vv_leveldb, const char *ff_sqlite) {
	char cmd[3000];
	snprintf(cmd, sizeof cmd, "/usr/bin/python3 /opt/ChickenBlocker/cb_vv_leechblock.py '%s' --sync-from '%s'",
		vv_leveldb, ff_sqlite);
	FILE *p = popen(cmd, "r");
	if (!p) return -1;
	char line[128];
	if (fgets(line, sizeof line, p)) {
		char *nl = strchr(line, '\n'); if (nl) *nl = 0;
		printf("VIVALDI LEECHBLOCK SYNC: %s\n", line);
	} else {
		printf("VIVALDI LEECHBLOCK SYNC: FAILED (no output)\n");
	}
	int rc = pclose(p);
	if (rc != 0) printf("VIVALDI LEECHBLOCK SYNC: helper exit %d\n", rc);
	return rc;
}

// Compare LeechBlock config (Firefox storage-sync DB vs Vivaldi LevelDB) WITHOUT
// taking Vivaldi's write lock: the python helper opens the LevelDB read-only, so
// this is safe while Vivaldi is running. Returns the number of drifted config
// keys, or -1 on error. Used to decide whether a disruptive kill+sync+relaunch
// is actually needed to propagate rule edits from Firefox to Vivaldi.
static int vivaldi_diff_firefox(const char *vv_leveldb, const char *ff_sqlite) {
	char cmd[3000];
	snprintf(cmd, sizeof cmd, "/usr/bin/python3 /opt/ChickenBlocker/cb_vv_leechblock.py '%s' --diff '%s'",
		vv_leveldb, ff_sqlite);
	FILE *p = popen(cmd, "r");
	if (!p) return -1;
	char line[128];
	int got = (fgets(line, sizeof line, p) != NULL);
	int rc = pclose(p);
	if (!got || rc != 0) return -1;
	char *nl = strchr(line, '\n'); if (nl) *nl = 0;
	if (!strncmp(line, "DRIFT ", 6)) return atoi(line + 6);
	if (!strcmp(line, "OK")) return 0;
	return -1;
}

// --- pause client program helpers (socket + binary integrity) --------------

static void gen_nonce(char *out, size_t n) {
	static const char a[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	for (size_t i = 0; i < n - 1; i++) out[i] = a[rand() % (sizeof a - 1)];
	out[n - 1] = 0;
}

static int sendf(int s, const char *fmt, ...) {
	char buf[1200];
	va_list ap; va_start(ap, fmt);
	int l = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (l < 0) return -1;
	if (l >= (int)sizeof buf) l = sizeof buf - 1;
	if (buf[l - 1] != '\n') { if (l < (int)sizeof buf) { buf[l] = '\n'; l++; } }
	size_t off = 0;
	while (off < (size_t)l) { ssize_t w = write(s, buf + off, l - off); if (w <= 0) return -1; off += w; }
	return 0;
}

static int recv_line(int s, char *buf, size_t n, int timeout_s) {
	size_t got = 0; time_t start = time(NULL);
	while (got < n - 1) {
		fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
		long left = timeout_s - (time(NULL) - start); if (left < 1) left = 1;
		struct timeval tv = { left, 0 };
		int r = select(s + 1, &rf, NULL, NULL, &tv);
		if (r <= 0) return -1;
		ssize_t rd = read(s, buf + got, n - 1 - got);
		if (rd <= 0) return -1;
		got += rd; buf[got] = 0;
		if (memchr(buf, '\n', got)) break;
	}
	buf[got] = 0;
	char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
	return 0;
}

static int pause_socket_setup(void) {
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) return -1;
	struct sockaddr_un a; memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX; a.sun_path[0] = '\0';
	size_t nl = strlen(PAUSE_SOCK_NAME);
	memcpy(a.sun_path + 1, PAUSE_SOCK_NAME, nl);
	socklen_t alen = offsetof(struct sockaddr_un, sun_path) + 1 + nl;
	if (bind(s, (struct sockaddr *)&a, alen) < 0) { close(s); return -1; }
	if (listen(s, 8) < 0) { close(s); return -1; }
	return s;
}

// Verify the peer is the logged-in user running the genuine, untampered
// /opt/ChickenBlocker/cb-pause binary (SO_PEERCRED uid + /proc/pid/exe path +
// sha256 of the running binary vs the stored digest). Returns 0 = trusted.
static int pause_verify_peer(int c, struct passwd *pw) {
	struct ucred cr; socklen_t cl = sizeof cr;
	if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) != 0) {
		printf("PAUSE reject: no peer creds\n"); return -1;
	}
	if (cr.uid != pw->pw_uid) {
		printf("PAUSE reject: uid %u != %u\n", cr.uid, pw->pw_uid); return -1;
	}
	char proc[64]; snprintf(proc, sizeof proc, "/proc/%d/exe", cr.pid);
	char exe[512]; ssize_t n = readlink(proc, exe, sizeof exe - 1);
	if (n < 0) { printf("PAUSE reject: cannot readlink %s\n", proc); return -1; }
	exe[n] = 0;
	if (strcmp(exe, CB_PAUSE_BIN) != 0) {
		printf("PAUSE reject: exe '%s' != '%s'\n", exe, CB_PAUSE_BIN); return -1;
	}
	char cmd[80]; snprintf(cmd, sizeof cmd, "sha256sum /proc/%d/exe", cr.pid);
	FILE *p = popen(cmd, "r");
	if (!p) { printf("PAUSE reject: cannot sha256sum\n"); return -1; }
	char h[80]; int got = (fgets(h, sizeof h, p) != NULL); pclose(p);
	if (!got) { printf("PAUSE reject: sha256sum produced no output\n"); return -1; }
	char *sp = strchr(h, ' '); if (sp) *sp = 0;
	size_t gl; char *good = read_file(CB_PAUSE_HASH, &gl);
	if (!good) { printf("PAUSE reject: no hash file %s\n", CB_PAUSE_HASH); return -1; }
	while (gl && (good[gl - 1] == '\n' || good[gl - 1] == '\r' || good[gl - 1] == ' ')) good[--gl] = 0;
	int ok = (strlen(h) == gl && memcmp(h, good, gl) == 0);
	if (!ok) printf("PAUSE reject: hash mismatch (got %.16s..., want %.16s...)\n", h, good);
	free(good);
	return ok ? 0 : -1;
}

static void pause_recreate_binary(void) {
	if (access(CB_PAUSE_BIN, F_OK) == 0) return;
	if (copy_file(CB_PAUSE_TMPL, CB_PAUSE_BIN) == 0) chmod(CB_PAUSE_BIN, 0755);
}

static void pause_delete_recreate_binary(void) {
	unlink(CB_PAUSE_BIN);
	if (copy_file(CB_PAUSE_TMPL, CB_PAUSE_BIN) == 0) chmod(CB_PAUSE_BIN, 0755);
}

static void swap_unblocked(struct passwd *pw) {
	const char *pp = "/etc/firefox/policies/policies.json";
	if (!files_equal(pp, "policies-unblocked.json")) {
		copy_file("policies-unblocked.json", pp);
		if (firefox_running()) {
			if (term_and_wait("firefox", 10) == 0) launch_firefox(pw);
			else printf("firefox kill failed (AppArmor?); policies applied, not relaunching\n");
		}
	}
	if (access("/usr/bin/vivaldi-stable", F_OK) == 0) {
		const char *vp = "/etc/vivaldi/policies/managed/ChickenBlocker.json";
		if (!files_equal(vp, "vivaldi-policies-unblocked.json")) {
			copy_file("vivaldi-policies-unblocked.json", vp);
			if (vivaldi_running()) {
				if (term_and_wait("vivaldi-bin", 10) == 0) launch_vivaldi(pw);
				else printf("vivaldi kill failed; policies applied, not relaunching\n");
			}
		}
	}
}

static void swap_blocking(struct passwd *pw, const char *ff_sync_sqlite,
	const char *vv_leveldb, struct PauseState *st) {
	int was_ff = firefox_running();
	int ff_killed = 1;
	if (was_ff) ff_killed = (term_and_wait("firefox", 10) == 0);
	copy_file("policies.json", "/etc/firefox/policies/policies.json");
	int nonempty[NUM_SETS];
	for (int i = 0; i < NUM_SETS; i++) nonempty[i] = 1;   // default: treat all as non-empty
	firefox_nonempty(ff_sync_sqlite, nonempty);   // best-effort; on failure keeps the safe default
	int now_a[NUM_SETS];
	if (firefox_activeblock(ff_sync_sqlite, now_a) == 0) {
		int target[NUM_SETS], changed = 0, have = 0;
		for (int i = 0; i < NUM_SETS; i++) if (st->ff_active[i] == 1 && nonempty[i]) have = 1;
		for (int i = 0; i < NUM_SETS; i++) {
			target[i] = (st->ff_active[i] == 1 && nonempty[i]) ? 1 : now_a[i];
			if (st->ff_active[i] == 1 && nonempty[i] && now_a[i] == 0) changed = 1;
		}
		if (have && changed) {
			printf("PAUSE: a non-empty block set was disabled; re-enabling\n");
			firefox_set_activeblock(ff_sync_sqlite, target);
		}
	}
	// Vivaldi: kill, restore blocking policy, mirror the (now-corrected) Firefox
	// LeechBlock config - this re-enables Vivaldi's non-empty block sets too.
	// Done before relaunching Firefox so the helper can read Firefox's sqlite
	// (which is stopped here) without a lock conflict.
	int have_vv = (access("/usr/bin/vivaldi-stable", F_OK) == 0);
	int was_vv = 0;
	int vv_killed = 1;
	if (have_vv) {
		was_vv = vivaldi_running();
		if (was_vv) vv_killed = (term_and_wait("vivaldi-bin", 10) == 0);
		copy_file("vivaldi-policies.json", "/etc/vivaldi/policies/managed/ChickenBlocker.json");
		if (vv_leveldb) {
			if (vivaldi_sync_from_firefox(vv_leveldb, ff_sync_sqlite) != 0)
				printf("PAUSE: Vivaldi LeechBlock sync FAILED; rules may be stale\n");
		}
	}
	if (was_ff) {
		if (ff_killed) launch_firefox(pw);
		else printf("firefox kill failed (AppArmor?); blocking policies applied, not relaunching\n");
	}
	if (have_vv && was_vv) {
		if (vv_killed) launch_vivaldi(pw);
		else printf("vivaldi kill failed; blocking policies applied, not relaunching\n");
	}
}

static int pause_handle_conn(int c, struct passwd *pw, const char *ff_sync_sqlite,
	int dry, struct PauseState *st) {
	if (pause_verify_peer(c, pw) != 0) { printf("PAUSE: rejected unverified peer\n"); return 0; }
	char line[1024];
	if (recv_line(c, line, sizeof line, 5) < 0) return 0;
	int dirty = 0;
	if (!strncmp(line, "HELLO", 5)) {
		if (st->phase == 0) {
			time_t now0 = time(NULL);
			if (now0 < st->cooldown_end) {
				sendf(c, "COOLDOWN %lld", (long long)(st->cooldown_end - now0));
			} else if (st->count < MAX_PAUSES_DAY) {
				// NOTE: do NOT count the attempt here - only a fully solved
				// challenge (5 correct -> grant) consumes a daily slot, so a
				// wrong/abandoned attempt costs nothing.
				st->cooldown_end = 0;
				st->phase = 1; st->qnum = 0;
				gen_question(st->qtext, sizeof st->qtext, &st->expected);
				gen_nonce(st->nonce, sizeof st->nonce);
				time_t now1 = time(NULL);
				st->qstart = now1;          // open window starts now
				st->open_time = now1;       // user is opening right now
				printf("PAUSE: start challenge (grants used %d/%d) Q1=%s\n", st->count, MAX_PAUSES_DAY, st->qtext);
				sendf(c, "QUESTION %d %s %lld %s", st->qnum + 1, st->nonce,
					(long long)(st->open_time + QUESTION_SECS), st->qtext);
				dirty = 1;
			} else sendf(c, "LIMIT");
		} else if (st->phase == 1) {
			time_t now = time(NULL);
			if (st->open_time == 0) {
				// Not opened yet for this question. After a correct answer the
				// open window only starts NEXT_Q_DELAY_SECS later (qstart is in
				// the future), so a too-early HELLO returns WAIT <secs> instead
				// of a question; the user must re-open after the delay.
				if (now < st->qstart) {
					sendf(c, "WAIT %lld", (long long)(st->qstart - now));
				} else if (now - st->qstart > OPEN_WINDOW_SECS) {
					// The open window (qstart..qstart+OPEN_WINDOW_SECS) has
					// passed without the user opening cb-pause -> cooldown.
					printf("PAUSE: open window missed; cooldown\n");
					st->phase = 0;
					st->cooldown_end = now + COOLDOWN_SECS;
					dirty = 1;
					sendf(c, "COOLDOWN %lld", (long long)COOLDOWN_SECS);
				} else {
					// First open for this question: anchor a fresh full minute.
					st->open_time = now;
					dirty = 1;
					sendf(c, "QUESTION %d %s %lld %s", st->qnum + 1, st->nonce,
						(long long)(st->open_time + QUESTION_SECS), st->qtext);
				}
			} else if (now - st->open_time > QUESTION_SECS) {
				// Already opened but not answered within the full minute:
				// failed -> 5-minute cooldown before a fresh attempt.
				printf("PAUSE: question expired; cooldown\n");
				st->phase = 0;
				st->cooldown_end = now + COOLDOWN_SECS;
				dirty = 1;
				sendf(c, "COOLDOWN %lld", (long long)COOLDOWN_SECS);
			} else {
				// Re-opened while the minute is still running: show the same
				// deadline (anchored to the first open), don't reset it.
				sendf(c, "QUESTION %d %s %lld %s", st->qnum + 1, st->nonce,
					(long long)(st->open_time + QUESTION_SECS), st->qtext);
			}
		} else {
			sendf(c, "GRANTED %s", st->grantmsg);
		}
	} else if (!strncmp(line, "ANSWER", 6)) {
		char nonce[64]; char ans[512];
		if (sscanf(line, "ANSWER %63s %511[^\n]", nonce, ans) < 2) { sendf(c, "BAD"); return dirty; }
		if (st->phase != 1 || strcmp(nonce, st->nonce) != 0) { sendf(c, "STALE"); return dirty; }
		time_t now = time(NULL);
		if (st->open_time == 0 || now - st->open_time > QUESTION_SECS + GRACE_SECS) {
			st->phase = 0;
			st->cooldown_end = now + COOLDOWN_SECS;
			sendf(c, "TIMEOUT %lld", (long long)COOLDOWN_SECS); dirty = 1;
			if (!dry) pause_delete_recreate_binary();
			return dirty;
		}
		double a; int found = parse_answer(ans, "", &a);
		if (found && round2ll(a) == round2ll(st->expected)) {
			st->qnum++;
			printf("PAUSE: correct (%d/%d)\n", st->qnum, QUESTIONS_NEEDED);
			if (st->qnum >= QUESTIONS_NEEDED) {
				st->count++;   // a fully solved challenge consumes a daily slot
				st->phase = 2; st->unblock_end = time(NULL) + UNBLOCK_SECS;
				for (int i = 0; i < NUM_SETS; i++) st->ff_active[i] = -1;
				if (!dry) {
					int snap[NUM_SETS];
					if (firefox_activeblock(ff_sync_sqlite, snap) == 0)
						for (int i = 0; i < NUM_SETS; i++) st->ff_active[i] = snap[i];
				}
				char tbuf[32]; strftime(tbuf, sizeof tbuf, "%H:%M:%S", localtime((time_t[]){time(NULL)}));
				snprintf(st->grantmsg, sizeof st->grantmsg,
					"You've unblocked it for 5 minutes at %s.", tbuf);
				printf("PAUSE: granted: %s\n", st->grantmsg);
				sendf(c, "GRANTED %s", st->grantmsg);
				if (!dry) { swap_unblocked(pw); pause_delete_recreate_binary(); }
			} else {
				gen_question(st->qtext, sizeof st->qtext, &st->expected);
				gen_nonce(st->nonce, sizeof st->nonce);
			// Next question is NOT available immediately: the open window
			// starts NEXT_Q_DELAY_SECS from now. HELLO before that returns
			// WAIT <secs>; after it the user has OPEN_WINDOW_SECS to open
			// cb-pause (else cooldown) and then a fresh full minute to answer
			// (open_time anchors the deadline on first open).
			st->qstart = time(NULL) + NEXT_Q_DELAY_SECS;
			st->open_time = 0;
			printf("PAUSE: next Q%d=%s (open in %ds, then within %ds)\n",
				st->qnum + 1, st->qtext, NEXT_Q_DELAY_SECS, OPEN_WINDOW_SECS);
			sendf(c, "CORRECT");
			if (!dry) pause_delete_recreate_binary();
			}
			dirty = 1;
		} else {
			printf("PAUSE: wrong answer; cooldown\n");
			st->phase = 0;
			st->cooldown_end = time(NULL) + COOLDOWN_SECS;
			sendf(c, "WRONG %lld", (long long)COOLDOWN_SECS);
			if (!dry) pause_delete_recreate_binary();
			dirty = 1;
		}
	}
	return dirty;
}

// End the normal poll cycle early when a SafeEyes work deadline falls within
// it. This keeps the broad enforcement cadence at ~28s without delaying a
// daemon-owned break by the remainder of that cadence.
static time_t pause_cycle_deadline(time_t cycle_start,
		time_t enforcement_deadline) {
	time_t cycle_end = cycle_start + CYCLE_BUDGET;
	if (enforcement_deadline > 0 && enforcement_deadline < cycle_end)
		return enforcement_deadline;
	return cycle_end;
}

// Run the pause poll loop for at most CYCLE_BUDGET seconds: keep the abstract
// socket open, accept + verify + handle cb-pause clients, and check the
// per-question timeout and unblock-window end at 1s granularity. If an active
// SafeEyes work deadline is sooner, return at that deadline so the persistent
// main loop immediately performs the break-enforcement pass.
static void pause_run(struct passwd *pw, const char *ff_sync_sqlite,
	const char *vv_leveldb, int dry, struct PauseState *st,
	time_t enforcement_deadline) {
	int sfd = pause_socket_setup();
	if (sfd < 0) fprintf(stderr, "ChickenBlocker: pause socket setup failed\n");
	time_t cycle_start = time(NULL);
	time_t cycle_end = pause_cycle_deadline(cycle_start, enforcement_deadline);
	int dirty = 0;
	while (time(NULL) < cycle_end) {
		if (!dry) pause_recreate_binary();
		if (sfd >= 0) {
			fd_set rf; FD_ZERO(&rf); FD_SET(sfd, &rf);
			struct timeval tv = { 1, 0 };
			int r = select(sfd + 1, &rf, NULL, NULL, &tv);
			if (r > 0) {
				int c = accept(sfd, NULL, NULL);
				if (c >= 0) { dirty |= pause_handle_conn(c, pw, ff_sync_sqlite, dry, st); close(c); }
			}
		} else sleep(1);
		if (time(NULL) >= cycle_end) {
			if (dirty) write_state(st);
			break;
		}
		enforce_vivaldi_managed_profile(pw, dry);
		time_t now = time(NULL);
		if (st->phase == 1) {
			int expired = 0;
			if (st->open_time == 0) {
				// User never opened cb-pause for this question in time.
				if (now - st->qstart > OPEN_WINDOW_SECS) {
					printf("PAUSE: open window missed (no answer); cooldown\n");
					expired = 1;
				}
			} else if (now - st->open_time > QUESTION_SECS + GRACE_SECS) {
				printf("PAUSE: question timed out (no answer); cooldown\n");
				expired = 1;
			}
			if (expired) {
				st->phase = 0;
				st->cooldown_end = now + COOLDOWN_SECS;
				dirty = 1;
			}
		}
		if (st->phase == 2 && now >= st->unblock_end) {
			printf("PAUSE: unblock window over; restoring blocking policies\n");
			if (!dry) swap_blocking(pw, ff_sync_sqlite, vv_leveldb, st);
			st->phase = 0; st->grantmsg[0] = 0; dirty = 1;
		}
		if (dirty) { write_state(st); dirty = 0; }
	}
	if (sfd >= 0) close(sfd);
}

// --- blocked browser snap removal -------------------------------------------
//
// AppArmor cannot filter `snap install <name>` by snap name (the snap client
// opens one socket then sends the name as protocol data; connect() is mediated
// before any name is on the wire). So instead of blocking install, the daemon
// DETECTS non-managed browser snaps every cycle and removes them within ~30s
// of them appearing. A browser that gets installed is usable only briefly
// before it is torn down, and the user cannot keep it.
//
// Managed snaps (NOT removed): firefox (ChickenBlocker manages it). Add a managed
// snap name to MANAGED_SNAPS to exempt it.
//
// How: `snap list` enumerates installed snaps; for each non-managed browser
// snap we fork+exec `snap remove <name>` (the daemon is root, the snap socket
// is NOT blocked by AppArmor here - only the apt path is pinned). We also kill
// the snap's processes first so `snap remove` doesn't hang on a running app.
//
// NOTE on AppArmor: ChickenBlocker runs unconfined, and `snap remove` is exec'd
// directly (fork+execvp, not system()/bin/sh), so it stays unconfined and can
// reach the snapd socket. (system() would transition /bin/sh to shell-bpf,
// which would still allow snap socket access - shell-bpf only denies bpf(2) -
// but direct exec is cleaner and avoids any future shell confinement.)

// Browser snap names we BLOCK (remove if present). The snap NAME (first column
// of `snap list`), not the launching binary's comm.
static const char *BLOCKED_SNAPS[] = {
	"chromium", "chromium-ffmpeg",
	"brave", "brave-ffmpeg",
	"google-chrome", "google-chrome-ffmpeg",
	"opera", "opera-ffmpeg",
	"microsoft-edge", "microsoft-edge-ffmpeg",
	"firefox-esr",            // firefox is managed; firefox-esr is a separate browser
	"vivaldi", "vivaldi-snapshot",
	"waterfox", "waterfox-classic",
	"palemoon", "palemoon-bin",
	"seamonkey",
	"torbrowser-launcher",
	"falkon", "qutebrowser", "nyxt", "midori", "epiphany",
	"netsurf",
	NULL
};

// Snaps ChickenBlocker MANAGES - never removed. Currently firefox (snap). Add a
// managed browser snap here when ChickenBlocker starts managing it.
static const char *MANAGED_SNAPS[] = {
	"firefox",
	NULL
};

static int snap_is_managed(const char *name) {
	for (int i = 0; MANAGED_SNAPS[i]; i++)
		if (strcmp(name, MANAGED_SNAPS[i]) == 0) return 1;
	return 0;
}

static int snap_is_blocked(const char *name) {
	for (int i = 0; BLOCKED_SNAPS[i]; i++)
		if (strcmp(name, BLOCKED_SNAPS[i]) == 0) return 1;
	return 0;
}

// Approximate comm name for a snap, used to kill its processes before remove.
// Most snap browsers run with comm == the snap name; chromium's comm is
// "chromium". Returns 0 on success (fills out), -1 if unknown.
static int snap_comm(const char *snap, char *out, size_t n) {
	// strip a "-ffmpeg"/"-bin" suffix; most map directly to comm.
	snprintf(out, n, "%s", snap);
	char *dash = strrchr(out, '-');
	if (dash && (strcmp(dash, "-ffmpeg") == 0 || strcmp(dash, "-bin") == 0))
		*dash = 0;
	return 0;
}

static void remove_blocked_snaps(int dry) {
	FILE *p = popen("snap list 2>/dev/null", "r");
	if (!p) return;
	char line[1024];
	// skip header line
	if (!fgets(line, sizeof line, p)) { pclose(p); return; }
	while (fgets(line, sizeof line, p)) {
		// snap list columns: Name Version Rev Tracking Publisher Notes
		char name[256];
		if (sscanf(line, "%255s", name) != 1) continue;
		if (!snap_is_blocked(name)) continue;
		if (snap_is_managed(name)) continue;
		char comm[256];
		snap_comm(name, comm, sizeof comm);
		printf("BLOCKED SNAP %s present; removing\n", name);
		if (dry) continue;
		// Kill its processes first so `snap remove` doesn't hang. Best-effort;
		// snap remove would force-shutdown the snap anyway if kill fails.
		proc_term_all(comm);
		pid_t pid = fork();
		if (pid == 0) {
			(void)freopen("/dev/null", "r", stdin);
			(void)freopen("/dev/null", "w", stdout);
			(void)freopen("/dev/null", "w", stderr);
			execlp("snap", "snap", "remove", name, "--purge", (char *)NULL);
			_exit(127);
		}
		if (pid > 0) {
			int status = 0;
			(void)waitpid(pid, &status, 0);
		}
	}
	pclose(p);
}

// --- main -------------------------------------------------------------------

#ifndef CHICKENBLOCKER_TEST
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	int dry = getenv("CHICKENBLOCKER_DRY_RUN") != NULL;
	for (;;) {
		// Reap detached browser launchers whose browser process has since exited.
		while (waitpid(-1, NULL, WNOHANG) > 0) {}

	// Remove any non-managed browser snaps that appeared since last cycle.
	// Done first so a freshly-installed browser is torn down within ~30s,
	// before any browser-policy sync runs. Root-only; harmless if no user.
	remove_blocked_snaps(dry);

	// The service starts at boot before a graphical login may exist. Keep the
	// persistent process alive and wait rather than failing into systemd's
	// restart rate limiter.
	char username[128];
	while (first_user(username, sizeof username) != 0) {
		fprintf(stderr, "ChickenBlocker: no logged-in user yet; retrying\n");
		sleep(2);
	}
	struct passwd *pw = getpwnam(username);
	if (!pw) { perror("getpwnam"); return 1; }
	const char *home = pw->pw_dir;

	// Vivaldi's managed UserDataDir policy does not reliably override Chromium's
	// profile switches. Tear down the complete process tree when any Vivaldi
	// process uses an alternate profile, then restore the managed instance.
	enforce_vivaldi_managed_profile(pw, dry);

	char firefox_path[1024];
	snprintf(firefox_path, sizeof firefox_path, "%s/snap/firefox/common/.mozilla/firefox", home);

	char profile_path[1024];
	if (find_profile_path(firefox_path, profile_path, sizeof profile_path)) {
		fprintf(stderr, "ChickenBlocker: could not find a profile Path= in %s/profiles.ini\n", firefox_path);
		return 1;
	}

	char ff_sync_sqlite[1280];
	snprintf(ff_sync_sqlite, sizeof ff_sync_sqlite, "%s/storage-sync-v2.sqlite", profile_path);

	// Vivaldi LeechBlock NG chrome.storage.local LevelDB. Only meaningful when
	// Vivaldi is installed; left as "" otherwise so callers can skip sync.
	char vv_leveldb[1536];
	vv_leveldb[0] = 0;
	if (access("/usr/bin/vivaldi-stable", F_OK) == 0) {
		snprintf(vv_leveldb, sizeof vv_leveldb,
			"%s/.config/vivaldi/Default/Local Extension Settings/%s",
			home, "blaaajhemilngeeffpbfkdjjoefldkok");
	}

	// Pause challenge state (root-only file). Read once; reset the daily
	// counter at midnight. phase != 0 skips the normal blocking sync below so
	// the pause logic owns browser policy during a challenge / unblock window.
	struct PauseState st;
	int have_state = (read_state(&st) == 0);
	if (!have_state) {
		memset(&st, 0, sizeof st);
		for (int i = 0; i < NUM_SETS; i++) st.ff_active[i] = -1;
	}
	char today[16];
	today_str(today, sizeof today);
	int pause_dirty_init = 0;
	if (strcmp(st.date, today) != 0) {
		st.date[0] = 0;
		snprintf(st.date, sizeof st.date, "%s", today);
		st.count = 0;
		pause_dirty_init = 1;
	}
	srand((unsigned)(time(NULL) ^ getpid()));
	int do_normal = (st.phase == 0);

	// Capture whether firefox is up BEFORE we touch anything. We only relaunch
	// it if it was already running - never start it on our own.
	int was_running = firefox_running();

	if (do_normal) {
	char ext_path[1280];
	snprintf(ext_path, sizeof ext_path, "%s/extensions.json", profile_path);
	char addon_startup_path[1280];
	snprintf(addon_startup_path, sizeof addon_startup_path, "%s/addonStartup.json.lz4", profile_path);

	int killed = 0, restart = 0;
	int ff_kill_ok = 1;   // result of the firefox term_and_wait (1 = no kill needed / succeeded)

	// Extension enforcement: re-enable leechblock-ng if it was disabled.
	size_t jlen;
	char *jbuf = read_file(ext_path, &jlen);
	Node *root = NULL;
	if (jbuf) {
		P = jbuf; END = jbuf + jlen;
		root = parse_value();
		Node *addons = obj_get(root, "addons");
		if (addons && addons->type == 1) {
			for (size_t i = 0; i < addons->n; i++) {
				Node *e = addons->vals[i];
				if (!e || e->type != 0) continue;
				Node *id = obj_get(e, "id");
				if (id && id->type == 2 && strcmp(id->raw, "leechblockng@proginosko.com") == 0) {
					Node *act = obj_get(e, "active");
					if (act && act->type == 3 && strcmp(act->raw, "false") == 0) {
						printf("EXTENSION ACTIVE: false\nNOT ACTIVE\n");
						set_bare(e, "active", "true");
						set_bare(e, "userDisabled", "false");
						killed = 1;
						restart = 1;
					if (!dry) {
						ff_kill_ok = (term_and_wait("firefox", 10) == 0);
						char *mem = NULL; size_t memlen = 0;
							FILE *mf = open_memstream(&mem, &memlen);
							ser(mf, root);
							fclose(mf);
							FILE *wf = fopen(ext_path, "wb");
							if (wf) { fwrite(mem, 1, memlen, wf); fclose(wf); }
							free(mem);
						}
					} else {
						printf("EXTENSION ACTIVE: %s\n", (act && act->type == 3) ? act->raw : "?");
					}
					break;
				}
			}
		}
	} else {
		fprintf(stderr, "ChickenBlocker: could not read %s\n", ext_path);
	}
	free_node(root);
	free(jbuf);

	// Policies.
	const char *policies_path = "/etc/firefox/policies/policies.json";
	if (!files_equal(policies_path, "policies.json")) {
		printf("COPYING policies.json\n");
		if (!dry) copy_file("policies.json", policies_path);
		restart = 1;
	}

	// userChrome.css.
	char chrome_path[1408];
	snprintf(chrome_path, sizeof chrome_path, "%s/chrome/userChrome.css", profile_path);
	if (!files_equal(chrome_path, "userChrome.css")) {
		printf("COPYING userChrome.css\n");
		if (!dry) copy_file("userChrome.css", chrome_path);
		restart = 1;
	}

	// userContent.css.
	char content_path[1408];
	snprintf(content_path, sizeof content_path, "%s/chrome/userContent.css", profile_path);
	if (!files_equal(content_path, "userContent.css")) {
		printf("COPYING userContent.css\n");
		if (!dry) copy_file("userContent.css", content_path);
		restart = 1;
	}

	if (restart) {
		if (!was_running) {
			printf("firefox was not running; changes applied, not launching\n");
		} else {
		if (!killed) {
			printf("KILLING FIREFOX\n");
			if (!dry) ff_kill_ok = (term_and_wait("firefox", 10) == 0);
		}
		if (dry || ff_kill_ok) {
			printf("RESTARTING FIREFOX\n");
			if (!dry) {
				if (access(addon_startup_path, F_OK) == 0) unlink(addon_startup_path);
				launch_firefox(pw);
			}
		} else {
			printf("firefox kill failed (AppArmor?); changes applied, not relaunching\n");
		}
		}
	}

	// --- Vivaldi (Chromium-based) policies ----------------------------------
	// Only act when Vivaldi is installed. Sync /etc/vivaldi/policies/managed/
	// ChickenBlocker.json from ./vivaldi-policies.json; on change, kill+relaunch
	// vivaldi ONLY if it was already running (never start it on our own).
	if (access("/usr/bin/vivaldi-stable", F_OK) == 0) {
		const char *vivaldi_policies_path =
			"/etc/vivaldi/policies/managed/ChickenBlocker.json";
		int v_was_running = vivaldi_running();
		int v_restart = 0;

		if (!files_equal(vivaldi_policies_path, "vivaldi-policies.json")) {
			printf("COPYING vivaldi-policies.json\n");
			if (!dry) copy_file("vivaldi-policies.json", vivaldi_policies_path);
			v_restart = 1;
		}

		// Extension enforcement (mirrors the Firefox check): inspect LeechBlock
		// NG in the Default profile's Preferences. Chromium has no per-extension
		// "active" flag; a non-empty disable_reasons array == disabled. If it is
		// disabled, restart Vivaldi so the ExtensionInstallForcelist policy
		// re-enables it. (Preferences, not Secure Preferences, holds settings in
		// this Vivaldi build; Secure Preferences is MAC-protected anyway.)
		// Gated by VV_RESTART_COOLDOWN: if disable_reasons persists across cycles
		// (the force-re-install did not clear it) we must NOT kill the browser
		// every ~28s or it becomes unusable. We retry once per cooldown window.
		const char *lb_id = "blaaajhemilngeeffpbfkdjjoefldkok";
		char vpref[1100];
		snprintf(vpref, sizeof vpref, "%s/.config/vivaldi/Default/Preferences", home);
		size_t vplen;
		char *vpbuf = read_file(vpref, &vplen);
		Node *vroot = NULL;
		if (vpbuf) {
			P = vpbuf; END = vpbuf + vplen;
			vroot = parse_value();
			Node *vext = obj_get(vroot, "extensions");
			Node *vsettings = vext ? obj_get(vext, "settings") : NULL;
			Node *lb = vsettings ? obj_get(vsettings, lb_id) : NULL;
			if (lb && lb->type == 0) {
				Node *dr = obj_get(lb, "disable_reasons");
				if (dr && dr->type == 1 && dr->n > 0) {
					time_t last = file_mtime(VV_RESTART_TS);
					long ago = (long)(time(NULL) - last);
					if (last == 0 || ago >= VV_RESTART_COOLDOWN) {
						printf("VIVALDI LEECHBLOCK DISABLED (disable_reasons count=%zu); restarting to re-enable\n", dr->n);
						v_restart = 1;
					} else {
						printf("VIVALDI LEECHBLOCK DISABLED; restart cooldown (%lds left)\n",
							(long)VV_RESTART_COOLDOWN - ago);
					}
				} else {
					printf("VIVALDI LEECHBLOCK ACTIVE\n");
				}
			} else {
				fprintf(stderr, "ChickenBlocker: LeechBlock NG not found in %s; policy will re-install on next launch\n", vpref);
			}
		} else {
			fprintf(stderr, "ChickenBlocker: could not read %s\n", vpref);
		}
		free_node(vroot);
		free(vpbuf);

		if (v_restart) {
			if (!v_was_running) {
				printf("vivaldi was not running; vivaldi policies applied, not launching\n");
			} else {
			printf("KILLING VIVALDI\n");
			int vv_ok = dry ? 1 : (term_and_wait("vivaldi-bin", 10) == 0);
			if (vv_ok) {
				printf("RESTARTING VIVALDI\n");
				if (!dry) { launch_vivaldi(pw); touch_file(VV_RESTART_TS); }
			} else {
				printf("vivaldi kill failed; vivaldi policies applied, not relaunching\n");
			}
			}
		} else {
			printf("vivaldi policies already in sync\n");
		}

		// LeechBlock config sync (Firefox -> Vivaldi). The LevelDB needs the
		// write lock, so:
		//  * if Vivaldi is NOT running: just write (no disruption), whenever
		//    there is real drift.
		//  * if Vivaldi IS running: only sync if there is drift, and only once
		//    per VV_RESTART_COOLDOWN window (kill+wait+sync+relaunch). This is
		//    what actually propagates rule edits from Firefox to Vivaldi within
		//    a few minutes instead of never, without killing the browser every
		//    cycle. A read-only diff (no lock) decides if a restart is warranted.
		if (vv_leveldb[0] && !dry) {
			int drift = vivaldi_diff_firefox(vv_leveldb, ff_sync_sqlite);
			if (drift < 0) {
				printf("VIVALDI LEECHBLOCK SYNC: diff failed\n");
			} else if (drift == 0) {
				printf("VIVALDI LEECHBLOCK SYNC: in sync (no drift)\n");
			} else if (!vivaldi_running()) {
				printf("VIVALDI LEECHBLOCK DRIFT %d; vivaldi stopped, syncing\n", drift);
				if (vivaldi_sync_from_firefox(vv_leveldb, ff_sync_sqlite) != 0)
					printf("VIVALDI LEECHBLOCK SYNC: FAILED\n");
			} else {
				time_t last = file_mtime(VV_RESTART_TS);
				long ago = (long)(time(NULL) - last);
				if (last != 0 && ago < VV_RESTART_COOLDOWN) {
					printf("VIVALDI LEECHBLOCK DRIFT %d; sync cooldown (%lds left)\n",
						drift, (long)VV_RESTART_COOLDOWN - ago);
				} else {
				printf("VIVALDI LEECHBLOCK DRIFT %d; restarting vivaldi to sync\n", drift);
				if (term_and_wait("vivaldi-bin", 10) != 0) {
					printf("vivaldi kill failed; not syncing/relaunching this cycle\n");
				} else if (vivaldi_sync_from_firefox(vv_leveldb, ff_sync_sqlite) == 0) {
					launch_vivaldi(pw);
					touch_file(VV_RESTART_TS);
				} else {
					printf("VIVALDI LEECHBLOCK SYNC: FAILED; relaunching anyway\n");
					launch_vivaldi(pw);
					touch_file(VV_RESTART_TS);
				}
				}
			}
		}
	}
	} /* end if (do_normal) */

	// --- SafeEyes enforcement (auto-start, pomodoro/night block, calendar pause).
	// Run every cycle under do_normal so it is skipped during a pause window.
	time_t enforcement_deadline = 0;
	if (do_normal) {
		struct SafeEyesState se;
		se_read_state(&se);
		safeeyes_run(pw, dry, &se);
		enforcement_deadline = se.se_work_end;
		// Power button lock is break-scoped: night (always a break) or an
		// active DAY break overlay (se_break_active; night never sets it).
		time_t now_kb = time(NULL);
		struct tm tm_kb; localtime_r(&now_kb, &tm_kb);
		de_killguard(pw, dry,
			night_mode_active(&tm_kb) || se.se_break_active);
		vt_night_guard(night_mode_active(&tm_kb), dry);
	}

	// --- pause: keep unblocked policies during a window, then run the poll
	// loop (socket server + timeouts). Normal blocking sync was skipped above
	// when a pause/challenge/window is in progress.
	if (st.phase == 2 && !dry) swap_unblocked(pw);

	if (pause_dirty_init) write_state(&st);
	pause_run(pw, ff_sync_sqlite, vv_leveldb, dry, &st,
		enforcement_deadline);
	// A deadline-shortened poll cycle must impose the break directly. Going
	// around the outer loop first would put snap/browser enforcement ahead of
	// SafeEyes again and could add several seconds of unrelated work.
	if (do_normal && st.phase == 0 && enforcement_deadline > 0
			&& time(NULL) >= enforcement_deadline) {
		struct SafeEyesState se;
		se_read_state(&se);
		safeeyes_run(pw, dry, &se);
		time_t now_kb = time(NULL);
		struct tm tm_kb; localtime_r(&now_kb, &tm_kb);
		de_killguard(pw, dry,
			night_mode_active(&tm_kb) || se.se_break_active);
		vt_night_guard(night_mode_active(&tm_kb), dry);
	}

	// Record the cycle's END wall-clock for fallback resume detection. The next
	// pass starts immediately, so any long gap indicates suspend or clock reset.
	if (do_normal) {
		struct SafeEyesState se;
		se_read_state(&se);
		se.se_last_cycle = time(NULL);
		se_write_state(&se);
	}
	}
	return 0;
}
#endif
