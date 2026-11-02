/*
 * ScreenWatch 1.4 - SERVER metamod plugin (CS 1.6 / ReHLDS / Metamod-R)
 * Author: FaTaL-[Hit]  https://t.me/Neo_Norm
 *
 * v1.4 changes (compile fixes for Metamod-R):
 *  - No pfnRegUserMsg access at all (metamod-r mutil_funcs_t has no such
 *    member). usermsg "SWRq" is registered via gameDLL pfnUserMsgRegister +
 *    engine pfnUserMsgID lookup; graceful fallback to the console trigger.
 *  - All engine access goes through explicit g_engfuncs.pfn* calls
 *    (pfnGetPlayerUserId, pfnIndexOfEdict, pfnCmd_Argv, pfnClientPrintf) -
 *    no SDK macros that differ between HLSDK revisions.
 *  - Author kept in Plugin_info, banner and load message: FaTaL-[Hit].
 */

#include <extdll.h>
#include <meta_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef HUD_PRINTCONSOLE
#define HUD_PRINTCONSOLE 2   /* enginefuncs pfnClientPrintf dest */
#endif

#define SW_TAG "[SW]"
#define SW_MAX_CLIENTS 32
#define SW_CHUNK_BYTES 384          /* must match client side */
#define MSG_SW_REQ "SWRq"

enum { REQ_WINPROC = 1, REQ_SCREENSHOT = 2 };

struct SwCfg {
	int   scan_on_join;
	int   interval_sec;
	int   max_shot_kb;
	char  save_dir[256];
	char  watchlist[1024];
	int   debug;
} g_Cfg;

static int    g_On = 0;
static double g_LastCfgCheck = 0.0;
static double g_NextSweep = 0.0;
static char   g_CfgPath[256] = "";
static time_t g_CfgMtime = 0;

static mutil_funcs_t *g_pMetaUtil = NULL;
static int g_msgSwReq = 0;

/* Custom usermsg "SWRq" registration.
   mutil_funcs_t in metamod-r has NO pfnRegUserMsg member (and its exact
   layout varies between MM forks), so we never poke it directly. We register
   through the game DLL's exported UserMsgRegister and look the id up via the
   engine. If that is unavailable we fall back to the console-line trigger
   ("sw_do"), which works everywhere. */
static int RegisterUserMsg(const char *name, int size) {
	if (gpGamedllFuncs && gpGamedllFuncs->dll_table &&
	    gpGamedllFuncs->dll_table->pfnUserMsgRegister) {
		gpGamedllFuncs->dll_table->pfnUserMsgRegister(name, size, NULL);
		int id = g_engfuncs.pfnUserMsgID(name, NULL);
		if (id > 0) return id;
	}
	return 0; /* fallback console trigger still works */
}


struct SlotState {
	int   connected;
	float joinTime;
	int   hasModule;
	int   optOut;
	double lastReadyAt;
	unsigned char *buf;
	size_t bufCap;
	size_t highWater;
	int   chunksGot, chunksExpected;
};
static SlotState g_Slot[SW_MAX_CLIENTS + 1];

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals = NULL;
meta_globals_t *gpMetaGlobals = NULL;
gamedll_funcs_t *gpGamedllFuncs = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static double NowSec() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void ConPrint(const char *fmt, ...) {
	char buf[768];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	g_engfuncs.pfnServerPrint(buf);
}

static void trim(char *s) {
	char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void lowerstr(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static edict_t *EdictOf(int idx) {
	if (idx < 1 || idx > gpGlobals->maxClients) return NULL;
	edict_t *e = g_engfuncs.pfnPEntityOfEntIndex(idx);
	if (!e || e == g_engfuncs.pfnPEntityOfEntIndex(0)) return NULL;
	return e;
}

static int IdxOf(edict_t *e) {
	if (!e) return 0;
	return g_engfuncs.pfnIndexOfEdict(e);
}

static int UserIdOf(edict_t *e) {
	return g_engfuncs.pfnGetPlayerUserId(e);
}

/* ------------------------------------------------------------------ */
/* Config                                                             */
/* ------------------------------------------------------------------ */

static const char *g_Paths[] = {
	"addons/screenwatch/screenwatch.ini",
	"cstrike/addons/screenwatch/screenwatch.ini",
	"../cstrike/addons/screenwatch/screenwatch.ini",
	"screenwatch.ini"
};

static int try_load_config(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (!line[0] || line[0] == ';' || line[0] == '#') continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0; char *key = line, *val = eq + 1;
		for (char *c = val; *c; c++) if (*c == ';' || *c == '#') { *c = 0; break; }
		trim(key); trim(val);
		if      (!strcmp(key, "scan_on_join")) g_Cfg.scan_on_join = atoi(val);
		else if (!strcmp(key, "interval_sec")) g_Cfg.interval_sec = atoi(val);
		else if (!strcmp(key, "max_shot_kb"))  g_Cfg.max_shot_kb = atoi(val);
		else if (!strcmp(key, "save_dir"))     { strncpy(g_Cfg.save_dir, val, 255); g_Cfg.save_dir[255] = 0; }
		else if (!strcmp(key, "watchlist"))    { strncpy(g_Cfg.watchlist, val, 1023); g_Cfg.watchlist[1023] = 0; lowerstr(g_Cfg.watchlist); }
		else if (!strcmp(key, "debug"))        g_Cfg.debug = atoi(val);
	}
	fclose(f);
	strncpy(g_CfgPath, path, 255); g_CfgPath[255] = 0;
	struct stat st; g_CfgMtime = (stat(path, &st) == 0) ? st.st_mtime : 0;
	return 1;
}

static void SetDefaults() {
	g_Cfg.scan_on_join = 1;
	g_Cfg.interval_sec = 0;
	g_Cfg.max_shot_kb = 900;
	strcpy(g_Cfg.save_dir, "cstrike/screenwatch");
	strcpy(g_Cfg.watchlist, "aimbot;wallhack;esp;overlay;d3dhook;injector;trainer;cheat");
	g_Cfg.debug = 1;
}

static bool LoadConfig(bool reload) {
	SwCfg old = g_Cfg;
	char oldPath[256]; time_t oldM = g_CfgMtime; memcpy(oldPath, g_CfgPath, 256);
	SetDefaults();
	bool ok = false;
	if (reload && oldPath[0]) ok = try_load_config(oldPath) != 0;
	else for (size_t i = 0; i < sizeof(g_Paths)/sizeof(g_Paths[0]); i++)
		if (try_load_config(g_Paths[i])) { ok = true; break; }
	if (!ok) { g_Cfg = old; memcpy(g_CfgPath, oldPath, 256); g_CfgMtime = oldM; return false; }
	if (g_Cfg.max_shot_kb < 100) g_Cfg.max_shot_kb = 100;
	if (g_Cfg.max_shot_kb > 4000) g_Cfg.max_shot_kb = 4000;
	if (g_Cfg.interval_sec < 0) g_Cfg.interval_sec = 0;
	return true;
}

static void CheckConfigReload(double now) {
	if (now - g_LastCfgCheck < 2.0) return;
	g_LastCfgCheck = now;
	struct stat st;
	if (!g_CfgPath[0]) return;
	if (stat(g_CfgPath, &st) != 0) return;
	if (st.st_mtime == g_CfgMtime) return;
	if (LoadConfig(true)) ConPrint(SW_TAG " config reloaded\n");
	else g_CfgMtime = st.st_mtime;
}

/* ------------------------------------------------------------------ */
/* Storage                                                            */
/* ------------------------------------------------------------------ */

static void EnsureDir() {
	char d[300]; strncpy(d, g_Cfg.save_dir, 299); d[299] = 0;
	for (char *p = d + 1; *p; p++) {
		if (*p == '/') { *p = 0; mkdir(d, 0755); *p = '/'; }
	}
	mkdir(d, 0755);
}

static int WatchlistHit(const char *namesLower) {
	char wl[1024]; strncpy(wl, g_Cfg.watchlist, 1023); wl[1023] = 0;
	char *tok = strtok(wl, ";");
	int hits = 0;
	while (tok) {
		trim(tok);
		if (*tok && strstr(namesLower, tok)) {
			ConPrint(SW_TAG " !! WATCHLIST HIT: '%s'\n", tok);
			hits++;
		}
		tok = strtok(NULL, ";");
	}
	return hits;
}

/* ------------------------------------------------------------------ */
/* Requests to client                                                 */
/* ------------------------------------------------------------------ */

static edict_t *FindPlayer(const char *arg) {
	if (!arg || !arg[0]) return NULL;
	/* "#N" means entity slot index directly (used by the AMXX menu) */
	if (arg[0] == '#' && isdigit((unsigned char)arg[1])) {
		int slot = atoi(arg + 1);
		edict_t *e = EdictOf(slot);
		if (e && (e->v.flags & FL_CLIENT)) return e;
		return NULL;
	}
	int uid = atoi(arg);
	if (uid > 0) {
		for (int i = 1; i <= gpGlobals->maxClients; i++) {
			edict_t *e = EdictOf(i);
			if (e && (e->v.flags & FL_CLIENT) && !(e->v.flags & FL_FAKECLIENT))
				if (UserIdOf(e) == uid) return e;
		}
		return NULL;
	}
	char low[64]; strncpy(low, arg, 63); low[63] = 0; lowerstr(low);
	for (int i = 1; i <= gpGlobals->maxClients; i++) {
		edict_t *e = EdictOf(i);
		if (e && (e->v.flags & FL_CLIENT) && !(e->v.flags & FL_FAKECLIENT)) {
			char nm[64]; strncpy(nm, STRING(e->v.netname), 63); nm[63] = 0; lowerstr(nm);
			if (strstr(nm, low)) return e;
		}
	}
	return NULL;
}

static void SendRequest(int idx, int flags) {
	edict_t *e = EdictOf(idx);
	if (!e) return;
	if (g_Slot[idx].optOut) {
		if (g_Cfg.debug) ConPrint(SW_TAG " slot %d opted out, request skipped\n", idx);
		return;
	}

	/* channel 1: custom usermsg (works if the loader bridges it) */
	if (g_msgSwReq > 0) {
		MESSAGE_BEGIN(MSG_ONE, g_msgSwReq, NULL, e);
		WRITE_BYTE(flags);
		MESSAGE_END();
	}

	/* channel 2: universal fallback - write "sw_do <flags>" into the client
	   console; screenwatch.cfg aliases sw_do to the client module export. */
	char txt[64];
	snprintf(txt, sizeof(txt), "\nsw_do %d\n", flags);
	g_engfuncs.pfnClientPrintf(e, HUD_PRINTCONSOLE, txt);

	if (g_Cfg.debug) ConPrint(SW_TAG " -> req flags=%d slot=%d\n", flags, idx);
}

/* ------------------------------------------------------------------ */
/* Ingest from client commands                                        */
/* ------------------------------------------------------------------ */

static int HexVal(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static void FreeBuf(SlotState *s) { free(s->buf); s->buf = NULL; s->bufCap = s->highWater = 0; }

static void OnShotBegin(int idx, int total, int chunks) {
	SlotState *s = &g_Slot[idx];
	s->chunksGot = 0; s->chunksExpected = chunks;
	if (total <= 0 || total > g_Cfg.max_shot_kb * 1024 || chunks <= 0 || chunks > 20000) {
		ConPrint(SW_TAG " shot REJECTED size=%d chunks=%d slot=%d\n", total, chunks, idx);
		FreeBuf(s); s->chunksExpected = -1;
		return;
	}
	FreeBuf(s);
	s->buf = (unsigned char *)malloc((size_t)total);
	if (!s->buf) { s->chunksExpected = -1; return; }
	s->bufCap = (size_t)total; s->highWater = 0;
	ConPrint(SW_TAG " shot begin slot %d: %d B / %d chunks\n", idx, total, chunks);
}

static void OnShotChunk(int idx, int seq, const char *hexpart) {
	SlotState *s = &g_Slot[idx];
	if (!s->buf || seq < 0) return;
	size_t pos = (size_t)seq * SW_CHUNK_BYTES;
	for (const char *p = hexpart; *p; p += 2) {
		int hi = HexVal(p[0]);
		int lo = p[1] ? HexVal(p[1]) : -1;
		if (hi < 0 || lo < 0) break;
		if (pos >= s->bufCap) break;
		s->buf[pos++] = (unsigned char)((hi << 4) | lo);
	}
	if (pos > s->highWater) s->highWater = pos;
	s->chunksGot = seq + 1;
}

static void OnShotEnd(int idx) {
	SlotState *s = &g_Slot[idx];
	if (!s->buf || s->chunksExpected < 0) { FreeBuf(s); return; }
	EnsureDir();
	char path[400];
	time_t t = time(NULL);
	snprintf(path, sizeof(path), "%s/p%d_%ld.bmp", g_Cfg.save_dir, idx, (long)t);
	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(s->buf, 1, s->highWater, f); fclose(f);
		ConPrint(SW_TAG " SCREENSHOT saved %s (%lu bytes)\n", path, (unsigned long)s->highWater);
	} else ConPrint(SW_TAG " FAILED writing %s\n", path);
	FreeBuf(s);
	s->chunksExpected = 0;
}

/* argv access that does not depend on SDK macros */
static int SwArgc() { return g_engfuncs.pfnCmd_Argc(); }
static const char *SwArgv(int n) { return g_engfuncs.pfnCmd_Argv(n); }

void CmdSwImpl(const char *args);

static void HandleClientCmd(edict_t *pl) {
	if (!g_On || !pl) return;
	int idx = IdxOf(pl);
	if (idx < 1 || idx > SW_MAX_CLIENTS) return;
	SlotState *s = &g_Slot[idx];

	const char *c = SwArgv(0);

	if (!strcmp(c, "sw_off")) {
		s->optOut = 1;
		ConPrint(SW_TAG " slot %d opted OUT of screen watch (consent withdrawn)\n", idx);
		return;
	}
	if (!strcmp(c, "sw_on")) {
		s->optOut = 0;
		ConPrint(SW_TAG " slot %d opted back IN\n", idx);
		return;
	}
	if (!strcmp(c, "sw_ready")) {
		s->hasModule = 1; s->lastReadyAt = NowSec();
		ConPrint(SW_TAG " module OK slot %d (%s)\n", idx, STRING(pl->v.netname));
		if (g_Cfg.scan_on_join) SendRequest(idx, REQ_WINPROC | REQ_SCREENSHOT);
		return;
	}
	if (!strcmp(c, "sw_windows") && SwArgc() >= 2) {
		char raw[1600]; strncpy(raw, SwArgv(1), 1599); raw[1599] = 0;
		ConPrint(SW_TAG " WINDOWS slot %d: %s\n", idx, raw);
		char low[1600]; strncpy(low, raw, 1599); low[1599] = 0; lowerstr(low);
		WatchlistHit(low);
		return;
	}
	if (!strcmp(c, "sw_procs") && SwArgc() >= 2) {
		char raw[1600]; strncpy(raw, SwArgv(1), 1599); raw[1599] = 0;
		ConPrint(SW_TAG " PROCS slot %d: %s\n", idx, raw);
		char low[1600]; strncpy(low, raw, 1599); low[1599] = 0; lowerstr(low);
		WatchlistHit(low);
		return;
	}
	if (!strcmp(c, "sw_shot_begin") && SwArgc() >= 3) {
		OnShotBegin(idx, atoi(SwArgv(1)), atoi(SwArgv(2))); return;
	}
	if (!strcmp(c, "sw_shot_chunk") && SwArgc() >= 3) {
		OnShotChunk(idx, atoi(SwArgv(1)), SwArgv(2)); return;
	}
	if (!strcmp(c, "sw_shot_end")) { OnShotEnd(idx); return; }
	if (!strcmp(c, "sw_shot_failed")) {
		ConPrint(SW_TAG " shot FAILED on client slot %d (secure desktop/RDP?)\n", idx);
		return;
	}
	/* admin subcommands typed in server console arrive here too when routed
	   through rcon client command forwarding (see CmdSw below) */
	if (!strncmp(c, "sw_", 3)) {
		char full[300];
		int need = snprintf(full, sizeof(full), "%s", c);
		for (int i = 1; i < SwArgc() && need < (int)sizeof(full) - 1; i++)
			need += snprintf(full + need, sizeof(full) - need, " %s", SwArgv(i));
		CmdSwImpl(full);
	}
}

/* ------------------------------------------------------------------ */
/* Entity hooks                                                       */
/* ------------------------------------------------------------------ */

static void OnClientCommand(edict_t *pl) {
	gpMetaGlobals->mres = MRES_IGNORED;
	HandleClientCmd(pl);
}

static void OnClientPutInServer_Post(edict_t *pl) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_On || !pl) return;
	int idx = IdxOf(pl);
	if (idx >= 1 && idx <= SW_MAX_CLIENTS) {
		FreeBuf(&g_Slot[idx]);
		memset(&g_Slot[idx], 0, sizeof(SlotState));
		g_Slot[idx].connected = 1;
		g_Slot[idx].joinTime = gpGlobals->time;
	}
}

static void OnClientDisconnect_Post(edict_t *pl) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!pl) return;
	int idx = IdxOf(pl);
	if (idx >= 1 && idx <= SW_MAX_CLIENTS) {
		if (g_Slot[idx].connected && !g_Slot[idx].hasModule)
			ConPrint(SW_TAG " slot %d left WITHOUT module (evading?)\n", idx);
		FreeBuf(&g_Slot[idx]);
		memset(&g_Slot[idx], 0, sizeof(SlotState));
	}
}

static void OnStartFrame(void) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_On) return;
	double now = NowSec();
	CheckConfigReload(now);

	if (g_Cfg.interval_sec > 0 && now >= g_NextSweep) {
		g_NextSweep = now + g_Cfg.interval_sec;
		for (int i = 1; i <= SW_MAX_CLIENTS; i++) {
			edict_t *e = EdictOf(i);
			if (e && (e->v.flags & FL_CLIENT) && g_Slot[i].hasModule)
				SendRequest(i, REQ_WINPROC | REQ_SCREENSHOT);
		}
	}

	for (int i = 1; i <= SW_MAX_CLIENTS; i++) {
		SlotState *s = &g_Slot[i];
		if (s->connected && !s->hasModule && s->joinTime > 0 &&
			(double)(gpGlobals->time - s->joinTime) > 20.0) {
			s->joinTime = -1.0f; /* report once */
			ConPrint(SW_TAG " slot %d: NO client module after 20s (blocked/unpatched)\n", i);
		}
	}
}

static void OnServerActivate_Post(edict_t *pEdictList, int edictCount, int clientMax) {
	gpMetaGlobals->mres = MRES_IGNORED;
	(void)pEdictList; (void)edictCount; (void)clientMax;
	for (int i = 1; i <= SW_MAX_CLIENTS; i++) { FreeBuf(&g_Slot[i]); memset(&g_Slot[i], 0, sizeof(SlotState)); }
	for (int i = 1; i <= gpGlobals->maxClients; i++) {
		edict_t *e = EdictOf(i);
		if (e && (e->v.flags & FL_CLIENT)) {
			g_Slot[i].connected = 1; g_Slot[i].joinTime = gpGlobals->time;
		}
	}
	g_NextSweep = NowSec() + 5.0;
}

/* ------------------------------------------------------------------ */
/* Command implementation (shared by rcon hook and client cmd)        */
/* ------------------------------------------------------------------ */

void CmdSwImpl(const char *args) {
	char buf[256]; strncpy(buf, args ? args : "", 255); buf[255] = 0;
	char *save = NULL;
	char *sub = strtok_r(buf, " \t", &save);
	if (!sub) {
		ConPrint(SW_TAG " usage: sw_shot|sw_win|sw_full <userid|#slot|name> | sw_list | sw_watch [add <pat>]\n");
		return;
	}
	char *a1 = strtok_r(NULL, " \t", &save);

	if ((!strcmp(sub, "sw_shot") || !strcmp(sub, "sw_win") || !strcmp(sub, "sw_full")) && a1) {
		edict_t *e = FindPlayer(a1);
		if (!e) { ConPrint(SW_TAG " player not found: %s\n", a1); return; }
		int idx = IdxOf(e);
		int flags = 0;
		if (!strcmp(sub, "sw_shot")) flags = REQ_SCREENSHOT;
		else if (!strcmp(sub, "sw_win")) flags = REQ_WINPROC;
		else flags = REQ_WINPROC | REQ_SCREENSHOT;
		SendRequest(idx, flags);
		return;
	}
	if (!strcmp(sub, "sw_list")) {
		ConPrint(SW_TAG " ---- module status ----\n");
		for (int i = 1; i <= gpGlobals->maxClients; i++) {
			edict_t *e = EdictOf(i);
			if (e && (e->v.flags & FL_CLIENT))
				ConPrint("  slot %d userid %d %s\n", i, UserIdOf(e),
				         g_Slot[i].hasModule ? "MODULE OK" : "no module");
		}
		return;
	}
	if (!strcmp(sub, "sw_watch")) {
		char *a2 = strtok_r(NULL, " \t", &save);
		if (a1 && !strcmp(a1, "add") && a2) {
			char low[128]; strncpy(low, a2, 127); low[127] = 0; lowerstr(low);
			strncat(g_Cfg.watchlist, ";", 1023 - strlen(g_Cfg.watchlist));
			strncat(g_Cfg.watchlist, low, 1023 - strlen(g_Cfg.watchlist));
			ConPrint(SW_TAG " watchlist += %s\n", low);
		} else ConPrint(SW_TAG " watchlist: %s\n", g_Cfg.watchlist);
		return;
	}
	ConPrint(SW_TAG " unknown subcommand\n");
}

/* ------------------------------------------------------------------ */
/* Metamod plumbing                                                   */
/* ------------------------------------------------------------------ */

plugin_info_t Plugin_info = {
	META_INTERFACE_VERSION,
	"ScreenWatch",
	"1.4",
	__DATE__,
	"FaTaL-[Hit]",
	"https://t.me/Neo_Norm",
	"SW",
	PT_STARTUP,
	PT_ANYTIME
};

C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion) {
	if (!pFunctionTable || !interfaceVersion || *interfaceVersion != INTERFACE_VERSION) {
		if (interfaceVersion) *interfaceVersion = INTERFACE_VERSION;
		return FALSE;
	}
	memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
	pFunctionTable->pfnStartFrame = OnStartFrame;
	pFunctionTable->pfnClientCommand = OnClientCommand;
	return TRUE;
}

C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion) {
	if (!pFunctionTable || !interfaceVersion || *interfaceVersion != INTERFACE_VERSION) {
		if (interfaceVersion) *interfaceVersion = INTERFACE_VERSION;
		return FALSE;
	}
	memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
	pFunctionTable->pfnClientPutInServer = OnClientPutInServer_Post;
	pFunctionTable->pfnClientDisconnect = OnClientDisconnect_Post;
	pFunctionTable->pfnServerActivate = OnServerActivate_Post;
	return TRUE;
}

META_FUNCTIONS gMetaFunctionTable = {
	NULL, NULL,
	GetEntityAPI2,
	GetEntityAPI2_Post,
	NULL, NULL,
	NULL, NULL
};

C_DLLEXPORT void GiveFnptrsToDll(enginefuncs_t *pengfuncs, globalvars_t *pGlobals) {
	memcpy(&g_engfuncs, pengfuncs, sizeof(enginefuncs_t));
	gpGlobals = pGlobals;
}

C_DLLEXPORT int Meta_Query(char *interfaceVersion, plugin_info_t **plinfo, mutil_funcs_t *pMetaUtilFuncs) {
	if (!interfaceVersion || !plinfo) return FALSE;
	if (strcmp(interfaceVersion, META_INTERFACE_VERSION) != 0) return FALSE;
	*plinfo = &Plugin_info;
	g_pMetaUtil = pMetaUtilFuncs;
	return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS *pFunctionTable,
                                                        meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs) {
	if (!pMGlobals || !pFunctionTable) return FALSE;
	if (now > PT_STARTUP) return FALSE;

	gpMetaGlobals = pMGlobals;
	gpGamedllFuncs = pGamedllFuncs;
	memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));

	/* register our custom usermsg (portable: metamod-r -> gameDLL,
	   classic MM 1.2x -> mutil pfnRegUserMsg) */
	g_msgSwReq = RegisterUserMsg(MSG_SW_REQ, 1);
	if (g_msgSwReq <= 0)
		ConPrint(SW_TAG " note: usermsg 'SWRq' not registered; using console-line trigger only\n");

	LoadConfig(false);
	EnsureDir();
	memset(g_Slot, 0, sizeof(g_Slot));
	g_LastCfgCheck = NowSec();
	g_NextSweep = NowSec() + 10.0;

	g_On = 1;
	ConPrint(SW_TAG " v1.4 loaded (Author: FaTaL-[Hit]). dir=%s scan_on_join=%d interval=%ds msgid=%d\n",
	         g_Cfg.save_dir, g_Cfg.scan_on_join, g_Cfg.interval_sec, g_msgSwReq);
	return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason) {
	(void)now; (void)reason;
	g_On = 0;
	for (int i = 1; i <= SW_MAX_CLIENTS; i++) FreeBuf(&g_Slot[i]);
	return TRUE;
}
