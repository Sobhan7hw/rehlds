#include <extdll.h>
#include <meta_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>

/*
 * ShotFix 1.1 - Metamod plugin for CS 1.6 / REHLDS (built against Metamod-R + AMX Mod X headers).
 *
 * NOTE: this source uses C++ features (bool without stdbool, reference-free but
 *       relies on the C++ metamod headers and global constructors from extdll.h),
 *       so it must be compiled as C++ (e.g. as shotfix.cpp) with the HL1/Metamod
 *       include paths, exactly like other MM/AMXX plugins.
 */

/*
 * NOTE: metamod-r's plugin_info_t (metamod.h) has NO "unload" member — the struct is:
 *   int interfaceVer; char *name; char *version; char *date; char *author;
 *   char *url; char *logtag; PLUG_LOADTIME mustLoad;
 * So this initializer must have exactly 8 members. The classic Metamod 1.20 struct
 * additionally has an "unload" field (9 members) — if you ever build against it,
 * add ", PT_ANYTIME" back as the last entry.
 */
plugin_info_t Plugin_info = {
	META_INTERFACE_VERSION,
	"ShotFix",
	"1.1",
	__DATE__,
	"FaTaL-[Hit]",
	"https://t.me/Neo_Norm",
	"SF",
	PT_STARTUP
};

/* Plugin's supported load window: attach only at/before PT_STARTUP (metamod-r's
   plugin_info_t has no unload member, so we gate the attach time ourselves). */
#define SF_LOAD_TIME PT_STARTUP

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals = NULL;
meta_globals_t *gpMetaGlobals = NULL;
gamedll_funcs_t *gpGamedllFuncs = NULL;

#define SF_TAG "[SF]"
#define SF_MAX_CLIENTS 32

struct SfSettings {
	float spread_scale;
	float shotgun_scale;
	float punch_aim;
	float recoil_scale;
	float max_spread_dev;
	float min_trace_len;
	float max_start_offset;
	int   require_attack;
	int   apply_to_bots;
	int   debug;
	int   enforce_cvars;
	float unlag;
	float maxunlag;
	float unlagpush;
	float unlagsamples;
	float clienttrace;
} g_Cfg;

static int    g_On = 0;
static double g_LastCfgCheck = 0.0;
static double g_LastEnforce = 0.0;

static float g_PrePunch[SF_MAX_CLIENTS + 1][3];
static unsigned char g_PreValid[SF_MAX_CLIENTS + 1];

/* debug counters: 0 total, 1 not firing, 2 not client/dead, 3 start far from eye,
   4 short trace, 5 already exact, 6 deviation too big, 7 applied */
static unsigned g_Cnt[8];
static unsigned g_CntPrev[8];
static float    g_LastStartDist = 0.0f;
static float    g_LastLen = 0.0f;
static float    g_LastDev = 0.0f;

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

static void ConPrint(const char *fmt, ...) {
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_engfuncs.pfnServerPrint(buf);
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

static const char *g_Paths[] = {
	"addons/shot_fix/shot_fix.ini",
	"cstrike/addons/shot_fix/shot_fix.ini",
	"../cstrike/addons/shot_fix/shot_fix.ini",
	"shot_fix.ini"
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

		if      (strcmp(key, "spread_scale") == 0)      g_Cfg.spread_scale = (float)atof(val);
		else if (strcmp(key, "shotgun_scale") == 0)     g_Cfg.shotgun_scale = (float)atof(val);
		else if (strcmp(key, "punch_aim") == 0)         g_Cfg.punch_aim = (float)atof(val);
		else if (strcmp(key, "recoil_scale") == 0)      g_Cfg.recoil_scale = (float)atof(val);
		else if (strcmp(key, "max_spread_dev") == 0)    g_Cfg.max_spread_dev = (float)atof(val);
		else if (strcmp(key, "min_trace_len") == 0)     g_Cfg.min_trace_len = (float)atof(val);
		else if (strcmp(key, "max_start_offset") == 0)  g_Cfg.max_start_offset = (float)atof(val);
		else if (strcmp(key, "require_attack") == 0)    g_Cfg.require_attack = atoi(val);
		else if (strcmp(key, "apply_to_bots") == 0)     g_Cfg.apply_to_bots = atoi(val);
		else if (strcmp(key, "debug") == 0)             g_Cfg.debug = atoi(val);
		else if (strcmp(key, "enforce_cvars") == 0)     g_Cfg.enforce_cvars = atoi(val);
		else if (strcmp(key, "unlag") == 0)             g_Cfg.unlag = (float)atof(val);
		else if (strcmp(key, "maxunlag") == 0)          g_Cfg.maxunlag = (float)atof(val);
		else if (strcmp(key, "unlagpush") == 0)         g_Cfg.unlagpush = (float)atof(val);
		else if (strcmp(key, "unlagsamples") == 0)      g_Cfg.unlagsamples = (float)atof(val);
		else if (strcmp(key, "clienttrace") == 0)       g_Cfg.clienttrace = (float)atof(val);
	}
	fclose(f);

	strncpy(g_CfgPath, path, sizeof(g_CfgPath) - 1);
	g_CfgPath[sizeof(g_CfgPath) - 1] = 0;
	struct stat st;
	g_CfgMtime = (stat(path, &st) == 0) ? st.st_mtime : 0;
	return 1;
}

static void SetDefaults() {
	g_Cfg.spread_scale = 0.6f;
	g_Cfg.shotgun_scale = 0.85f;
	g_Cfg.punch_aim = 1.0f;
	g_Cfg.recoil_scale = 0.8f;
	g_Cfg.max_spread_dev = 1.2f;
	g_Cfg.min_trace_len = 1000.0f;
	g_Cfg.max_start_offset = 40.0f;
	g_Cfg.require_attack = 0;
	g_Cfg.apply_to_bots = 1;
	g_Cfg.debug = 1;
	g_Cfg.enforce_cvars = 1;
	g_Cfg.unlag = 1.0f;
	g_Cfg.maxunlag = 0.5f;
	g_Cfg.unlagpush = 0.0f;
	g_Cfg.unlagsamples = 1.0f;
	g_Cfg.clienttrace = -1.0f;
}

static void ClampConfig() {
	g_Cfg.spread_scale = clampf(g_Cfg.spread_scale, 0.0f, 1.0f);
	g_Cfg.shotgun_scale = clampf(g_Cfg.shotgun_scale, 0.0f, 1.0f);
	g_Cfg.punch_aim = clampf(g_Cfg.punch_aim, 0.0f, 1.0f);
	g_Cfg.recoil_scale = clampf(g_Cfg.recoil_scale, 0.0f, 1.0f);
	g_Cfg.max_spread_dev = clampf(g_Cfg.max_spread_dev, 0.1f, 2.0f);
	g_Cfg.min_trace_len = clampf(g_Cfg.min_trace_len, 200.0f, 8000.0f);
	g_Cfg.max_start_offset = clampf(g_Cfg.max_start_offset, 2.0f, 200.0f);
	if (g_Cfg.unlag < 0.0f) g_Cfg.unlag = -1.0f;
	if (g_Cfg.maxunlag < 0.0f) g_Cfg.maxunlag = -1.0f;
	if (g_Cfg.unlagpush < 0.0f) g_Cfg.unlagpush = -1.0f;
	if (g_Cfg.unlagsamples < 0.0f) g_Cfg.unlagsamples = -1.0f;
	if (g_Cfg.clienttrace < 0.0f) g_Cfg.clienttrace = -1.0f;
}

static bool LoadConfig(bool reload) {
	SfSettings old = g_Cfg;
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
/* Server cvars                                                        */
/* ------------------------------------------------------------------ */

static const char *kCvarNames[5] = {
	"sv_unlag", "sv_maxunlag", "sv_unlagpush", "sv_unlagsamples", "sv_clienttrace"
};
static cvar_t *g_CvPtr[5];
static float   g_CvOrig[5];
static int     g_CvSaved[5];
static int     g_CvMissingShown[5];

static float WantOf(int i) {
	switch (i) {
		case 0: return g_Cfg.unlag;
		case 1: return g_Cfg.maxunlag;
		case 2: return g_Cfg.unlagpush;
		case 3: return g_Cfg.unlagsamples;
		default: return g_Cfg.clienttrace;
	}
}

static void ApplyCvars() {
	for (int i = 0; i < 5; i++) {
		if (!g_CvPtr[i]) g_CvPtr[i] = g_engfuncs.pfnCVarGetPointer(kCvarNames[i]);
		cvar_t *c = g_CvPtr[i];
		float want = WantOf(i);

		if (!c) {
			if (want >= 0.0f && !g_CvMissingShown[i]) {
				g_CvMissingShown[i] = 1;
				ConPrint(SF_TAG " cvar %s not found on this server, skipped.\n", kCvarNames[i]);
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
	for (int i = 0; i < 5; i++) {
		if (g_CvSaved[i] && g_CvPtr[i]) {
			g_engfuncs.pfnCVarSetFloat(kCvarNames[i], g_CvOrig[i]);
			g_CvSaved[i] = 0;
		}
	}
}

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
		ConPrint(SF_TAG " config reloaded: spread=%.2f shotgun=%.2f punch_aim=%.2f recoil=%.2f\n",
				 g_Cfg.spread_scale, g_Cfg.shotgun_scale, g_Cfg.punch_aim, g_Cfg.recoil_scale);
	} else if (g_CfgPath[0]) {
		g_CfgMtime = st.st_mtime;
	}
}

/* ------------------------------------------------------------------ */
/* Spread reduction                                                    */
/* ------------------------------------------------------------------ */

static bool IsShotgun(edict_t *pl) {
	if (!pl->v.weaponmodel || !gpGlobals || !gpGlobals->pStringBase) return false;
	const char *wm = gpGlobals->pStringBase + pl->v.weaponmodel;
	return strstr(wm, "p_m3") != NULL || strstr(wm, "p_xm1014") != NULL;
}

static void OnTraceLine(const float *v1, const float *v2, int fNoMonsters,
						edict_t *skip, TraceResult *ptr) {
	gpMetaGlobals->mres = MRES_IGNORED;

	if (!g_On || !skip || !v1 || !v2) return;
	if (g_Cfg.spread_scale >= 0.999f && g_Cfg.shotgun_scale >= 0.999f && g_Cfg.punch_aim >= 0.999f) return;

	/* only players: cheap check before counting */
	int fl = skip->v.flags;
	if (!(fl & FL_CLIENT)) return;

	g_Cnt[0]++;

	if (g_Cfg.require_attack && !(skip->v.button & IN_ATTACK)) { g_Cnt[1]++; return; }
	if ((fl & FL_FAKECLIENT) && !g_Cfg.apply_to_bots) { g_Cnt[2]++; return; }
	if (skip->v.deadflag != DEAD_NO) { g_Cnt[2]++; return; }

	/* bullets start near the eye (origin + view_ofs) */
	float ex = skip->v.origin.x + skip->v.view_ofs.x;
	float ey = skip->v.origin.y + skip->v.view_ofs.y;
	float ez = skip->v.origin.z + skip->v.view_ofs.z;
	float sx = v1[0] - ex, sy = v1[1] - ey, sz = v1[2] - ez;
	float sd2 = sx * sx + sy * sy + sz * sz;
	g_LastStartDist = sqrtf(sd2);
	if (sd2 > g_Cfg.max_start_offset * g_Cfg.max_start_offset) { g_Cnt[3]++; return; }

	float d[3] = { v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2] };
	float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	g_LastLen = len;
	if (len < g_Cfg.min_trace_len) { g_Cnt[4]++; return; }
	float inv = 1.0f / len;
	d[0] *= inv; d[1] *= inv; d[2] *= inv;

	/* aim direction: view angle + (scaled) current recoil punch */
	float pa = g_Cfg.punch_aim;
	float ang[3] = {
		skip->v.v_angle.x + skip->v.punchangle.x * pa,
		skip->v.v_angle.y + skip->v.punchangle.y * pa,
		skip->v.v_angle.z + skip->v.punchangle.z * pa
	};
	float fwd[3];
	g_engfuncs.pfnAngleVectors(ang, fwd, NULL, NULL);

	float dev[3] = { d[0] - fwd[0], d[1] - fwd[1], d[2] - fwd[2] };
	float dl2 = dev[0] * dev[0] + dev[1] * dev[1] + dev[2] * dev[2];
	g_LastDev = sqrtf(dl2);
	if (dl2 < 1e-8f) { g_Cnt[5]++; return; }
	if (dl2 > g_Cfg.max_spread_dev * g_Cfg.max_spread_dev) { g_Cnt[6]++; return; }

	float sc = IsShotgun(skip) ? g_Cfg.shotgun_scale : g_Cfg.spread_scale;
	if (sc >= 0.999f && pa >= 0.999f) return;

	float n[3] = { fwd[0] + dev[0] * sc, fwd[1] + dev[1] * sc, fwd[2] + dev[2] * sc };
	float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (nl < 1e-6f) return;
	nl = len / nl;

	float end[3] = { v1[0] + n[0] * nl, v1[1] + n[1] * nl, v1[2] + n[2] * nl };

	g_engfuncs.pfnTraceLine(v1, end, fNoMonsters, skip, ptr);
	g_Cnt[7]++;
	gpMetaGlobals->mres = MRES_SUPERCEDE;
}

/* ------------------------------------------------------------------ */
/* Recoil reduction                                                    */
/* ------------------------------------------------------------------ */

static void OnPlayerPostThink(edict_t *pl) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_On || !pl) return;

	int idx = g_engfuncs.pfnIndexOfEdict(pl);
	if (idx < 1 || idx > SF_MAX_CLIENTS) return;

	/* Always snapshot the pre-think punchangle, even when recoil_scale is 1.0,
	   so that a later ini hot-reload to a smaller value has valid data for the
	   current frame. */
	g_PrePunch[idx][0] = pl->v.punchangle.x;
	g_PrePunch[idx][1] = pl->v.punchangle.y;
	g_PrePunch[idx][2] = pl->v.punchangle.z;
	g_PreValid[idx] = 1;
}

static void OnPlayerPostThink_Post(edict_t *pl) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_On || !pl) return;

	int idx = g_engfuncs.pfnIndexOfEdict(pl);
	if (idx < 1 || idx > SF_MAX_CLIENTS) return;
	if (!g_PreValid[idx]) return;
	g_PreValid[idx] = 0;

	/* Nothing to scale if recoil is left at vanilla strength. */
	if (g_Cfg.recoil_scale >= 0.999f) return;

	if (pl->v.deadflag != DEAD_NO) return;
	if (!(pl->v.button & IN_ATTACK)) return;
	if ((pl->v.flags & FL_FAKECLIENT) && !g_Cfg.apply_to_bots) return;

	const float *pre = g_PrePunch[idx];
	float cx = pl->v.punchangle.x, cy = pl->v.punchangle.y, cz = pl->v.punchangle.z;
	float dx = cx - pre[0], dy = cy - pre[1], dz = cz - pre[2];

	float dmag = dx * dx + dy * dy + dz * dz;
	if (dmag < 1e-6f || dmag > 900.0f) return;

	float pre2 = pre[0] * pre[0] + pre[1] * pre[1] + pre[2] * pre[2];
	float cur2 = cx * cx + cy * cy + cz * cz;
	if (cur2 <= pre2) return;

	float s = g_Cfg.recoil_scale;
	pl->v.punchangle.x = pre[0] + dx * s;
	pl->v.punchangle.y = pre[1] + dy * s;
	pl->v.punchangle.z = pre[2] + dz * s;
}

/* ------------------------------------------------------------------ */
/* Other hooks                                                         */
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

	if (g_Cfg.debug) {
		static double last_dbg = 0.0;
		if (now - last_dbg >= 3.0) {
			last_dbg = now;
			unsigned delta[8];
			bool changed = false;
			for (int i = 0; i < 8; i++) {
				delta[i] = g_Cnt[i] - g_CntPrev[i]; /* interval delta (wraps safely) */
				if (delta[i] != 0) changed = true;
			}
			if (changed) {
				ConPrint(SF_TAG " dbg(+3s): total=%u notfiring=%u skipped=%u startfar=%u short=%u exact=%u bigdev=%u APPLIED=%u"
						 " | last: start=%.1f len=%.0f dev=%.3f\n",
						 delta[0], delta[1], delta[2], delta[3], delta[4], delta[5], delta[6], delta[7],
						 g_LastStartDist, g_LastLen, g_LastDev);
				memcpy(g_CntPrev, g_Cnt, sizeof(g_Cnt));
			}
		}
	}
}

static void OnServerActivate_Post(edict_t *pEdictList, int edictCount, int clientMax) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_On) return;
	memset(g_PreValid, 0, sizeof(g_PreValid));
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
	pFunctionTable->pfnPlayerPostThink = OnPlayerPostThink;
	return TRUE;
}

C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion) {
	if (!pFunctionTable || !interfaceVersion || *interfaceVersion != INTERFACE_VERSION) {
		if (interfaceVersion) *interfaceVersion = INTERFACE_VERSION;
		return FALSE;
	}
	memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
	pFunctionTable->pfnPlayerPostThink = OnPlayerPostThink_Post;
	pFunctionTable->pfnServerActivate = OnServerActivate_Post;
	return TRUE;
}

C_DLLEXPORT int GetEngineFunctions(enginefuncs_t *pengfuncsFromEngine, int *interfaceVersion) {
	if (!pengfuncsFromEngine || !interfaceVersion) return FALSE;
	if (*interfaceVersion != ENGINE_INTERFACE_VERSION) {
		ConPrint(SF_TAG " engine hook NOT registered: interface version mismatch (got %d, expected %d)\n",
				 *interfaceVersion, ENGINE_INTERFACE_VERSION);
		*interfaceVersion = ENGINE_INTERFACE_VERSION;
		return FALSE;
	}
	memset(pengfuncsFromEngine, 0, sizeof(enginefuncs_t));
	pengfuncsFromEngine->pfnTraceLine = OnTraceLine;
	ConPrint(SF_TAG " engine hook registered (pfnTraceLine)\n");
	return TRUE;
}

META_FUNCTIONS gMetaFunctionTable = {
	NULL,                 /* GetEntityAPI */
	NULL,                 /* GetEntityAPI_Post */
	GetEntityAPI2,        /* GetEntityAPI2 */
	GetEntityAPI2_Post,   /* GetEntityAPI2_Post */
	NULL, NULL,           /* GetNewDLLFunctions(_Post) */
	GetEngineFunctions,   /* GetEngineFunctions */
	NULL                  /* GetEngineFunctions_Post */
};

C_DLLEXPORT void GiveFnptrsToDll(enginefuncs_t *pengfuncs, globalvars_t *pGlobals) {
	memcpy(&g_engfuncs, pengfuncs, sizeof(enginefuncs_t));
	gpGlobals = pGlobals;
}

C_DLLEXPORT int Meta_Query(char *interfaceVersion, plugin_info_t **plinfo, mutil_funcs_t *pMetaUtilFuncs) {
	if (!interfaceVersion || !plinfo) return FALSE;
	/* Only accept the exact metamod interface version this plugin was built for. */
	if (strcmp(interfaceVersion, META_INTERFACE_VERSION) != 0) return FALSE;
	*plinfo = &Plugin_info;
	return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS *pFunctionTable,
							meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs) {
	if (!pMGlobals || !pFunctionTable) return FALSE;

	/* Refuse to attach at load times we don't support.
	   (Plugin_info has no .load/.unload members in metamod-r — use our own constants.) */
	if (now > SF_LOAD_TIME) return FALSE;

	gpMetaGlobals = pMGlobals;
	gpGamedllFuncs = pGamedllFuncs;
	memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));

	bool found = LoadConfig(false);

	memset(g_PreValid, 0, sizeof(g_PreValid));
	memset(g_Cnt, 0, sizeof(g_Cnt));
	memset(g_CntPrev, 0, sizeof(g_CntPrev));
	memset(g_CvPtr, 0, sizeof(g_CvPtr));
	memset(g_CvOrig, 0, sizeof(g_CvOrig));
	memset(g_CvSaved, 0, sizeof(g_CvSaved));
	memset(g_CvMissingShown, 0, sizeof(g_CvMissingShown));
	g_LastCfgCheck = NowSec();
	g_LastEnforce = 0.0;

	ApplyCvars();
	g_On = 1;

	ConPrint(SF_TAG " v1.1 loaded (%s): spread=%.2f shotgun=%.2f punch_aim=%.2f recoil=%.2f debug=%d\n",
			 found ? g_CfgPath : "no ini, defaults",
			 g_Cfg.spread_scale, g_Cfg.shotgun_scale, g_Cfg.punch_aim, g_Cfg.recoil_scale, g_Cfg.debug);
	return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason) {
	g_On = 0;
	RestoreCvars();
	return TRUE;
}
