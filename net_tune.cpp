#include <extdll.h>
#include <meta_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/stat.h>

/*
 * NetTune 1.5 - Connection quality tuner for CS 1.6 / ReHLDS / Metamod-R
 * Author: FaTaL-[Hit] - https://t.me/Neo_Norm
 *
 * Changelog vs 1.4 (compile errors on the ReHLDS 3.15 SDK you are using):
 *   Your rehlds_api.h does NOT declare IRehldsHook_SV_EmitPings and
 *   IRehldsHookchains has no SV_EmitPings() member, so every reference to
 *   them failed to compile. v1.5 keeps ALL logic intact but puts the whole
 *   scoreboard-ping-smoothing path (hook + message writer + API handles)
 *   behind the NT_HAVE_REHLDS feature flag:
 *     - default (no flag): section excluded -> compiles cleanly on your
 *       SDK; smoothing reports "not compiled" in the console at load.
 *       Every other feature (auto-tune tiers, cvar enforcement, client
 *       push, tier cache, kick policy, hot reload) works exactly as before.
 *     - -DNT_HAVE_REHLDS=1: full v1.4 behaviour, requires SDK headers that
 *       expose the SV_EmitPings hookchain (newer ReHLDS dev builds).
 */

#ifndef NT_HAVE_REHLDS
#define NT_HAVE_REHLDS 0
#endif

plugin_info_t Plugin_info = {
	META_INTERFACE_VERSION,
	"NetTune",
	"1.5",
	__DATE__,
	"FaTaL-[Hit]",
	"https://t.me/Neo_Norm",
	"NT",
	PT_STARTUP,
	PT_ANYTIME
};

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals = NULL;
meta_globals_t *gpMetaGlobals = NULL;
gamedll_funcs_t *gpGamedllFuncs = NULL;

#define NT_TAG "[NT]"
#define NT_MAX_CLIENTS 32
#define NT_WINDOW 20          /* samples kept per player (1 per second) */
#define NT_FAST 5             /* short window used to detect a worsening line */
#define NT_MIN_SAMPLES 10     /* minimum samples before a decision */
#define NT_SETTLE_SEC 4.0     /* ignore the first seconds after join */
#define NT_TIERS 4
#define NT_CACHE 64
#define NT_SVC_PINGS 17       /* svc_pings message id */

struct NtSettings {
	/* server cvars, negative = do not touch */
	int   enforce_cvars;
	float srv_maxrate;
	float srv_minrate;
	float srv_maxupdaterate;
	float srv_minupdaterate;

	/* client push */
	int   client_enforce;
	int   cl_rate;
	int   force_cl_lw;
	int   force_cl_lc;
	float apply_delay;

	/* auto tune */
	int   auto_tune;
	int   start_tier;
	float learn_seconds;
	float worsen_seconds;
	float improve_seconds;
	float min_change_interval;
	float apply_defer_max;
	float emergency_loss;
	float remember_seconds;

	/* tier thresholds (tier 0..2; anything worse is tier 3) */
	float t_max_loss[NT_TIERS - 1];
	float t_max_jitter[NT_TIERS - 1];
	float t_max_spike[NT_TIERS - 1];

	/* tier profiles */
	int   t_upd[NT_TIERS];
	int   t_cmd[NT_TIERS];
	float t_ratio[NT_TIERS];

	/* scoreboard ping smoothing (display only, real value is never lowered) */
	int   smooth_ping;
	float smooth_seconds;
	float smooth_deadband;
	float smooth_max_dev;

	/* optional kick for permanently high ping (0 = off) */
	int   max_ping;
	float max_ping_seconds;
	float ping_grace;

	int   debug;
} g_Cfg;

struct Slot {
	int    active;
	int    pending_push;
	double apply_at;
	double join_time;
	char   auth[40];
	int    has_auth;

	int    ping_hist[NT_WINDOW];
	int    loss_hist[NT_WINDOW];
	int    n;
	int    pos;

	float  avg_ping, avg_loss, jitter, spike;
	float  f_loss, f_jitter, f_spike;

	int    tier;
	int    learned;
	int    pending_tier;       /* -1 = none */
	double pending_since;
	double worse_since;
	double better_since;
	double last_change;

	int    ping;
	int    loss;
	double hp_since;

	/* scoreboard display */
	float  disp_ping;
	int    disp_init;
	double disp_time;
};

struct CacheEnt {
	char   id[40];
	int    tier;
	double t;
};

static Slot     g_Slot[NT_MAX_CLIENTS + 1];
static CacheEnt g_Cache[NT_CACHE];
static int      g_On = 0;
static double   g_LastCfgCheck = 0.0;
static double   g_LastEnforce = 0.0;
static double   g_LastPoll = 0.0;
static double   g_LastDebug = 0.0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static double NowSec() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
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

/* ------------------------------------------------------------------ */
/* Config (optional file, auto reloaded)                               */
/* ------------------------------------------------------------------ */

static const char *g_Paths[] = {
	"addons/net_tune/net_tune.ini",
	"cstrike/addons/net_tune/net_tune.ini",
	"../cstrike/addons/net_tune/net_tune.ini",
	"net_tune.ini"
};
static char   g_CfgPath[256] = "";
static time_t g_CfgMtime = 0;

/* keys like t0_updaterate, t2_max_loss ... */
static void ParseTierKey(const char *key, const char *val) {
	if (key[0] != 't' || key[1] < '0' || key[1] > '3' || key[2] != '_') return;
	int t = key[1] - '0';
	const char *sfx = key + 3;
	if      (strcmp(sfx, "updaterate") == 0)   g_Cfg.t_upd[t] = atoi(val);
	else if (strcmp(sfx, "cmdrate") == 0)      g_Cfg.t_cmd[t] = atoi(val);
	else if (strcmp(sfx, "interp_ratio") == 0) g_Cfg.t_ratio[t] = (float)atof(val);
	else if (t < NT_TIERS - 1 && strcmp(sfx, "max_loss") == 0)   g_Cfg.t_max_loss[t] = (float)atof(val);
	else if (t < NT_TIERS - 1 && strcmp(sfx, "max_jitter") == 0) g_Cfg.t_max_jitter[t] = (float)atof(val);
	else if (t < NT_TIERS - 1 && strcmp(sfx, "max_spike") == 0)  g_Cfg.t_max_spike[t] = (float)atof(val);
}

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

		if      (strcmp(key, "enforce_cvars") == 0)          g_Cfg.enforce_cvars = atoi(val);
		else if (strcmp(key, "sv_maxrate") == 0)             g_Cfg.srv_maxrate = (float)atof(val);
		else if (strcmp(key, "sv_minrate") == 0)             g_Cfg.srv_minrate = (float)atof(val);
		else if (strcmp(key, "sv_maxupdaterate") == 0)       g_Cfg.srv_maxupdaterate = (float)atof(val);
		else if (strcmp(key, "sv_minupdaterate") == 0)       g_Cfg.srv_minupdaterate = (float)atof(val);
		else if (strcmp(key, "client_enforce") == 0)         g_Cfg.client_enforce = atoi(val);
		else if (strcmp(key, "cl_rate") == 0)                g_Cfg.cl_rate = atoi(val);
		else if (strcmp(key, "force_cl_lw") == 0)            g_Cfg.force_cl_lw = atoi(val);
		else if (strcmp(key, "force_cl_lc") == 0)            g_Cfg.force_cl_lc = atoi(val);
		else if (strcmp(key, "apply_delay") == 0)            g_Cfg.apply_delay = (float)atof(val);
		else if (strcmp(key, "auto_tune") == 0)              g_Cfg.auto_tune = atoi(val);
		else if (strcmp(key, "start_tier") == 0)             g_Cfg.start_tier = atoi(val);
		else if (strcmp(key, "learn_seconds") == 0)          g_Cfg.learn_seconds = (float)atof(val);
		else if (strcmp(key, "worsen_seconds") == 0)         g_Cfg.worsen_seconds = (float)atof(val);
		else if (strcmp(key, "improve_seconds") == 0)        g_Cfg.improve_seconds = (float)atof(val);
		else if (strcmp(key, "min_change_interval") == 0)    g_Cfg.min_change_interval = (float)atof(val);
		else if (strcmp(key, "apply_defer_max") == 0)        g_Cfg.apply_defer_max = (float)atof(val);
		else if (strcmp(key, "emergency_loss") == 0)         g_Cfg.emergency_loss = (float)atof(val);
		else if (strcmp(key, "remember_seconds") == 0)       g_Cfg.remember_seconds = (float)atof(val);
		else if (strcmp(key, "smooth_ping") == 0)            g_Cfg.smooth_ping = atoi(val);
		else if (strcmp(key, "smooth_seconds") == 0)         g_Cfg.smooth_seconds = (float)atof(val);
		else if (strcmp(key, "smooth_deadband") == 0)        g_Cfg.smooth_deadband = (float)atof(val);
		else if (strcmp(key, "smooth_max_dev") == 0)         g_Cfg.smooth_max_dev = (float)atof(val);
		else if (strcmp(key, "max_ping") == 0)               g_Cfg.max_ping = atoi(val);
		else if (strcmp(key, "max_ping_seconds") == 0)       g_Cfg.max_ping_seconds = (float)atof(val);
		else if (strcmp(key, "ping_grace") == 0)             g_Cfg.ping_grace = (float)atof(val);
		else if (strcmp(key, "debug") == 0)                  g_Cfg.debug = atoi(val);
		else ParseTierKey(key, val);
	}
	fclose(f);

	strncpy(g_CfgPath, path, sizeof(g_CfgPath) - 1);
	g_CfgPath[sizeof(g_CfgPath) - 1] = 0;
	struct stat st;
	g_CfgMtime = (stat(path, &st) == 0) ? st.st_mtime : 0;
	return 1;
}

static void SetDefaults() {
	g_Cfg.enforce_cvars = 1;
	g_Cfg.srv_maxrate = 100000.0f;
	g_Cfg.srv_minrate = 25000.0f;
	g_Cfg.srv_maxupdaterate = 101.0f;
	g_Cfg.srv_minupdaterate = 30.0f;

	g_Cfg.client_enforce = 1;
	g_Cfg.cl_rate = 100000;
	g_Cfg.force_cl_lw = 1;
	g_Cfg.force_cl_lc = 1;
	g_Cfg.apply_delay = 3.0f;

	g_Cfg.auto_tune = 1;
	g_Cfg.start_tier = 0;
	g_Cfg.learn_seconds = 20.0f;
	g_Cfg.worsen_seconds = 6.0f;
	g_Cfg.improve_seconds = 45.0f;
	g_Cfg.min_change_interval = 20.0f;
	g_Cfg.apply_defer_max = 15.0f;
	g_Cfg.emergency_loss = 15.0f;
	g_Cfg.remember_seconds = 1800.0f;

	const float ml[NT_TIERS - 1] = { 0.5f, 2.0f, 5.0f };
	const float mj[NT_TIERS - 1] = { 5.0f, 12.0f, 25.0f };
	const float ms[NT_TIERS - 1] = { 25.0f, 50.0f, 90.0f };
	const int   up[NT_TIERS] = { 101, 101, 90, 70 };
	const int   cm[NT_TIERS] = { 101, 101, 101, 80 };
	const float ra[NT_TIERS] = { 1.0f, 1.5f, 2.0f, 2.5f };
	for (int i = 0; i < NT_TIERS - 1; i++) {
		g_Cfg.t_max_loss[i] = ml[i];
		g_Cfg.t_max_jitter[i] = mj[i];
		g_Cfg.t_max_spike[i] = ms[i];
	}
	for (int i = 0; i < NT_TIERS; i++) {
		g_Cfg.t_upd[i] = up[i];
		g_Cfg.t_cmd[i] = cm[i];
		g_Cfg.t_ratio[i] = ra[i];
	}

	g_Cfg.smooth_ping = 1;
	g_Cfg.smooth_seconds = 6.0f;
	g_Cfg.smooth_deadband = 3.0f;
	g_Cfg.smooth_max_dev = 8.0f;

	g_Cfg.max_ping = 0;
	g_Cfg.max_ping_seconds = 20.0f;
	g_Cfg.ping_grace = 30.0f;

	g_Cfg.debug = 0;
}

static void ClampConfig() {
	if (g_Cfg.srv_maxrate < 0.0f) g_Cfg.srv_maxrate = -1.0f;
	if (g_Cfg.srv_minrate < 0.0f) g_Cfg.srv_minrate = -1.0f;
	if (g_Cfg.srv_maxupdaterate < 0.0f) g_Cfg.srv_maxupdaterate = -1.0f;
	if (g_Cfg.srv_minupdaterate < 0.0f) g_Cfg.srv_minupdaterate = -1.0f;

	g_Cfg.cl_rate = clampi(g_Cfg.cl_rate, 5000, 1000000);
	g_Cfg.apply_delay = clampf(g_Cfg.apply_delay, 0.5f, 30.0f);

	g_Cfg.start_tier = clampi(g_Cfg.start_tier, 0, NT_TIERS - 1);
	g_Cfg.learn_seconds = clampf(g_Cfg.learn_seconds, 12.0f, 300.0f);
	g_Cfg.worsen_seconds = clampf(g_Cfg.worsen_seconds, 2.0f, 300.0f);
	g_Cfg.improve_seconds = clampf(g_Cfg.improve_seconds, 5.0f, 900.0f);
	g_Cfg.min_change_interval = clampf(g_Cfg.min_change_interval, 5.0f, 900.0f);
	g_Cfg.apply_defer_max = clampf(g_Cfg.apply_defer_max, 0.0f, 120.0f);
	g_Cfg.emergency_loss = clampf(g_Cfg.emergency_loss, 5.0f, 100.0f);
	g_Cfg.remember_seconds = clampf(g_Cfg.remember_seconds, 0.0f, 86400.0f);

	for (int i = 0; i < NT_TIERS - 1; i++) {
		g_Cfg.t_max_loss[i] = clampf(g_Cfg.t_max_loss[i], 0.0f, 100.0f);
		g_Cfg.t_max_jitter[i] = clampf(g_Cfg.t_max_jitter[i], 0.0f, 500.0f);
		g_Cfg.t_max_spike[i] = clampf(g_Cfg.t_max_spike[i], 0.0f, 1000.0f);
		if (i > 0) {   /* thresholds must grow with the tier */
			if (g_Cfg.t_max_loss[i] < g_Cfg.t_max_loss[i - 1]) g_Cfg.t_max_loss[i] = g_Cfg.t_max_loss[i - 1];
			if (g_Cfg.t_max_jitter[i] < g_Cfg.t_max_jitter[i - 1]) g_Cfg.t_max_jitter[i] = g_Cfg.t_max_jitter[i - 1];
			if (g_Cfg.t_max_spike[i] < g_Cfg.t_max_spike[i - 1]) g_Cfg.t_max_spike[i] = g_Cfg.t_max_spike[i - 1];
		}
	}
	for (int i = 0; i < NT_TIERS; i++) {
		g_Cfg.t_upd[i] = clampi(g_Cfg.t_upd[i], 10, 200);
		g_Cfg.t_cmd[i] = clampi(g_Cfg.t_cmd[i], 10, 200);
		g_Cfg.t_ratio[i] = clampf(g_Cfg.t_ratio[i], 1.0f, 5.0f);
	}

	g_Cfg.smooth_seconds = clampf(g_Cfg.smooth_seconds, 1.0f, 60.0f);
	g_Cfg.smooth_deadband = clampf(g_Cfg.smooth_deadband, 0.0f, 20.0f);
	g_Cfg.smooth_max_dev = clampf(g_Cfg.smooth_max_dev, 1.0f, 50.0f);

	if (g_Cfg.max_ping < 0) g_Cfg.max_ping = 0;
	g_Cfg.max_ping_seconds = clampf(g_Cfg.max_ping_seconds, 5.0f, 600.0f);
	g_Cfg.ping_grace = clampf(g_Cfg.ping_grace, 5.0f, 600.0f);
}

/* returns true if a file was read. A failed reload restores the full old state. */
static bool LoadConfig(bool reload) {
	NtSettings oldCfg = g_Cfg;
	char oldPath[256];
	time_t oldMtime = g_CfgMtime;
	memcpy(oldPath, g_CfgPath, sizeof(oldPath));

	SetDefaults();

	bool ok = false;
	if (reload && oldPath[0]) {
		ok = try_load_config(oldPath) != 0;
	} else {
		for (size_t i = 0; i < sizeof(g_Paths) / sizeof(g_Paths[0]); i++) {
			if (try_load_config(g_Paths[i])) { ok = true; break; }
		}
	}

	if (!ok) {
		if (reload) {
			g_Cfg = oldCfg;
			memcpy(g_CfgPath, oldPath, sizeof(g_CfgPath));
			g_CfgMtime = oldMtime;
		} else {
			ClampConfig();
		}
		return false;
	}
	ClampConfig();
	return true;
}

/* ------------------------------------------------------------------ */
/* Server cvars                                                        */
/* ------------------------------------------------------------------ */

static const char *kCvarNames[4] = {
	"sv_maxrate", "sv_minrate", "sv_maxupdaterate", "sv_minupdaterate"
};
static cvar_t *g_CvPtr[4];
static float   g_CvOrig[4];
static int     g_CvSaved[4];
static int     g_CvMissingShown[4];

static float WantOf(int i) {
	switch (i) {
		case 0: return g_Cfg.srv_maxrate;
		case 1: return g_Cfg.srv_minrate;
		case 2: return g_Cfg.srv_maxupdaterate;
		default: return g_Cfg.srv_minupdaterate;
	}
}

static void ApplyCvars() {
	for (int i = 0; i < 4; i++) {
		if (!g_CvPtr[i]) g_CvPtr[i] = g_engfuncs.pfnCVarGetPointer(kCvarNames[i]);
		cvar_t *c = g_CvPtr[i];
		float want = g_Cfg.enforce_cvars ? WantOf(i) : -1.0f;

		if (!c) {
			if (want >= 0.0f && !g_CvMissingShown[i]) {
				g_CvMissingShown[i] = 1;
				ConPrint(NT_TAG " cvar %s not found on this server, skipped.\n", kCvarNames[i]);
			}
			continue;
		}
		if (want < 0.0f) {
			if (g_CvSaved[i]) {
				if (fabsf(c->value - g_CvOrig[i]) > 0.0001f)
					g_engfuncs.pfnCVarSetFloat(kCvarNames[i], g_CvOrig[i]);
				g_CvSaved[i] = 0;
			}
			continue;
		}
		if (!g_CvSaved[i]) {
			g_CvOrig[i] = c->value;
			g_CvSaved[i] = 1;
		}
		if (fabsf(c->value - want) > 0.0001f)
			g_engfuncs.pfnCVarSetFloat(kCvarNames[i], want);
	}
}

static void RestoreCvars() {
	for (int i = 0; i < 4; i++) {
		if (g_CvSaved[i] && g_CvPtr[i]) {
			g_engfuncs.pfnCVarSetFloat(kCvarNames[i], g_CvOrig[i]);
			g_CvSaved[i] = 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Tier memory (survives map changes and reconnects)                   */
/* ------------------------------------------------------------------ */

static bool GetAuthKey(edict_t *e, char *out, size_t cap) {
	const char *a = g_engfuncs.pfnGetPlayerAuthId(e);
	if (!a || !a[0]) return false;
	if (strstr(a, "LAN") || strstr(a, "PENDING") || strstr(a, "UNKNOWN") ||
		strstr(a, "BOT") || strstr(a, "HLTV")) return false;
	strncpy(out, a, cap - 1);
	out[cap - 1] = 0;
	return true;
}

static void CacheStore(const Slot &s, int tier, double now) {
	if (!s.has_auth || g_Cfg.remember_seconds <= 0.0f) return;
	int free_i = -1, oldest = 0;
	for (int i = 0; i < NT_CACHE; i++) {
		if (g_Cache[i].id[0] && strcmp(g_Cache[i].id, s.auth) == 0) {
			g_Cache[i].tier = tier;
			g_Cache[i].t = now;
			return;
		}
		if (!g_Cache[i].id[0] && free_i < 0) free_i = i;
		if (g_Cache[i].t < g_Cache[oldest].t) oldest = i;
	}
	int i = (free_i >= 0) ? free_i : oldest;
	strncpy(g_Cache[i].id, s.auth, sizeof(g_Cache[i].id) - 1);
	g_Cache[i].id[sizeof(g_Cache[i].id) - 1] = 0;
	g_Cache[i].tier = tier;
	g_Cache[i].t = now;
}

static bool CacheLookup(const Slot &s, double now, int *tier) {
	if (!s.has_auth || g_Cfg.remember_seconds <= 0.0f) return false;
	for (int i = 0; i < NT_CACHE; i++) {
		if (g_Cache[i].id[0] && strcmp(g_Cache[i].id, s.auth) == 0) {
			if (now - g_Cache[i].t > (double)g_Cfg.remember_seconds) return false;
			*tier = clampi(g_Cache[i].tier, 0, NT_TIERS - 1);
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Client settings                                                     */
/* ------------------------------------------------------------------ */

/* sends rate / updaterate / cmdrate / interp of the player's CURRENT tier.
 *  full = also send cl_lw / cl_lc (only on join and on config reload) */
static void PushClientSettings(int idx, edict_t *ent, bool full) {
	if (!ent) return;
	int t = clampi(g_Slot[idx].tier, 0, NT_TIERS - 1);
	int upd = g_Cfg.t_upd[t];
	int cmdr = g_Cfg.t_cmd[t];
	float interp = g_Cfg.t_ratio[t] / (float)upd;

	char buf[220];
	int n = snprintf(buf, sizeof(buf), "rate %d;cl_updaterate %d;cl_cmdrate %d;ex_interp %.4f",
					 g_Cfg.cl_rate, upd, cmdr, interp);
	if (full && g_Cfg.force_cl_lc && n > 0 && n < (int)sizeof(buf) - 12)
		n += snprintf(buf + n, sizeof(buf) - n, ";cl_lc 1");
	if (full && g_Cfg.force_cl_lw && n > 0 && n < (int)sizeof(buf) - 12)
		n += snprintf(buf + n, sizeof(buf) - n, ";cl_lw 1");
	if (n > 0 && n < (int)sizeof(buf) - 2) { buf[n++] = '\n'; buf[n] = 0; }

	static char fmt[] = "%s";
	g_engfuncs.pfnClientCommand(ent, fmt, buf);
}

static bool IsRealPlayer(edict_t *e) {
	if (!e || e->free) return false;
	int fl = e->v.flags;
	if (!(fl & FL_CLIENT)) return false;
	if (fl & FL_FAKECLIENT) return false;   /* bots */
	if (fl & FL_PROXY) return false;        /* HLTV */
	return true;
}

static void QueueAllClients(double now) {
	for (int i = 1; i <= NT_MAX_CLIENTS; i++) {
		if (!g_Slot[i].active) continue;
		g_Slot[i].pending_push = 1;
		g_Slot[i].apply_at = now;
	}
}

/* ------------------------------------------------------------------ */
/* Connection quality                                                  */
/* ------------------------------------------------------------------ */

static void AddSample(Slot &s, int ping, int loss) {
	s.ping_hist[s.pos] = ping;
	s.loss_hist[s.pos] = loss;
	s.pos = (s.pos + 1) % NT_WINDOW;
	if (s.n < NT_WINDOW) s.n++;
}

/* metrics over the last m samples */
static void WindowMetrics(const Slot &s, int m, float *loss, float *jitter, float *spike, float *ping) {
	if (m > s.n) m = s.n;
	if (m <= 0) { *loss = *jitter = *spike = *ping = 0.0f; return; }
	float sp = 0.0f, sl = 0.0f;
	int mx = 0;
	for (int k = 0; k < m; k++) {
		int idx = (s.pos - 1 - k + NT_WINDOW * 2) % NT_WINDOW;
		sp += (float)s.ping_hist[idx];
		sl += (float)s.loss_hist[idx];
		if (s.ping_hist[idx] > mx) mx = s.ping_hist[idx];
	}
	float avg = sp / (float)m;
	float dev = 0.0f;
	for (int k = 0; k < m; k++) {
		int idx = (s.pos - 1 - k + NT_WINDOW * 2) % NT_WINDOW;
		dev += fabsf((float)s.ping_hist[idx] - avg);
	}
	*ping = avg;
	*loss = sl / (float)m;
	*jitter = dev / (float)m;
	*spike = (float)mx - avg;
}

static void ComputeMetrics(Slot &s) {
	WindowMetrics(s, NT_WINDOW, &s.avg_loss, &s.jitter, &s.spike, &s.avg_ping);
	float p;
	WindowMetrics(s, NT_FAST, &s.f_loss, &s.f_jitter, &s.f_spike, &p);
}

static int TierFor(float loss, float jitter, float spike) {
	for (int t = 0; t < NT_TIERS - 1; t++) {
		if (loss <= g_Cfg.t_max_loss[t] && jitter <= g_Cfg.t_max_jitter[t] && spike <= g_Cfg.t_max_spike[t])
			return t;
	}
	return NT_TIERS - 1;
}

static void QueueTier(Slot &s, int tier, double now, bool urgent) {
	s.pending_tier = tier;
	s.pending_since = urgent ? now - (double)g_Cfg.apply_defer_max : now;
	s.worse_since = 0.0;
	s.better_since = 0.0;
}

/* worse: judged on the SHORT window (fast reaction)
 *  better: judged on the LONG window, one step at a time (no flapping) */
static void EvaluateTier(Slot &s, double now) {
	if (!g_Cfg.auto_tune) return;
	if (s.n < NT_MIN_SAMPLES) return;
	if (now - s.join_time < (double)g_Cfg.learn_seconds) return;

	int want_fast = TierFor(s.f_loss, s.f_jitter, s.f_spike);
	int want_slow = TierFor(s.avg_loss, s.jitter, s.spike);

	/* first assessment right after joining */
	if (!s.learned) {
		s.learned = 1;
		s.last_change = now;
		int want = (want_fast > want_slow) ? want_fast : want_slow;
		if (want != s.tier) QueueTier(s, want, now, true);
		else CacheStore(s, s.tier, now);
		return;
	}

	/* a change is already waiting for a safe moment: cancel it if the line recovered */
	if (s.pending_tier >= 0) {
		if (s.pending_tier > s.tier && want_fast <= s.tier) s.pending_tier = -1;
		else if (s.pending_tier < s.tier && want_slow >= s.tier) s.pending_tier = -1;
		return;
	}

	bool can_change = (now - s.last_change) >= (double)g_Cfg.min_change_interval;

	/* emergency: heavy loss right now, do not wait for a safe moment */
	if (s.f_loss >= g_Cfg.emergency_loss && s.tier < NT_TIERS - 1) {
		QueueTier(s, NT_TIERS - 1, now, true);
		return;
	}

	if (want_fast > s.tier) {
		s.better_since = 0.0;
		if (s.worse_since == 0.0) s.worse_since = now;
		if (can_change && now - s.worse_since >= (double)g_Cfg.worsen_seconds)
			QueueTier(s, want_fast, now, false);
	} else {
		s.worse_since = 0.0;
		if (want_slow < s.tier) {
			if (s.better_since == 0.0) s.better_since = now;
			if (can_change && now - s.better_since >= (double)g_Cfg.improve_seconds)
				QueueTier(s, s.tier - 1, now, false);
		} else {
			s.better_since = 0.0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Scoreboard ping smoothing (display only)                            */
/* ------------------------------------------------------------------ */

/* Exponential moving average of the REAL ping, with a dead band so tiny changes
 *  do not move the number. Never biased up or down, and never further than
 *  smooth_max_dev away from the real value. */
static void UpdateDisplayPing(Slot &s, int ping, double now) {
	if (ping <= 0) return;
	if (!s.disp_init) {
		s.disp_ping = (float)ping;
		s.disp_init = 1;
		s.disp_time = now;
		return;
	}
	double dt = now - s.disp_time;
	s.disp_time = now;
	if (dt < 0.0) dt = 0.0;
	if (dt > 5.0) dt = 5.0;

	float a = 1.0f - expf(-(float)dt / g_Cfg.smooth_seconds);
	float diff = (float)ping - s.disp_ping;
	if (fabsf(diff) > g_Cfg.smooth_deadband) s.disp_ping += diff * a;

	float lo = (float)ping - g_Cfg.smooth_max_dev;
	float hi = (float)ping + g_Cfg.smooth_max_dev;
	if (s.disp_ping < lo) s.disp_ping = lo;
	if (s.disp_ping > hi) s.disp_ping = hi;
	if (s.disp_ping < 0.0f) s.disp_ping = 0.0f;
}

static void PollClients(double now) {
	int maxc = gpGlobals ? gpGlobals->maxClients : 0;
	if (maxc > NT_MAX_CLIENTS) maxc = NT_MAX_CLIENTS;

	for (int i = 1; i <= maxc; i++) {
		Slot &s = g_Slot[i];
		if (!s.active) continue;

		edict_t *e = INDEXENT(i);
		if (!IsRealPlayer(e)) {
			if (!e || e->free || !(e->v.flags & FL_CLIENT)) memset(&s, 0, sizeof(Slot));
			continue;
		}

		if (g_Cfg.client_enforce && s.pending_push && now >= s.apply_at) {
			s.pending_push = 0;
			PushClientSettings(i, e, true);
		}

		int ping = 0, loss = 0;
		g_engfuncs.pfnGetPlayerStats(e, &ping, &loss);
		s.ping = ping;
		s.loss = loss;

		UpdateDisplayPing(s, ping, now);

		/* measure only after the connection has settled */
		if (now - s.join_time >= NT_SETTLE_SEC && ping > 0) {
			AddSample(s, ping, loss);
			ComputeMetrics(s);
		}

		if (g_Cfg.client_enforce) {
			EvaluateTier(s, now);

			/* apply a waiting change when the player is dead/spectating,
			 *		   or when it has waited long enough (urgent changes do not wait) */
			if (s.pending_tier >= 0) {
				bool safe = (e->v.deadflag != DEAD_NO);
				if (safe || now - s.pending_since >= (double)g_Cfg.apply_defer_max) {
					int old = s.tier;
					s.tier = s.pending_tier;
					s.pending_tier = -1;
					s.last_change = now;
					PushClientSettings(i, e, false);
					CacheStore(s, s.tier, now);
					if (g_Cfg.debug)
						ConPrint(NT_TAG " #%d tier %d -> %d (loss=%.1f%% jitter=%.1fms spike=%.0fms ping=%.0f)\n",
								 i, old, s.tier, s.avg_loss, s.jitter, s.spike, s.avg_ping);
				}
			}
		}

		/* ---- optional kick for permanently high ping ---- */
		if (g_Cfg.max_ping > 0 && now - s.join_time >= (double)g_Cfg.ping_grace) {
			if (ping > g_Cfg.max_ping) {
				if (s.hp_since == 0.0) s.hp_since = now;
				if (now - s.hp_since >= (double)g_Cfg.max_ping_seconds) {
					char cmd[160];
					snprintf(cmd, sizeof(cmd), "kick #%d \"High ping (%d ms, limit %d)\"\n",
							 g_engfuncs.pfnGetPlayerUserId(e), ping, g_Cfg.max_ping);
					g_engfuncs.pfnServerCommand(cmd);
					ConPrint(NT_TAG " kicked #%d: ping %d > %d for %.0fs\n",
							 i, ping, g_Cfg.max_ping, g_Cfg.max_ping_seconds);
					s.hp_since = 0.0;
				}
			} else {
				s.hp_since = 0.0;
			}
		}
	}
}

static void DebugSummary(double now) {
	if (now - g_LastDebug < 10.0) return;
	g_LastDebug = now;

	char buf[700];
	size_t n = 0;
	int count = 0;
	n += snprintf(buf + n, sizeof(buf) - n, NT_TAG " players:");
	for (int i = 1; i <= NT_MAX_CLIENTS && n < sizeof(buf) - 120; i++) {
		if (!g_Slot[i].active) continue;
		count++;
		n += snprintf(buf + n, sizeof(buf) - n, " #%d(T%d p=%.0f shown=%.0f l=%.1f j=%.1f s=%.0f)",
					  i, g_Slot[i].tier, g_Slot[i].avg_ping, g_Slot[i].disp_ping,
				g_Slot[i].avg_loss, g_Slot[i].jitter, g_Slot[i].spike);
	}
	if (count == 0) return;
	snprintf(buf + n, sizeof(buf) - n, "\n");
	g_engfuncs.pfnServerPrint(buf);
}

/* ------------------------------------------------------------------ */
/* ReHLDS: scoreboard ping message (SV_EmitPings hook)                 */
/* The installed ReHLDS 3.15 headers do not declare the SV_EmitPings   */
/* hookchain, so this whole path is guarded by NT_HAVE_REHLDS:         */
/*   -DNT_HAVE_REHLDS=1  -> smoothing compiled (needs SDK headers that */
/*                          expose IRehldsHook_SV_EmitPings + the       */
/*                          SV_EmitPings() member on IRehldsHookchains) */
/*   no flag / =0        -> section excluded, everything else builds   */
/* Nothing else in NetTune needs ReHLDS; on your current SDK just drop */
/* the flag and the plugin compiles with smoothing reported as "not    */
/* compiled". When you later update the SDK headers, add the flag back.*/
/* ------------------------------------------------------------------ */

#if NT_HAVE_REHLDS
#include <rehlds_api.h>

/* ReHLDS API handles (declared here: the types exist only when the header is included) */
static IRehldsApi              *g_RehldsApi = NULL;
static const RehldsFuncs_t     *g_RehldsFuncs = NULL;
static IRehldsHookchains       *g_RehldsHookchains = NULL;
static IRehldsServerStatic     *g_RehldsSvs = NULL;
static int                      g_HookRegistered = 0;
static int                      g_PingFmt = 0;   /* 0 unverified, 1 verified, -1 failed */

typedef void *(*NtCreateIface)(const char *, int *);

static bool RehldsInit() {
	const char *names[] = { "engine_i486.so", "./engine_i486.so" };
	void *h = NULL;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !h; i++)
		h = dlopen(names[i], RTLD_NOW | RTLD_NOLOAD);
	if (!h) return false;

	NtCreateIface fn = (NtCreateIface)dlsym(h, "CreateInterface");
	dlclose(h);   /* the engine stays loaded, this only drops our extra reference */
	if (!fn) return false;

	int ret = 0;
	g_RehldsApi = (IRehldsApi *)fn(VREHLDS_HLDS_API_VERSION, &ret);
	if (!g_RehldsApi) return false;

	if (g_RehldsApi->GetMajorVersion() != REHLDS_API_VERSION_MAJOR) return false;

	g_RehldsFuncs = g_RehldsApi->GetFuncs();
	g_RehldsHookchains = g_RehldsApi->GetHookchains();
	g_RehldsSvs = g_RehldsApi->GetServerStatic();
	return g_RehldsFuncs && g_RehldsHookchains && g_RehldsSvs;
}

/* same layout the engine writes: svc_pings, then per active client
 *  1 bit (present) + 5 bits (slot) + 12 bits (ping) + 7 bits (loss), then a 0 bit */
static void WritePingsMessage(sizebuf_t *sb, bool smooth) {
	g_RehldsFuncs->MSG_WriteByte(sb, NT_SVC_PINGS);
	g_RehldsFuncs->MSG_StartBitWriting(sb);

	int maxc = gpGlobals ? gpGlobals->maxClients : 0;
	if (maxc > NT_MAX_CLIENTS) maxc = NT_MAX_CLIENTS;

	for (int i = 0; i < maxc; i++) {
		IGameClient *c = g_RehldsSvs->GetClient(i);
		if (!c || !c->IsActive()) continue;
		edict_t *e = c->GetEdict();
		if (!e) continue;

		int ping = 0, loss = 0;
		g_engfuncs.pfnGetPlayerStats(e, &ping, &loss);

		if (smooth) {
			const Slot &s = g_Slot[i + 1];
			if (s.active && s.disp_init) ping = (int)(s.disp_ping + 0.5f);
		}
		ping = clampi(ping, 0, 4095);
		loss = clampi(loss, 0, 127);

		g_RehldsFuncs->MSG_WriteBits(1, 1);
		g_RehldsFuncs->MSG_WriteBits((uint32)i, 5);
		g_RehldsFuncs->MSG_WriteBits((uint32)ping, 12);
		g_RehldsFuncs->MSG_WriteBits((uint32)loss, 7);
	}
	g_RehldsFuncs->MSG_WriteBits(0, 1);
	g_RehldsFuncs->MSG_EndBitWriting(sb);
}

static void NT_SV_EmitPings(IRehldsHook_SV_EmitPings *chain, IGameClient *cl, sizebuf_t *msg) {
	if (!g_On || !g_Cfg.smooth_ping || g_PingFmt < 0 || !g_RehldsFuncs || !g_RehldsSvs) {
		chain->callNext(cl, msg);
		return;
	}

	if (g_PingFmt == 0) {
		/* one-time self check: build the engine's message and ours (with REAL values)
		 *	   into scratch buffers and compare them byte by byte */
		static unsigned char bufA[1024], bufB[1024];
		sizebuf_t a, b;
		memset(&a, 0, sizeof(a));
		memset(&b, 0, sizeof(b));
		a.buffername = "nt_a"; a.data = bufA; a.maxsize = (int)sizeof(bufA);
		b.buffername = "nt_b"; b.data = bufB; b.maxsize = (int)sizeof(bufB);

		chain->callNext(cl, &a);
		WritePingsMessage(&b, false);

		bool same = (a.cursize > 0 && a.cursize == b.cursize && a.flags == 0 && b.flags == 0 &&
		memcmp(bufA, bufB, (size_t)a.cursize) == 0);
		if (same) {
			g_PingFmt = 1;
			ConPrint(NT_TAG " ping display: message format verified, smoothing active\n");
			WritePingsMessage(msg, true);
		} else {
			g_PingFmt = -1;
			ConPrint(NT_TAG " ping display: message format differs from the engine (engine %d bytes, ours %d), "
			"smoothing disabled, scoreboard untouched\n", a.cursize, b.cursize);
			chain->callNext(cl, msg);
		}
		return;
	}

	WritePingsMessage(msg, true);
}

#endif /* NT_HAVE_REHLDS */

/* ------------------------------------------------------------------ */
/* Config hot reload                                                   */
/* ------------------------------------------------------------------ */

static void CheckConfigReload(double now) {
	if (now - g_LastCfgCheck < 2.0) return;
	g_LastCfgCheck = now;

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

	if (LoadConfig(true)) {
		ApplyCvars();
		/* values of a tier may have changed: push once to everyone */
		for (int i = 1; i <= NT_MAX_CLIENTS; i++) {
			if (!g_Slot[i].active) continue;
			if (!g_Cfg.auto_tune) g_Slot[i].tier = g_Cfg.start_tier;
			g_Slot[i].pending_tier = -1;
		}
		QueueAllClients(now);
		ConPrint(NT_TAG " config reloaded: auto_tune=%d start_tier=%d rate=%d smooth_ping=%d\n",
				 g_Cfg.auto_tune, g_Cfg.start_tier, g_Cfg.cl_rate, g_Cfg.smooth_ping);
	} else if (g_CfgPath[0]) {
		g_CfgMtime = st.st_mtime; /* stop retrying a file that fails to parse */
	}
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

static void OnStartFrame() {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_On) return;

	double now = NowSec();
	CheckConfigReload(now);

	if (g_Cfg.enforce_cvars && now - g_LastEnforce >= 2.0) {
		g_LastEnforce = now;
		ApplyCvars();
	}

	if (now - g_LastPoll >= 1.0) {
		g_LastPoll = now;
		PollClients(now);
		if (g_Cfg.debug) DebugSummary(now);
	}
}

static void OnClientPutInServer(edict_t *pEntity) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_On || !pEntity) return;

	int idx = g_engfuncs.pfnIndexOfEdict(pEntity);
	if (idx < 1 || idx > NT_MAX_CLIENTS) return;
	if (!IsRealPlayer(pEntity)) return;

	double now = NowSec();
	Slot &s = g_Slot[idx];
	memset(&s, 0, sizeof(Slot));
	s.active = 1;
	s.join_time = now;
	s.pending_push = 1;
	s.apply_at = now + (double)g_Cfg.apply_delay;
	s.tier = g_Cfg.start_tier;
	s.pending_tier = -1;
	s.has_auth = GetAuthKey(pEntity, s.auth, sizeof(s.auth)) ? 1 : 0;

	/* known player: start from the tier he had last time */
	int remembered;
	if (g_Cfg.auto_tune && CacheLookup(s, now, &remembered)) s.tier = remembered;
}

static void OnClientDisconnect(edict_t *pEntity) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_On || !pEntity) return;

	int idx = g_engfuncs.pfnIndexOfEdict(pEntity);
	if (idx < 1 || idx > NT_MAX_CLIENTS) return;
	if (g_Slot[idx].active && g_Slot[idx].learned) CacheStore(g_Slot[idx], g_Slot[idx].tier, NowSec());
	memset(&g_Slot[idx], 0, sizeof(Slot));
}

static void OnServerActivate_Post(edict_t *pEdictList, int edictCount, int clientMax) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_On) return;
	/* clients re-enter through ClientPutInServer on the new map; their tier is in the cache */
	double now = NowSec();
	for (int i = 1; i <= NT_MAX_CLIENTS; i++)
		if (g_Slot[i].active && g_Slot[i].learned) CacheStore(g_Slot[i], g_Slot[i].tier, now);
	memset(g_Slot, 0, sizeof(g_Slot));
	ApplyCvars();
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
	pFunctionTable->pfnClientPutInServer = OnClientPutInServer;
	pFunctionTable->pfnClientDisconnect = OnClientDisconnect;
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
NULL,                 /* pfnGetEntityAPI */
NULL,                 /* pfnGetEntityAPI_Post */
GetEntityAPI2,        /* pfnGetEntityAPI2 */
GetEntityAPI2_Post,   /* pfnGetEntityAPI2_Post */
NULL, NULL,           /* pfnGetNewDLLFunctions(_Post) */
NULL, NULL            /* pfnGetEngineFunctions(_Post) */
};

C_DLLEXPORT void GiveFnptrsToDll(enginefuncs_t *pengfuncs, globalvars_t *pGlobals) {
	memcpy(&g_engfuncs, pengfuncs, sizeof(enginefuncs_t));
	gpGlobals = pGlobals;
}

C_DLLEXPORT int Meta_Query(char *interfaceVersion, plugin_info_t **plinfo, mutil_funcs_t *pMetaUtilFuncs) {
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

	memset(g_Slot, 0, sizeof(g_Slot));
	memset(g_Cache, 0, sizeof(g_Cache));
	memset(g_CvPtr, 0, sizeof(g_CvPtr));
	memset(g_CvOrig, 0, sizeof(g_CvOrig));
	memset(g_CvSaved, 0, sizeof(g_CvSaved));
	memset(g_CvMissingShown, 0, sizeof(g_CvMissingShown));
	g_LastCfgCheck = NowSec();
	g_LastEnforce = 0.0;
	g_LastPoll = 0.0;
	g_LastDebug = 0.0;
#if NT_HAVE_REHLDS
	g_PingFmt = 0;
#endif

	ApplyCvars();

	/* ping smoothing needs the ReHLDS API; everything else works without it */
	const char *ping_state = "off";
#if NT_HAVE_REHLDS
	if (g_Cfg.smooth_ping) {
		if (RehldsInit()) {
			g_RehldsHookchains->SV_EmitPings()->registerHook(NT_SV_EmitPings);
			g_HookRegistered = 1;
			ping_state = "on (verified at first use)";
		} else {
			ping_state = "unavailable (ReHLDS API not found or too old)";
		}
	}
#else
	if (g_Cfg.smooth_ping)
		ping_state = "not compiled (SDK lacks SV_EmitPings; rebuild with -DNT_HAVE_REHLDS=1 + matching headers)";
#endif

	g_On = 1;

	ConPrint(NT_TAG " v1.5 loaded (%s): auto_tune=%d start_tier=%d rate=%d max_ping=%d ping_smoothing=%s\n",
			 found ? g_CfgPath : "no ini, defaults",
		  g_Cfg.auto_tune, g_Cfg.start_tier, g_Cfg.cl_rate, g_Cfg.max_ping, ping_state);
	return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason) {
	g_On = 0;
#if NT_HAVE_REHLDS
	if (g_HookRegistered && g_RehldsHookchains) {
		g_RehldsHookchains->SV_EmitPings()->unregisterHook(NT_SV_EmitPings);
		g_HookRegistered = 0;
	}
#endif
	RestoreCvars();
	return TRUE;
}
