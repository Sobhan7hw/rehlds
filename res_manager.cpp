#include <extdll.h>
#include <meta_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/prctl.h>

#ifndef PR_SET_TIMERSLACK
#define PR_SET_TIMERSLACK 29
#endif

plugin_info_t Plugin_info = {
	META_INTERFACE_VERSION,
	"ResourceManager",
	"3.1",
	__DATE__,
	"FaTaL-[Hit]",
	"https://t.me/Neo_Norm",
	"RM",
	PT_STARTUP,
	PT_ANYTIME
};

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals = NULL;
meta_globals_t *gpMetaGlobals = NULL;
gamedll_funcs_t *gpGamedllFuncs = NULL;

/* rm_status: 0 = silent, 1 = print a status line to console every 60s */
static cvar_t g_RmStatus = { "rm_status", "0", 0.0f, 0, NULL };

#define RM_TAG "[RM]"
#define RM_MAX_CLIENTS 32

/* ---- constants ---- */
static const double POST_MAP_TRIM_SEC    = 10.0;
static const double LIMIT_TRIM_INTERVAL  = 60.0;
static const double RT_CPU_LIMIT         = 90.0;
static const int    RT_CPU_STRIKES       = 5;
static const double RESYNC_INTERVAL      = 60.0;  /* safety recount of players */
static const double RAM_INTERVAL_ACTIVE  = 15.0;
static const double RAM_INTERVAL_IDLE    = 60.0;
static const double POLL_ACTIVE          = 1.0;
static const double POLL_IDLE            = 5.0;
static const double ENFORCE_ACTIVE       = 0.25;
static const double ENFORCE_IDLE         = 2.0;
static const double CFG_CHECK_ACTIVE     = 2.0;
static const double CFG_CHECK_IDLE       = 10.0;

struct ResSettings {
	float idle_ticrate;
	float active_ticrate;      /* 0 = auto (target_fps * ticrate_factor) */
	float target_fps;
	float ticrate_factor;
	int   enforce_ticrate;
	float idle_delay;
	float idle_trim_interval;
	float connect_hold;
	float map_hold;
	int   show_fps;
	int   fps_mode;            /* 1 = top line, 2 = window title, 3 = both */
	float fps_update_interval;
	float term_refresh;        /* while players are present */
	float idle_term_refresh;   /* while the server is empty */
	int   target_cores;
	int   pin_main_thread;
	int   priority;
	int   realtime;
	int   realtime_prio;
	int   timer_slack_ns;
	int   trim_memory;
	long  max_memory_mb;
} g_Cfg;

struct RuntimeState {
	int     initialized;
	int     tuned;
	int     term_usable;
	int     term_ready;
	int     player_count;
	int     players_before_change;
	int     applied_ticrate;
	int     orig_saved;
	float   orig_ticrate;
	cvar_t *tic_cvar;

	double  hold_active_until;
	double  last_player_time;

	double  last_poll;
	double  last_enforce;
	double  last_cfg_check;
	double  last_resync;
	double  last_cpu_wall;
	double  last_cpu_time;
	double  last_ram_check;
	double  last_term_draw;
	double  last_trim;
	double  trim_at;
	double  fps_window_start;
	unsigned long frames;
	float   current_fps;
	double  cpu_usage;
	long    ram_usage_mb;
	int     was_active;

	int     rt_on;
	int     rt_blocked;
	int     rt_strikes;
	int     prio_applied;

	char    status[200];
} g_State;

static unsigned char g_Slot[RM_MAX_CLIENTS + 1];
static double g_LastCvarStatus = 0.0;
static int  g_StatmFd = -1;
static long g_PageSize = 4096;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static double NowSec() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double ProcessCpuSec() {
	struct timespec ts;
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0.0;
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void trim(char *s) {
	char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n > 0) {
		char c = s[n - 1];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') s[--n] = 0;
		else break;
	}
}

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static int clampi(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static void ConPrint(const char *fmt, ...) {
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_engfuncs.pfnServerPrint(buf);
}

static int ActiveTic() {
	float t = (g_Cfg.active_ticrate > 0.0f) ? g_Cfg.active_ticrate
	: g_Cfg.target_fps * g_Cfg.ticrate_factor;
	return (int)(clampf(t, 10.0f, 100000.0f) + 0.5f);
}

static int IdleTic() {
	return (int)(g_Cfg.idle_ticrate + 0.5f);
}

/* ------------------------------------------------------------------ */
/* Config (optional file, auto reloaded when it changes)               */
/* ------------------------------------------------------------------ */

static const char *g_Paths[] = {
	"addons/resource_manager/resource_manager.ini",
	"cstrike/addons/resource_manager/resource_manager.ini",
	"../cstrike/addons/resource_manager/resource_manager.ini",
	"resource_manager.ini",
	/* old names, still accepted */
	"addons/res_manager/res_manager.ini",
	"cstrike/addons/res_manager/res_manager.ini",
	"../cstrike/addons/res_manager/res_manager.ini",
	"res_manager.ini"
};
static char   g_CfgPath[256] = "";
static time_t g_CfgMtime = 0;

static int try_load_config(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (line[0] == 0 || line[0] == ';' || line[0] == '#') continue;

		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *key = line;
		char *val = eq + 1;
		for (char *c = val; *c; c++) {
			if (*c == ';' || *c == '#') { *c = 0; break; }
		}
		trim(key);
		trim(val);

		if      (strcmp(key, "idle_ticrate") == 0)        g_Cfg.idle_ticrate = (float)atof(val);
		else if (strcmp(key, "active_ticrate") == 0)      g_Cfg.active_ticrate = (float)atof(val);
		else if (strcmp(key, "target_fps") == 0)          g_Cfg.target_fps = (float)atof(val);
		else if (strcmp(key, "ticrate_factor") == 0)      g_Cfg.ticrate_factor = (float)atof(val);
		else if (strcmp(key, "enforce_ticrate") == 0)     g_Cfg.enforce_ticrate = atoi(val);
		else if (strcmp(key, "idle_delay") == 0)          g_Cfg.idle_delay = (float)atof(val);
		else if (strcmp(key, "idle_trim_interval") == 0)  g_Cfg.idle_trim_interval = (float)atof(val);
		else if (strcmp(key, "connect_hold") == 0)        g_Cfg.connect_hold = (float)atof(val);
		else if (strcmp(key, "map_hold") == 0)            g_Cfg.map_hold = (float)atof(val);
		else if (strcmp(key, "show_fps") == 0)            g_Cfg.show_fps = atoi(val);
		else if (strcmp(key, "fps_mode") == 0)            g_Cfg.fps_mode = atoi(val);
		else if (strcmp(key, "fps_update_interval") == 0) g_Cfg.fps_update_interval = (float)atof(val);
		else if (strcmp(key, "term_refresh") == 0)        g_Cfg.term_refresh = (float)atof(val);
		else if (strcmp(key, "idle_term_refresh") == 0)   g_Cfg.idle_term_refresh = (float)atof(val);
		else if (strcmp(key, "target_cores") == 0)        g_Cfg.target_cores = atoi(val);
		else if (strcmp(key, "pin_main_thread") == 0)     g_Cfg.pin_main_thread = atoi(val);
		else if (strcmp(key, "priority") == 0)            g_Cfg.priority = atoi(val);
		else if (strcmp(key, "realtime") == 0)            g_Cfg.realtime = atoi(val);
		else if (strcmp(key, "realtime_prio") == 0)       g_Cfg.realtime_prio = atoi(val);
		else if (strcmp(key, "timer_slack_ns") == 0)      g_Cfg.timer_slack_ns = atoi(val);
		else if (strcmp(key, "trim_memory") == 0)         g_Cfg.trim_memory = atoi(val);
		else if (strcmp(key, "max_memory_mb") == 0)       g_Cfg.max_memory_mb = atol(val);
	}
	fclose(f);

	strncpy(g_CfgPath, path, sizeof(g_CfgPath) - 1);
	g_CfgPath[sizeof(g_CfgPath) - 1] = 0;
	struct stat st;
	g_CfgMtime = (stat(path, &st) == 0) ? st.st_mtime : 0;
	return 1;
}

static void SetDefaults() {
	g_Cfg.idle_ticrate = 100.0f;
	g_Cfg.active_ticrate = 0.0f;
	g_Cfg.target_fps = 1000.0f;
	g_Cfg.ticrate_factor = 1.5f;
	g_Cfg.enforce_ticrate = 1;
	g_Cfg.idle_delay = 30.0f;
	g_Cfg.idle_trim_interval = 300.0f;
	g_Cfg.connect_hold = 20.0f;
	g_Cfg.map_hold = 45.0f;
	g_Cfg.show_fps = 1;
	g_Cfg.fps_mode = 1;
	g_Cfg.fps_update_interval = 1.0f;
	g_Cfg.term_refresh = 1.0f;
	g_Cfg.idle_term_refresh = 5.0f;
	g_Cfg.target_cores = 0;
	g_Cfg.pin_main_thread = 1;
	g_Cfg.priority = 0;
	g_Cfg.realtime = 1;
	g_Cfg.realtime_prio = 10;
	g_Cfg.timer_slack_ns = 1;
	g_Cfg.trim_memory = 1;
	g_Cfg.max_memory_mb = 0;
}

static void ClampConfig() {
	g_Cfg.idle_ticrate = clampf(g_Cfg.idle_ticrate, 10.0f, 100000.0f);
	if (g_Cfg.active_ticrate < 0.0f) g_Cfg.active_ticrate = 0.0f;
	if (g_Cfg.active_ticrate > 0.0f) g_Cfg.active_ticrate = clampf(g_Cfg.active_ticrate, 10.0f, 100000.0f);
	g_Cfg.target_fps = clampf(g_Cfg.target_fps, 20.0f, 100000.0f);
	g_Cfg.ticrate_factor = clampf(g_Cfg.ticrate_factor, 1.0f, 5.0f);
	g_Cfg.idle_delay = clampf(g_Cfg.idle_delay, 0.0f, 3600.0f);
	g_Cfg.idle_trim_interval = clampf(g_Cfg.idle_trim_interval, 30.0f, 86400.0f);
	g_Cfg.connect_hold = clampf(g_Cfg.connect_hold, 0.0f, 600.0f);
	g_Cfg.map_hold = clampf(g_Cfg.map_hold, 0.0f, 600.0f);
	g_Cfg.fps_update_interval = clampf(g_Cfg.fps_update_interval, 1.0f, 60.0f);
	g_Cfg.term_refresh = clampf(g_Cfg.term_refresh, 0.5f, 10.0f);
	g_Cfg.idle_term_refresh = clampf(g_Cfg.idle_term_refresh, 1.0f, 60.0f);
	g_Cfg.fps_mode = clampi(g_Cfg.fps_mode, 1, 3);
	g_Cfg.priority = clampi(g_Cfg.priority, -20, 19);
	g_Cfg.realtime_prio = clampi(g_Cfg.realtime_prio, 1, 50);
	g_Cfg.timer_slack_ns = clampi(g_Cfg.timer_slack_ns, 0, 1000000);
	if (g_Cfg.target_cores < 0) g_Cfg.target_cores = 0;
	if (g_Cfg.max_memory_mb < 0) g_Cfg.max_memory_mb = 0;
}

/* returns true if a file was read. On a failed reload the old settings stay. */
static bool LoadConfig(bool reload) {
	ResSettings old = g_Cfg;
	SetDefaults();

	bool ok = false;
	if (reload && g_CfgPath[0]) {
		ok = try_load_config(g_CfgPath) != 0;
	} else {
		for (size_t i = 0; i < sizeof(g_Paths) / sizeof(g_Paths[0]); i++) {
			if (try_load_config(g_Paths[i])) { ok = true; break; }
		}
	}

	if (!ok) {
		if (reload) g_Cfg = old;
		else ClampConfig();
		return false;
	}
	ClampConfig();
	return true;
}

/* ------------------------------------------------------------------ */
/* Terminal (own non-blocking fd: a slow terminal can never stall the  */
/* engine's main thread)                                               */
/* ------------------------------------------------------------------ */

static int    g_TermFd = -1;
static char   g_Pend[1100];
static size_t g_PendLen = 0;

static void TermOpen() {
	if (!isatty(STDOUT_FILENO)) return;
	/* a separate open file description, so O_NONBLOCK does not leak to other processes */
	g_TermFd = open("/proc/self/fd/1", O_WRONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
}

static void TermClose() {
	if (g_TermFd >= 0) {
		close(g_TermFd);
		g_TermFd = -1;
	}
	g_PendLen = 0;
}

/* true = accepted (written or queued), false = dropped (terminal busy) */
static bool TermSend(const char *buf, size_t len) {
	if (g_TermFd < 0 || len == 0) return false;

	if (g_PendLen > 0) {
		ssize_t r = write(g_TermFd, g_Pend, g_PendLen);
		if (r > 0) {
			memmove(g_Pend, g_Pend + r, g_PendLen - (size_t)r);
			g_PendLen -= (size_t)r;
		}
		if (g_PendLen > 0) return false; /* still busy: skip this frame */
	}

	ssize_t r = write(g_TermFd, buf, len);
	if (r < 0) return false; /* EAGAIN or error: nothing was written */
		if ((size_t)r < len) {
			size_t rest = len - (size_t)r;
			if (rest > sizeof(g_Pend)) rest = sizeof(g_Pend);
			memcpy(g_Pend, buf + r, rest);
			g_PendLen = rest;
		}
		return true;
}

static void GetTermSize(int &rows, int &cols) {
	struct winsize w;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row >= 4 && w.ws_col >= 20) {
		rows = w.ws_row;
		cols = w.ws_col;
	} else {
		rows = 24;
		cols = 80;
	}
}

static void Append(char *out, size_t &n, size_t cap, const char *fmt, ...) {
	if (n >= cap - 1) return;
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(out + n, cap - n, fmt, ap);
	va_end(ap);
	if (r > 0) {
		size_t add = (size_t)r;
		if (add > cap - n - 1) add = cap - n - 1;
		n += add;
	}
}

static void DrawStatus() {
	if (!g_State.term_usable || g_TermFd < 0 || !g_State.status[0]) return;

	int rows, cols;
	GetTermSize(rows, cols);

	char out[1024];
	size_t n = 0;
	const size_t cap = sizeof(out);
	bool first = false;

	if (g_Cfg.fps_mode & 2) {
		Append(out, n, cap, "\033]0;%s\007", g_State.status);
	}

	if (g_Cfg.fps_mode & 1) {
		if (!g_State.term_ready) {
			Append(out, n, cap, "\033[%d;1H\n\033[%d;1H", rows, rows);
			first = true;
		}
		/* scroll region re-applied on every draw: survives resize, clear, map change */
		Append(out, n, cap,
			   "\033" "7"
			   "\033[2;%dr"
			   "\033[1;1H\033[2K"
			   "\033[31m%.*s\033[0m"
			   "\033" "8",
		 rows, cols - 1, g_State.status);
	}

	if (n > 0) {
		fflush(stdout);
		if (TermSend(out, n) && first) g_State.term_ready = 1;
	}
}

static void TermReset() {
	if (!g_State.term_usable || !g_State.term_ready || g_TermFd < 0) return;
	int rows, cols;
	GetTermSize(rows, cols);
	char out[64];
	int n = snprintf(out, sizeof(out), "\033[r\033[1;1H\033[2K\033[%d;1H", rows);
	if (n > 0) {
		fflush(stdout);
		TermSend(out, (size_t)n);
	}
	g_State.term_ready = 0;
}

static void AtExitReset() {
	TermReset();
}

/* ------------------------------------------------------------------ */
/* System tuning                                                       */
/* ------------------------------------------------------------------ */

static cpu_set_t g_OrigSet;
static cpu_set_t g_TargetSet;
static int       g_OrigSaved = 0;
static int       g_AffApplied = 0;
static int       g_Pinned = 0;
static int       g_PrioOk = 0;
static int       g_PrioFail = 0;
static int       g_PrioErr = 0;

static void ApplyAffinityToTask(pid_t tid) {
	sched_setaffinity(tid, sizeof(g_TargetSet), &g_TargetSet);
}

static void ApplyPriorityToTask(pid_t tid) {
	if (setpriority(PRIO_PROCESS, (id_t)tid, g_Cfg.priority) == 0) g_PrioOk++;
	else { g_PrioFail++; g_PrioErr = errno; }
}

static void ForEachTask(void (*fn)(pid_t)) {
	DIR *d = opendir("/proc/self/task");
	if (!d) {
		fn(0);
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
		fn((pid_t)atoi(e->d_name));
	}
	closedir(d);
}

/* Realtime applies to the calling thread only (hooks run on the engine main thread). */
static void SetRealtime(bool on) {
	if (on == (g_State.rt_on != 0)) return;

	struct sched_param sp;
	memset(&sp, 0, sizeof(sp));

	if (on) {
		sp.sched_priority = g_Cfg.realtime_prio;
		if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) {
			g_State.rt_on = 1;
		} else {
			g_State.rt_blocked = 1;
			ConPrint(RM_TAG " realtime unavailable (%s). Needs root or CAP_SYS_NICE; continuing normally.\n",
					 strerror(errno));
		}
	} else {
		sp.sched_priority = 0;
		sched_setscheduler(0, SCHED_OTHER, &sp);
		g_State.rt_on = 0;
	}
}

static void ApplyRealtimeState() {
	if (g_Cfg.realtime && !g_State.rt_blocked) SetRealtime(g_State.player_count > 0);
	else if (!g_Cfg.realtime && g_State.rt_on) SetRealtime(false);
}

/* Keeps the main (engine) thread on one core: no migrations = steadier frames. */
static void ApplyMainThreadPin() {
	if (!g_OrigSaved) return;
	cpu_set_t base = g_AffApplied ? g_TargetSet : g_OrigSet;

	if (g_Cfg.pin_main_thread && CPU_COUNT(&base) > 1) {
		int cpu = sched_getcpu();
		if (cpu >= 0 && cpu < CPU_SETSIZE && CPU_ISSET(cpu, &base)) {
			cpu_set_t one;
			CPU_ZERO(&one);
			CPU_SET(cpu, &one);
			if (sched_setaffinity(0, sizeof(one), &one) == 0) g_Pinned = 1;
		}
	} else if (g_Pinned) {
		sched_setaffinity(0, sizeof(base), &base);
		g_Pinned = 0;
	}
}

/* runs in the main thread; called again after every config reload */
static void ApplySystemTuning() {
	g_State.tuned = 1;

	if (g_Cfg.timer_slack_ns > 0)
		prctl(PR_SET_TIMERSLACK, (unsigned long)g_Cfg.timer_slack_ns, 0UL, 0UL, 0UL);

	if (!g_OrigSaved) {
		CPU_ZERO(&g_OrigSet);
		if (sched_getaffinity(0, sizeof(g_OrigSet), &g_OrigSet) == 0) g_OrigSaved = 1;
	}
	if (g_OrigSaved) {
		int avail = CPU_COUNT(&g_OrigSet);
		int want = g_Cfg.target_cores;
		if (want > 0 && avail > 1 && want < avail) {
			CPU_ZERO(&g_TargetSet);
			int picked = 0;
			for (int cpu = CPU_SETSIZE - 1; cpu >= 0 && picked < want; --cpu) {
				if (CPU_ISSET(cpu, &g_OrigSet)) {
					CPU_SET(cpu, &g_TargetSet);
					picked++;
				}
			}
			if (picked > 0) {
				ForEachTask(ApplyAffinityToTask);
				g_AffApplied = 1;
				g_Pinned = 0;
			}
		} else if (g_AffApplied) {
			g_TargetSet = g_OrigSet;
			ForEachTask(ApplyAffinityToTask);
			g_AffApplied = 0;
			g_Pinned = 0;
		}
		ApplyMainThreadPin();
	}

	if (g_Cfg.priority != 0 || g_State.prio_applied) {
		g_PrioOk = 0;
		g_PrioFail = 0;
		ForEachTask(ApplyPriorityToTask);
		if (g_PrioOk == 0 && g_PrioFail > 0) {
			ConPrint(RM_TAG " priority %d failed (%s). Run as root or give CAP_SYS_NICE.\n",
					 g_Cfg.priority, strerror(g_PrioErr));
		}
		g_State.prio_applied = (g_Cfg.priority != 0) ? 1 : 0;
	}
}

/* ------------------------------------------------------------------ */
/* Stats                                                               */
/* ------------------------------------------------------------------ */

static void ReadCpuUsage(double now) {
	double cpu = ProcessCpuSec();
	double dt = now - g_State.last_cpu_wall;
	if (g_State.last_cpu_wall > 0.0 && dt > 0.0) {
		g_State.cpu_usage = ((cpu - g_State.last_cpu_time) / dt) * 100.0;
	}
	g_State.last_cpu_time = cpu;
	g_State.last_cpu_wall = now;
}

/* statm fd stays open: only a pread per check, no fopen in the main thread */
static void ReadRamUsage() {
	if (g_StatmFd < 0) return;
	char b[128];
	ssize_t n = pread(g_StatmFd, b, sizeof(b) - 1, 0);
	if (n <= 0) return;
	b[n] = 0;
	long total = 0, resident = 0;
	if (sscanf(b, "%ld %ld", &total, &resident) == 2) {
		g_State.ram_usage_mb = (resident * g_PageSize) / (1024 * 1024);
	}
}

/* ------------------------------------------------------------------ */
/* Players (counted by hooks, full recount only as a safety net)       */
/* ------------------------------------------------------------------ */

static void RecountSlots() {
	int c = 0;
	for (int i = 1; i <= RM_MAX_CLIENTS; i++) c += g_Slot[i] ? 1 : 0;
	g_State.player_count = c;
}

static void RescanPlayers() {
	if (!gpGlobals) return;
	int maxc = gpGlobals->maxClients;
	if (maxc > RM_MAX_CLIENTS) maxc = RM_MAX_CLIENTS;
	memset(g_Slot, 0, sizeof(g_Slot));
	for (int i = 1; i <= maxc; i++) {
		edict_t *p = INDEXENT(i);
		if (!p || p->free) continue;
		int fl = p->v.flags;
		if ((fl & FL_CLIENT) && !(fl & FL_FAKECLIENT)) g_Slot[i] = 1;
	}
	RecountSlots();
}

/* ------------------------------------------------------------------ */
/* Ticrate control                                                     */
/* ------------------------------------------------------------------ */

static bool IsActive(double now) {
	return g_State.player_count > 0
	|| now < g_State.hold_active_until
	|| (now - g_State.last_player_time) < (double)g_Cfg.idle_delay;
}

static void SetTicrate(int want) {
	if (!g_State.tic_cvar)
		g_State.tic_cvar = g_engfuncs.pfnCVarGetPointer("sys_ticrate");

	bool need;
	if (g_State.tic_cvar) need = fabsf(g_State.tic_cvar->value - (float)want) >= 0.5f;
	else                  need = (g_State.applied_ticrate != want);

	if (need) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%d", want);
		g_engfuncs.pfnCVarSetString("sys_ticrate", buf);
	}
	g_State.applied_ticrate = want;
}

static void SyncTicrate(double now, bool strict) {
	int want = IsActive(now) ? ActiveTic() : IdleTic();
	if (strict || want != g_State.applied_ticrate) SetTicrate(want);
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static void DoTrim(double now) {
	malloc_trim(0);
	g_State.last_trim = now;
	ReadRamUsage();
}

static void ManageMemory(double now) {
	if (!g_Cfg.trim_memory) return;

	if (g_State.trim_at > 0.0 && now >= g_State.trim_at) {
		g_State.trim_at = 0.0;
		DoTrim(now);
		return;
	}
	if (!IsActive(now) && now - g_State.last_trim >= (double)g_Cfg.idle_trim_interval) {
		DoTrim(now);
		return;
	}
	if (g_Cfg.max_memory_mb > 0 && g_State.ram_usage_mb > g_Cfg.max_memory_mb
		&& now - g_State.last_trim >= LIMIT_TRIM_INTERVAL) {
		DoTrim(now);
		}
}

/* ------------------------------------------------------------------ */
/* Config hot reload                                                   */
/* ------------------------------------------------------------------ */

static void OnConfigReloaded(double now) {
	g_State.tuned = 0;
	g_State.last_poll = 0.0;
	g_State.last_term_draw = 0.0;
	g_State.rt_blocked = 0;
	g_State.rt_strikes = 0;
	ApplyRealtimeState();
	SyncTicrate(now, true);

	ConPrint(RM_TAG " config reloaded: idle=%d, active=%d, target_fps=%.0f\n",
			 IdleTic(), ActiveTic(), g_Cfg.target_fps);
}

static void CheckConfigReload(double now, double interval) {
	if (now - g_State.last_cfg_check < interval) return;
	g_State.last_cfg_check = now;

	struct stat st;
	if (g_CfgPath[0]) {
		if (stat(g_CfgPath, &st) != 0) return;
		if (st.st_mtime == g_CfgMtime) return;
	} else {
		bool found = false;
		for (size_t i = 0; i < sizeof(g_Paths) / sizeof(g_Paths[0]); i++) {
			if (stat(g_Paths[i], &st) == 0) { found = true; break; }
		}
		if (!found) return;
	}

	if (LoadConfig(true)) OnConfigReloaded(now);
	else if (g_CfgPath[0]) g_CfgMtime = st.st_mtime;
}

/* ------------------------------------------------------------------ */
/* Status text                                                         */
/* ------------------------------------------------------------------ */

static void BuildStatus(bool active) {
	snprintf(g_State.status, sizeof(g_State.status),
			 RM_TAG " P:%2d FPS:%6.1f CPU:%5.1f%% RAM:%4ldMB tic:%5d%c%s",
		  g_State.player_count, g_State.current_fps,
		  g_State.cpu_usage, g_State.ram_usage_mb, g_State.applied_ticrate,
		  active ? 'A' : 'I', g_State.rt_on ? "R" : "");
}

/* ------------------------------------------------------------------ */
/* Console commands                                                    */
/* ------------------------------------------------------------------ */

static void Cmd_Status(void) {
	bool active = IsActive(NowSec());
	ConPrint(RM_TAG " status: %s | players=%d fps=%.1f cpu=%.1f%% ram=%ldMB tic=%d (idle=%d active=%d)%s\n",
			 active ? "ACTIVE" : "IDLE", g_State.player_count, g_State.current_fps,
			 g_State.cpu_usage, g_State.ram_usage_mb, g_State.applied_ticrate,
			 IdleTic(), ActiveTic(), g_State.rt_on ? " [realtime]" : "");
	ConPrint(RM_TAG " config: %s | trim=%d maxmem=%ldMB pin=%d cores=%d prio=%d slack=%dns\n",
			 g_CfgPath[0] ? g_CfgPath : "(defaults)", g_Cfg.trim_memory,
			 g_Cfg.max_memory_mb, g_Cfg.pin_main_thread, g_Cfg.target_cores,
			 g_Cfg.priority, g_Cfg.timer_slack_ns);
}

static void Cmd_Reload(void) {
	if (!g_CfgPath[0]) { ConPrint(RM_TAG " no ini file in use, nothing to reload.\n"); return; }
	if (LoadConfig(true)) OnConfigReloaded(NowSec());
	else ConPrint(RM_TAG " reload failed, keeping current settings.\n");
}

static void Cmd_Trim(void) {
	long before = g_State.ram_usage_mb;
	DoTrim(NowSec());
	ConPrint(RM_TAG " malloc_trim: RAM %ld -> %ld MB\n", before, g_State.ram_usage_mb);
}

static void Cmd_Realtime(void) {
	int argc = g_engfuncs.pfnCmd_Argc();
	const char *a = (argc > 1) ? g_engfuncs.pfnCmd_Argv(1) : NULL;
	if (a && a[0]) {
		int on = atoi(a);
		g_Cfg.realtime = on ? 1 : 0;
		g_State.rt_blocked = 0;
		g_State.rt_strikes = 0;
	}
	ApplyRealtimeState();
	ConPrint(RM_TAG " realtime: %s%s\n", g_State.rt_on ? "ON" : "OFF",
			 g_State.rt_blocked ? " (blocked by system)" : "");
}


static void RegisterCommands() {
	g_engfuncs.pfnAddCommand("rm_status", Cmd_Status);
	g_engfuncs.pfnAddCommand("rm_reload", Cmd_Reload);
	g_engfuncs.pfnAddCommand("rm_trim", Cmd_Trim);
	g_engfuncs.pfnAddCommand("rm_realtime", Cmd_Realtime);
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

static void OnStartFrame() {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_State.initialized) return;

	double now = NowSec();
	g_State.frames++;

	bool active = IsActive(now);

	/* state change: restart the FPS window so idle and active values never mix */
	if (active != (g_State.was_active != 0)) {
		g_State.was_active = active ? 1 : 0;
		g_State.frames = 0;
		g_State.fps_window_start = now;
		g_State.last_term_draw = 0.0;
	}

	CheckConfigReload(now, active ? CFG_CHECK_ACTIVE : CFG_CHECK_IDLE);
	if (!g_State.tuned) ApplySystemTuning();

	/* periodic work: every 1s with players, every 5s when the server is empty */
	if (now - g_State.last_poll >= (active ? POLL_ACTIVE : POLL_IDLE)) {
		g_State.last_poll = now;
		ReadCpuUsage(now);

		if (now - g_State.last_resync >= RESYNC_INTERVAL) {
			g_State.last_resync = now;
			RescanPlayers();
		}

		SyncTicrate(now, g_Cfg.enforce_ticrate != 0);

		if (g_State.rt_on) {
			if (g_State.cpu_usage > RT_CPU_LIMIT) g_State.rt_strikes++;
			else g_State.rt_strikes = 0;
			if (g_State.rt_strikes >= RT_CPU_STRIKES) {
				SetRealtime(false);
				g_State.rt_blocked = 1;
				ConPrint(RM_TAG " CPU stayed above %.0f%%: realtime disabled for safety.\n", RT_CPU_LIMIT);
			}
		}
		ApplyRealtimeState();

		double ram_int = active ? RAM_INTERVAL_ACTIVE : RAM_INTERVAL_IDLE;
		if (now - g_State.last_ram_check >= ram_int) {
			g_State.last_ram_check = now;
			ReadRamUsage();
		}
		ManageMemory(now);
	}

	/* undo outside changes of sys_ticrate (server.cfg, rcon) */
	if (g_Cfg.enforce_ticrate && now - g_State.last_enforce >= (active ? ENFORCE_ACTIVE : ENFORCE_IDLE)) {
		g_State.last_enforce = now;
		SyncTicrate(now, true);
	}

	double win = now - g_State.fps_window_start;
	double win_len = active ? (double)g_Cfg.fps_update_interval : (double)g_Cfg.idle_term_refresh;
	if (win >= win_len) {
		g_State.current_fps = (float)((double)g_State.frames / win);
		g_State.frames = 0;
		g_State.fps_window_start = now;
		BuildStatus(active);
	}

	/* rm_status cvar: one console line per minute for log-based monitoring */
	if (g_RmStatus.value >= 1.0f && now - g_LastCvarStatus >= 60.0) {
		g_LastCvarStatus = now;
		BuildStatus(active);
		ConPrint(RM_TAG " %s\n", g_State.status);
	}

	if (g_Cfg.show_fps) {
		double draw_int = active ? (double)g_Cfg.term_refresh : (double)g_Cfg.idle_term_refresh;
		if (now - g_State.last_term_draw >= draw_int) {
			g_State.last_term_draw = now;
			DrawStatus();
		}
	}
}

static qboolean OnClientConnect(edict_t *pEntity, const char *pszName,
							const char *pszAddress, char szRejectReason[128]) {
	(void)pEntity; (void)pszName; (void)pszAddress; (void)szRejectReason;
	gpMetaGlobals->mres = MRES_IGNORED;
	if (g_State.initialized) {
		double now = NowSec();
		double until = now + (double)g_Cfg.connect_hold;
		if (until > g_State.hold_active_until) g_State.hold_active_until = until;
		SyncTicrate(now, true);
	}
	return TRUE;
}

/* fires before the map changes: remember how many players were on */
static void OnServerDeactivate(void) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_State.initialized) return;
	g_State.players_before_change = g_State.player_count;
}

static void OnClientPutInServer(edict_t *pEntity) {
		gpMetaGlobals->mres = MRES_IGNORED;
		if (!g_State.initialized || !pEntity) return;

		int idx = g_engfuncs.pfnIndexOfEdict(pEntity);
		if (idx < 1 || idx > RM_MAX_CLIENTS) return;
		if (pEntity->v.flags & FL_FAKECLIENT) return; /* bots are not players */

		g_Slot[idx] = 1;
		RecountSlots();

		double now = NowSec();
		g_State.last_player_time = now;
		SyncTicrate(now, true);   /* active ticrate right now, not at the next poll */
		ApplyRealtimeState();
		}

		static void OnClientDisconnect(edict_t *pEntity) {
		gpMetaGlobals->mres = MRES_IGNORED;
		if (!g_State.initialized || !pEntity) return;

		int idx = g_engfuncs.pfnIndexOfEdict(pEntity);
		if (idx < 1 || idx > RM_MAX_CLIENTS) return;

		g_Slot[idx] = 0;
		RecountSlots();
		if (g_State.player_count == 0) g_State.last_player_time = NowSec();
		ApplyRealtimeState();
		}

		static void OnServerActivate_Post(edict_t *pEdictList, int edictCount, int clientMax) {
		gpMetaGlobals->mres = MRES_IGNORED;
		if (!g_State.initialized) return;

		double now = NowSec();
		g_State.frames = 0;
		g_State.fps_window_start = now;
		g_State.last_poll = 0.0;
		g_State.last_term_draw = 0.0;
		g_State.tic_cvar = g_engfuncs.pfnCVarGetPointer("sys_ticrate");

		/* clients reconnect to the new map through ClientPutInServer */
		memset(g_Slot, 0, sizeof(g_Slot));
		g_State.player_count = 0;

		if (g_State.players_before_change > 0) {
		double until = now + (double)g_Cfg.map_hold;
		if (until > g_State.hold_active_until) g_State.hold_active_until = until;
		g_State.last_player_time = now;
		}
		g_State.players_before_change = 0;

		SyncTicrate(now, true);
		if (g_Cfg.trim_memory) g_State.trim_at = now + POST_MAP_TRIM_SEC;
		}

		/* ------------------------------------------------------------------ */
		/* Metamod exports                                                     */
		/* ------------------------------------------------------------------ */

		C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion) {
		if (!pFunctionTable || !interfaceVersion || *interfaceVersion != INTERFACE_VERSION) {
		if (interfaceVersion) *interfaceVersion = INTERFACE_VERSION;
		return FALSE;
		}
		memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
		pFunctionTable->pfnStartFrame = OnStartFrame;
		pFunctionTable->pfnClientConnect = OnClientConnect;
		pFunctionTable->pfnClientPutInServer = OnClientPutInServer;
		pFunctionTable->pfnClientDisconnect = OnClientDisconnect;
	pFunctionTable->pfnServerDeactivate = OnServerDeactivate;
			return TRUE;
		}

		C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion) {
		if (!pFunctionTable || !interfaceVersion || *interfaceVersion != INTERFACE_VERSION) {
		if (interfaceVersion) *interfaceVersion = INTERFACE_VERSION;
		return FALSE;
		}
		memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
		pFunctionTable->pfnServerActivate = OnServerActivate_Post;
		return TRUE;
		}

		META_FUNCTIONS gMetaFunctionTable = {
		NULL,                 /* GetEntityAPI */
		NULL,                 /* GetEntityAPI_Post */
		GetEntityAPI2,        /* GetEntityAPI2 */
		GetEntityAPI2_Post,   /* GetEntityAPI2_Post */
		NULL, NULL,           /* GetNewDLLFunctions(_Post) */
		NULL, NULL            /* GetEngineFunctions(_Post) */
		};

		C_DLLEXPORT void GiveFnptrsToDll(enginefuncs_t *pengfuncs, globalvars_t *pGlobals) {
		memcpy(&g_engfuncs, pengfuncs, sizeof(enginefuncs_t));
		gpGlobals = pGlobals;
		}

		C_DLLEXPORT int Meta_Query(char *interfaceVersion, plugin_info_t **plinfo, mutil_funcs_t *pMetaUtilFuncs) {
	(void)interfaceVersion; (void)pMetaUtilFuncs;
	if (!plinfo) return FALSE;
	*plinfo = &Plugin_info;
	return TRUE;
}

		C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS *pFunctionTable,
			meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs) {
		if (!pMGlobals || !pFunctionTable) return FALSE;

		gpMetaGlobals = pMGlobals;
		gpGamedllFuncs = pGamedllFuncs;
		memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));

		bool found = LoadConfig(false);

		memset(&g_State, 0, sizeof(g_State));
		memset(g_Slot, 0, sizeof(g_Slot));
		g_PendLen = 0;

		const char *term = getenv("TERM");
		g_State.term_usable = (isatty(STDOUT_FILENO) && term && strcmp(term, "dumb") != 0) ? 1 : 0;
		if (g_State.term_usable) {
		TermOpen();
		if (g_TermFd < 0) g_State.term_usable = 0;
		}

		long ps = sysconf(_SC_PAGESIZE);
		g_PageSize = (ps > 0) ? ps : 4096;
		g_StatmFd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
		ReadRamUsage();

		g_State.applied_ticrate = -1;
		g_State.last_player_time = -1e9;
		g_State.fps_window_start = NowSec();
		g_State.last_trim = NowSec();
		g_State.last_resync = NowSec();

		g_State.tic_cvar = g_engfuncs.pfnCVarGetPointer("sys_ticrate");
		if (g_State.tic_cvar) {
		g_State.orig_ticrate = g_State.tic_cvar->value;
		g_State.orig_saved = 1;
		}

		g_engfuncs.pfnRegisterCVar(&g_RmStatus);
				RescanPlayers(); /* correct when loaded in the middle of a game */
		g_State.was_active = IsActive(NowSec()) ? 1 : 0;
		g_State.initialized = 1;
				RegisterCommands();

		static int atexit_done = 0;
		if (!atexit_done) {
		atexit(AtExitReset);
		atexit_done = 1;
		}

		ConPrint(RM_TAG " v3.1 loaded (%s): idle=%d, active=%d, target_fps=%.0f, realtime=%d\n",
		 found ? g_CfgPath : "no ini, defaults", IdleTic(), ActiveTic(),
		 g_Cfg.target_fps, g_Cfg.realtime);
		return TRUE;
			}

			C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason) {
				g_State.initialized = 0;
				SetRealtime(false);
				if (g_Pinned && g_OrigSaved) {
				cpu_set_t base = g_AffApplied ? g_TargetSet : g_OrigSet;
				sched_setaffinity(0, sizeof(base), &base);
				g_Pinned = 0;
				}
				if (g_State.orig_saved) {
				char buf[32];
				snprintf(buf, sizeof(buf), "%g", g_State.orig_ticrate);
				g_engfuncs.pfnCVarSetString("sys_ticrate", buf);
			}
				TermReset();
				TermClose();
				if (g_StatmFd >= 0) {
				close(g_StatmFd);
				g_StatmFd = -1;
				}
				return TRUE;
			}
