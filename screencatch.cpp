#include <extdll.h>
#include <meta_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

/*
 * ScreenCatch 1.0 - Metamod plugin for CS 1.6 / REHLDS / Metamod-R
 *
 * WHAT IT DOES
 *   Hooks the engine network-message layer (pfnMessageBegin, pfnWriteByte/
 *   String and friends, pfnMessageEnd via GetEngineFunctions) and captures
 *   EVERYTHING that is drawn on a player's screen by the server:
 *     - SayText      : chat, hud_saytext/client_print output, menu text,
 *                      most cheat overlays that print info into chat
 *     - TextMsg      : top-center HUD lines ("You picked up the M4A1...")
 *     - HudText      : center-screen text (round start/end, notices)
 *     - HudMsg       : positioned colored HUD messages (radar/ESP panels)
 *     - StatusValue / StatusText : score/status rows
 *     - ScoreInfo    : scoreboard rows (off by default, noisy)
 *   Everything captured is:
 *     1) written to logs/screencatch/<map>-YYYYMMDD-HHMMSS.log (full detail)
 *     2) kept in a per-player ring buffer = the "screen snapshot" history
 *     3) relayed LIVE to an admin who issued sc_watch, and SUSPECT alerts
 *        go to all admins listed in screencatch.ini (admin_steamid=...)
 *
 * ADMIN COMMANDS (server console/RCON, or typed in chat by an admin):
 *   sc_watch <name|#userid|slot>   live feed of that player's screen to you
 *   sc_unwatch                     stop watching
 *   sc_shot   <name|#userid|slot>  SCREENSHOT: dump last buffered lines now
 *   sc_dump   <name|#userid|slot>  dump full recent history
 *   sc_list                        show current watches
 *   sc_stats                       counters per message type + log path
 *   sc_on / sc_off                 master switch
 *
 * CHEAT HEURISTICS (logged as SUSPECT + alert to admins):
 *   - known cheat words in any text reaching a client's screen
 *   - binary/control-char junk beyond normal color codes (overlay spam)
 *   - flood: many HUD/SayText msgs per second to one client
 *
 * HONEST LIMITATION
 *   This sees what the SERVER sends to each client's screen. Purely
 *   client-side renderers (external ESP overlays, direct gEngfuncs HUD
 *   calls) never touch the network and are invisible here. But the majority
 *   of public CS cheats route their text through saytext/hudmsg because it
 *   is the easiest way to draw text in HL - those ARE visible, and this also
 *   gives you a real-time mirror of everything any suspect player sees.
 */

#define SC_TAG "[SC]"
#define SC_MAX_CLIENTS 32
#define SC_LINE_MAX 512
#define SC_RELAY_MAX 16
#define SC_HIST_CAP 256

// Define load time explicitly to avoid struct mismatch issues in some MM versions
#define SC_LOAD_TIME PT_ANYTIME
#define SC_UNLOAD_TIME PT_ANYTIME

plugin_info_t Plugin_info = {
	META_INTERFACE_VERSION,
	"ScreenCatch",
	"1.0",
	__DATE__,
	"FaTaL-[Hit]",
	"https://t.me/Neo_Norm",
	"SC",
	SC_LOAD_TIME,
	SC_UNLOAD_TIME
};

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals = NULL;
meta_globals_t *gpMetaGlobals = NULL;
gamedll_funcs_t *gpGamedllFuncs = NULL;
mutil_funcs_t *g_pMetaUtil = NULL;

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

struct ScCfg {
	int   enabled;
	int   catch_saytext;
	int   catch_textmsg;
	int   catch_hudtext;
	int   catch_hudmsg;
	int   catch_status;
	int   catch_score;
	int   relay_console;      /* echo SUSPECT alerts to server console too */
	int   alert_suspect;      /* send SUSPECT alerts to admins */
	float flood_per_sec;      /* msgs/sec threshold per client */
	int   hist_lines;         /* ring buffer lines kept per player */
	char  logdir[128];
	char  admins[SC_RELAY_MAX][32]; /* steamids */
	int   num_admins;
} g_Cfg;

static const char *g_Paths[] = {
	"addons/screen_catch/screencatch.ini",
	"cstrike/addons/screen_catch/screencatch.ini",
	"../cstrike/addons/screen_catch/screencatch.ini",
	"screencatch.ini"
};
static char   g_CfgPath[256] = "";
static time_t g_CfgMtime = 0;

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

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void SetDefaults(void) {
	g_Cfg.enabled = 1;
	g_Cfg.catch_saytext = 1;
	g_Cfg.catch_textmsg = 1;
	g_Cfg.catch_hudtext = 1;
	g_Cfg.catch_hudmsg = 1;
	g_Cfg.catch_status = 1;
	g_Cfg.catch_score = 0;
	g_Cfg.relay_console = 1;
	g_Cfg.alert_suspect = 1;
	g_Cfg.flood_per_sec = 8.0f;
	g_Cfg.hist_lines = 48;
	strcpy(g_Cfg.logdir, "logs/screencatch");
	g_Cfg.num_admins = 0;
}

static int try_load_config(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;

	char line[256];
	g_Cfg.num_admins = 0;
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
		trim(key); trim(val);

		if      (strcmp(key, "enabled") == 0)            g_Cfg.enabled = atoi(val);
		else if (strcmp(key, "catch_saytext") == 0)      g_Cfg.catch_saytext = atoi(val);
		else if (strcmp(key, "catch_textmsg") == 0)      g_Cfg.catch_textmsg = atoi(val);
		else if (strcmp(key, "catch_hudtext") == 0)      g_Cfg.catch_hudtext = atoi(val);
		else if (strcmp(key, "catch_hudmsg") == 0)       g_Cfg.catch_hudmsg = atoi(val);
		else if (strcmp(key, "catch_status") == 0)       g_Cfg.catch_status = atoi(val);
		else if (strcmp(key, "catch_score") == 0)        g_Cfg.catch_score = atoi(val);
		else if (strcmp(key, "relay_console") == 0)      g_Cfg.relay_console = atoi(val);
		else if (strcmp(key, "alert_suspect") == 0)      g_Cfg.alert_suspect = atoi(val);
		else if (strcmp(key, "flood_per_sec") == 0)      g_Cfg.flood_per_sec = (float)atof(val);
		else if (strcmp(key, "hist_lines") == 0)         g_Cfg.hist_lines = atoi(val);
		else if (strcmp(key, "logdir") == 0)             { strncpy(g_Cfg.logdir, val, sizeof(g_Cfg.logdir)-1); g_Cfg.logdir[sizeof(g_Cfg.logdir)-1]=0; }
		else if (strcmp(key, "admin_steamid") == 0) {
			if (g_Cfg.num_admins < SC_RELAY_MAX && val[0]) {
				strncpy(g_Cfg.admins[g_Cfg.num_admins], val, 31);
				g_Cfg.admins[g_Cfg.num_admins][31] = 0;
				g_Cfg.num_admins++;
			}
		}
	}
	fclose(f);

	strncpy(g_CfgPath, path, sizeof(g_CfgPath) - 1);
	g_CfgPath[sizeof(g_CfgPath) - 1] = 0;
	struct stat st;
	g_CfgMtime = (stat(path, &st) == 0) ? st.st_mtime : 0;
	return 1;
}

static void ClampConfig(void) {
	g_Cfg.flood_per_sec = clampf(g_Cfg.flood_per_sec, 1.0f, 100.0f);
	g_Cfg.hist_lines = (int)clampf((float)g_Cfg.hist_lines, 8.0f, (float)SC_HIST_CAP);
}

static bool LoadConfig(bool reload) {
	ScCfg oldCfg = g_Cfg;
	char oldPath[256]; time_t oldMtime = g_CfgMtime;
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
		g_Cfg = oldCfg;
		memcpy(g_CfgPath, oldPath, sizeof(g_CfgPath));
		g_CfgMtime = oldMtime;
		return false;
	}
	ClampConfig();
	return true;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

struct ScHist {
	char   line[SC_LINE_MAX];
	int    type;
	double t;
};

static ScHist  g_Hist[SC_MAX_CLIENTS + 1][SC_HIST_CAP];
static int     g_HistHead[SC_MAX_CLIENTS + 1];
static int     g_HistCount[SC_MAX_CLIENTS + 1];

static double  g_MsgTime[SC_MAX_CLIENTS + 1];
static int     g_MsgBurst[SC_MAX_CLIENTS + 1];
static unsigned long g_TotalCap[SC_MAX_CLIENTS + 1];

static int     g_WatchSlot[SC_MAX_CLIENTS + 1];   /* watcher -> target */
static int     g_WatchedBy[SC_MAX_CLIENTS + 1];   /* target -> watcher */

/* counters: 0 SayText 1 TextMsg 2 HudText 3 HudMsg 4 StatusVal
   5 StatusText 6 ScoreInfo 7 suspect */
static unsigned g_TypeCnt[8];

static FILE   *g_Log = NULL;
static char    g_LogName[256] = "";
static double  g_LastCfgCheck = 0.0;

/* message decoding state */
static int     g_MsgActive = 0;
static int     g_MsgType = 0, g_MsgDest = 0, g_MsgArg1 = 0;
static uchar   g_MsgBuf[2048];
static int     g_MsgBufLen = 0;
static int     g_RelayLevel = 0;   /* >0 while we send our own SayText */

/* resolved user-message ids */
static int g_SayTextId = 0, g_TextMsgId = 0, g_HudTextId = 0, g_HudMsgId = 0,
		   g_StatusValueId = 0, g_StatusTextId = 0, g_ScoreInfoId = 0;

/* forward decls (Capture <-> RelayToAdmins mutual dependency) */
static void TellClient(int idx, const char *fmt, ...);
static void RelayToAdmins(const char *fmt, ...);
static inline int IsEnabled(void) { return g_Cfg.enabled; }

/* ------------------------------------------------------------------ */
/* Engine helpers                                                      */
/* ------------------------------------------------------------------ */

static double NowSec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void ConPrint(const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_engfuncs.pfnServerPrint(buf);
}

static const char *PlayerName(int idx) {
	static char nm[64];
	nm[0] = 0;
	if (idx >= 1 && idx <= gpGlobals->maxclients) {
		edict_t *p = INDEXENT(idx);
		if (p && !p->free) {
			strncpy(nm, STRING(p->v.netname), sizeof(nm) - 1);
			nm[sizeof(nm) - 1] = 0;
		}
	}
	if (!nm[0]) strcpy(nm, "?");
	return nm;
}

static void TellClient(int idx, const char *fmt, ...) {
	if (idx < 1 || idx > gpGlobals->maxclients) return;
	edict_t *pPlayer = INDEXENT(idx);
	if (!pPlayer || pPlayer->free || !(pPlayer->v.flags & FL_CLIENT)) return;

	char msg[SC_LINE_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	g_RelayLevel++;
	MESSAGE_BEGIN(MSG_ONE, g_SayTextId, NULL, pPlayer);
		WRITE_BYTE(idx);
		WRITE_STRING(msg);
	MESSAGE_END();
	g_RelayLevel--;
}

static int IsAdminIdx(int idx) {
	const char *sid = g_engfuncs.pfnGetPlayerAuthId(idx);
	if (!sid) return 0;
	for (int i = 0; i < g_Cfg.num_admins; i++) {
		if (g_Cfg.admins[i][0] && strcmp(sid, g_Cfg.admins[i]) == 0) return 1;
	}
	return 0;
}

static void RelayToAdmins(const char *fmt, ...) {
	char msg[SC_LINE_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	if (g_Cfg.relay_console) ConPrint("%s\n", msg);

	for (int i = 1; i <= gpGlobals->maxclients; i++) {
		edict_t *p = INDEXENT(i);
		if (!p || p->free || !(p->v.flags & FL_CLIENT)) continue;
		if (IsAdminIdx(i)) TellClient(i, "%s", msg);
	}
}

/* ------------------------------------------------------------------ */
/* Log file                                                            */
/* ------------------------------------------------------------------ */

static void MakeDirTree(const char *path) {
	char tmp[256];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static void EnsureLog(void) {
	if (g_Log) return;
	MakeDirTree(g_Cfg.logdir);

	time_t now = time(NULL);
	struct tm lt; localtime_r(&now, &lt);
	const char *map = (gpGlobals && gpGlobals->mapname) ? STRING(gpGlobals->mapname) : "unknown";

	snprintf(g_LogName, sizeof(g_LogName), "%s/%s-%04d%02d%02d-%02d%02d%02d.log",
			 g_Cfg.logdir, map,
			 lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
			 lt.tm_hour, lt.tm_min, lt.tm_sec);
	g_Log = fopen(g_LogName, "a");
	if (g_Log) {
		setvbuf(g_Log, NULL, _IOLBF, 0);
		ConPrint(SC_TAG " logging to %s\n", g_LogName);
	} else {
		ConPrint(SC_TAG " WARNING: cannot open log %s\n", g_LogName);
	}
}

static void CloseLog(void) {
	if (g_Log) { fclose(g_Log); g_Log = NULL; }
}

static void LogRaw(int idx, const char *type, const char *text) {
	if (!g_Log) EnsureLog();
	if (!g_Log) return;
	time_t now = time(NULL);
	struct tm lt; localtime_r(&now, &lt);
	fprintf(g_Log, "[%02d:%02d:%02d] #%d <%s> %s | %s\n",
			lt.tm_hour, lt.tm_min, lt.tm_sec, idx, PlayerName(idx), type, text);
}

/* ------------------------------------------------------------------ */
/* Sanitize (strip CR/LF/NUL, escape control chars)                    */
/* ------------------------------------------------------------------ */

static void Sanitize(char *out, size_t outsz, const char *in) {
	size_t o = 0;
	if (outsz == 0) return;
	for (const char *p = in; *p && o + 1 < outsz; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '\r' || c == '\n' || c == 0) continue;
		if (c < 32 || c == 127) {
			if (o + 4 < outsz) {
				out[o++] = '\\'; out[o++] = 'x';
				out[o++] = "0123456789abcdef"[c >> 4];
				out[o++] = "0123456789abcdef"[c & 15];
			}
			continue;
		}
		out[o++] = (char)c;
	}
	out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* Cheat heuristics                                                    */
/* ------------------------------------------------------------------ */

static const char *kBadWords[] = {
	"wallhack","wall hack","aimbot","aim bot","autolock","auto lock",
	"triggerbot","trigger bot","silent aim","speedhack","speed hack",
	"bhop","chams","glow hack","radar hack","injector","hacked",
	"aimlock","no recoil","norecoil","anti-recoil","antirecoil",
	"rapid fire","rapidfire","supernova","deadclient","prohack",
	"discord.gg","mega.nz","t.me/",           /* overlay promo links */
	NULL
};

static int ContainsBadWord(const char *s) {
	static char low[SC_LINE_MAX];
	Sanitize(low, sizeof(low), s);
	for (char *q = low; *q; q++) *q = (char)tolower((unsigned char)*q);
	for (int i = 0; kBadWords[i]; i++) {
		if (strstr(low, kBadWords[i])) return i + 1;
	}
	return 0;
}

static int HasBinaryJunk(const char *s) {
	int weird = 0;
	for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
		if (*p >= 128) weird++;
		else if (*p < 32 && *p != '\t') weird += 2;
	}
	return weird > 6; /* tolerate normal color codes, flag heavy junk */
}

/* ------------------------------------------------------------------ */
/* Core capture entry                                                  */
/* ------------------------------------------------------------------ */

static void Capture(int idx, int type, const char *text) {
	if (!IsEnabled()) return;
	if (idx < 1 || idx > SC_MAX_CLIENTS || !text || !text[0]) return;

	g_TypeCnt[type < 8 ? type : 0]++;
	g_TotalCap[idx]++;

	/* flood detection */
	double now = NowSec();
	if (now - g_MsgTime[idx] < 1.0) {
		g_MsgBurst[idx]++;
		if (g_MsgBurst[idx] == (int)(g_Cfg.flood_per_sec * 2) && g_Cfg.alert_suspect) {
			RelayToAdmins(SC_TAG " SUSPECT #%d <%s>: message flood (%d hud msgs in 1s) - possible overlay spam",
						  idx, PlayerName(idx), g_MsgBurst[idx]);
			LogRaw(idx, "SUSPECT", "message flood");
			g_TypeCnt[7]++;
		}
	} else {
		g_MsgBurst[idx] = 0;
		g_MsgTime[idx] = now;
	}

	/* content heuristics */
	int badw = ContainsBadWord(text);
	int junk = HasBinaryJunk(text);
	if (g_Cfg.alert_suspect && (badw || junk)) {
		char clean[SC_LINE_MAX];
		Sanitize(clean, sizeof(clean), text);
		if (badw)
			RelayToAdmins(SC_TAG " SUSPECT #%d <%s>: keyword '%s' on screen: %.120s",
						  idx, PlayerName(idx), kBadWords[badw - 1], clean);
		else
			RelayToAdmins(SC_TAG " SUSPECT #%d <%s>: binary/colored junk on screen: %.120s",
						  idx, PlayerName(idx), clean);
		LogRaw(idx, "SUSPECT", clean);
		g_TypeCnt[7]++;
	}

	/* ring buffer (the "snapshot" source) */
	int cap = g_Cfg.hist_lines;
	int slot = g_HistHead[idx];
	g_Hist[idx][slot].t = now;
	g_Hist[idx][slot].type = type;
	Sanitize(g_Hist[idx][slot].line, SC_LINE_MAX, text);
	g_HistHead[idx] = (slot + 1) % cap;
	if (g_HistCount[idx] < cap) g_HistCount[idx]++;

	/* log */
	static const char *kTypeNames[] = {"SAY","TXT","CTR","HUD","STVAL","STXT","SCORE"};
	LogRaw(idx, kTypeNames[type < 7 ? type : 0], text);

	/* live watch relay */
	if (g_WatchedBy[idx]) {
		TellClient(g_WatchedBy[idx], "[SC:#%d|%s] %s", idx,
				   kTypeNames[type < 7 ? type : 0], g_Hist[idx][slot].line);
	}
}

/* ------------------------------------------------------------------ */
/* Message id resolution                                               */
/* ------------------------------------------------------------------ */

static void ResolveUserMsgIds(void) {
	g_SayTextId     = GET_MSG_ID("SayText");
	g_TextMsgId     = GET_MSG_ID("TextMsg");
	g_HudTextId     = GET_MSG_ID("HudText");
	g_HudMsgId      = GET_MSG_ID("HudMsg");
	g_StatusValueId = GET_MSG_ID("StatusValue");
	g_StatusTextId  = GET_MSG_ID("StatusText");
	g_ScoreInfoId   = GET_MSG_ID("ScoreInfo");
}

static int ClassifyMsg(int msgType) {
	if (msgType == g_SayTextId)     return 0;
	if (msgType == g_TextMsgId)     return 1;
	if (msgType == g_HudTextId)     return 2;
	if (msgType == g_HudMsgId)      return 3;
	if (msgType == g_StatusValueId) return 4;
	if (msgType == g_StatusTextId)  return 5;
	if (msgType == g_ScoreInfoId)   return 6;
	return -1;
}

static int CatchableFor(int cls) {
	switch (cls) {
		case 0: return g_Cfg.catch_saytext;
		case 1: return g_Cfg.catch_textmsg;
		case 2: return g_Cfg.catch_hudtext;
		case 3: return g_Cfg.catch_hudmsg;
		case 4: case 5: return g_Cfg.catch_status;
		case 6: return g_Cfg.catch_score;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Decode mirrored byte stream into readable text                      */
/* ------------------------------------------------------------------ */

struct MsgReader {
	const uchar *b;
	int len;
	int p;
	int err;
};

static int rByte(MsgReader *r) {
	if (r->p + 1 > r->len) { r->err = 1; return 0; }
	return r->b[r->p++];
}
static int rShort(MsgReader *r) {
	if (r->p + 2 > r->len) { r->err = 1; return 0; }
	int v = (short)(r->b[r->p] | (r->b[r->p + 1] << 8));
	r->p += 2;
	return v;
}
static int rLong(MsgReader *r) {
	if (r->p + 4 > r->len) { r->err = 1; return 0; }
	int v = (int)((unsigned)r->b[r->p] | ((unsigned)r->b[r->p+1] << 8) |
				  ((unsigned)r->b[r->p+2] << 16) | ((unsigned)r->b[r->p+3] << 24));
	r->p += 4;
	return v;
}
static void rSkip(MsgReader *r, int n) {
	if (r->p + n > r->len) { r->err = 1; return; }
	r->p += n;
}
static const char *rStr(MsgReader *r, char *tmp, size_t tmpsz) {
	size_t n = 0;
	tmp[0] = 0;
	while (r->p < r->len) {
		uchar c = r->b[r->p++];
		if (!c) return tmp;
		if (n + 1 < tmpsz) tmp[n++] = (char)c;
	}
	r->err = 1; /* unterminated */
	tmp[n] = 0;
	return tmp;
}

static void DecodeAndCapture(int receiverIdx, int msgType,
							 const uchar *buf, int len) {
	int cls = ClassifyMsg(msgType);
	if (cls < 0 || !CatchableFor(cls)) return;

	char out[SC_LINE_MAX];
	char tmp[SC_LINE_MAX];
	out[0] = 0;
	MsgReader r = { buf, len, 0, 0 };

	switch (cls) {
		case 0: /* SayText: byte talker, string text */
			rByte(&r);
			snprintf(out, sizeof(out), "%s", rStr(&r, tmp, sizeof(tmp)));
			break;
		case 1: /* TextMsg: byte dest, string */
			rByte(&r);
			snprintf(out, sizeof(out), "%s", rStr(&r, tmp, sizeof(tmp)));
			break;
		case 2: /* HudText: coord x,y + short sync + short hide + str */
			rSkip(&r, 12); rShort(&r); rShort(&r);
			snprintf(out, sizeof(out), "%s", rStr(&r, tmp, sizeof(tmp)));
			break;
		case 3: { /* HudMsg: x,y,effect(3 coords=12) + short effect + rgba + fades + params + fmt */
			rSkip(&r, 12);
			int fx = rShort(&r);
			rByte(&r); rByte(&r); rByte(&r); rByte(&r);          /* rgba */
			rShort(&r); rShort(&r); rShort(&r);                  /* fadein/hold/fadeout */
			int nparams = rByte(&r);
			if (nparams > 3) nparams = 3;
			for (int i = 0; i < nparams && !r.err; i++) {
				int pt = rByte(&r);
				if (pt == 0) rLong(&r);
				else if (pt == 1) rShort(&r);
				else if (pt == 2) rSkip(&r, 12);
				else if (pt == 3) rStr(&r, tmp, sizeof(tmp));
			}
			snprintf(out, sizeof(out), "[hudfx %d] %s", fx, rStr(&r, tmp, sizeof(tmp)));
			break;
		}
		case 4: { /* StatusValue: short slot + long value (+ optional string when slot==0) */
			int slot = rShort(&r);
			int val = rLong(&r);
			if (slot == 0) snprintf(out, sizeof(out), "statusval: %s", rStr(&r, tmp, sizeof(tmp)));
			else snprintf(out, sizeof(out), "statusval slot=%d val=%d", slot, val);
			break;
		}
		case 5: /* StatusText: short slot + string */
			snprintf(out, sizeof(out), "statustxt slot=%d %s",
					 rShort(&r), rStr(&r, tmp, sizeof(tmp)));
			break;
		case 6: { /* ScoreInfo row */
			int client = rShort(&r); rShort(&r);      /* index, userid */
			int frags = rShort(&r); rShort(&r);       /* frags, team */
			rShort(&r); rByte(&r);                    /* deaths, class */
			int health = rShort(&r);
			snprintf(out, sizeof(out), "score #%d %s F%d HP%d",
					 client, rStr(&r, tmp, sizeof(tmp)), frags, health);
			break;
		}
		default: return;
	}

	if (!r.err && out[0]) Capture(receiverIdx, cls, out);
}

/* ------------------------------------------------------------------ */
/* Engine hooks (PRE chain so we see every message before send)        */
/* ------------------------------------------------------------------ */

static void OnMessageBegin_PRE(int msgType, int msgDest, int arg1) {
	gpMetaGlobals->mres = MRES_IGNORED;

	if (g_RelayLevel > 0) return;            /* our own TellClient -> skip */
	if (!IsEnabled()) return;
	if (ClassifyMsg(msgType) < 0) return;    /* not interesting at all */

	g_MsgActive = 1;
	g_MsgType = msgType;
	g_MsgDest = msgDest;
	g_MsgArg1 = arg1;
	g_MsgBufLen = 0;
}

static void OnMessageEnd_PRE(void) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!g_MsgActive) return;
	g_MsgActive = 0;

	int receivers[SC_MAX_CLIENTS + 1];
	int nrec = 0;

	if (g_MsgDest == MSG_ONE || g_MsgDest == MSG_ONE_UNRELIABLE) {
		int slot = g_MsgArg1; /* MESSAGE_BEGIN passes the player slot here */
		if (slot >= 1 && slot <= gpGlobals->maxclients) {
			edict_t *p = INDEXENT(slot);
			if (p && !p->free) receivers[nrec++] = slot;
		}
	} else {
		for (int i = 1; i <= gpGlobals->maxclients && i <= SC_MAX_CLIENTS; i++) {
			edict_t *p = INDEXENT(i);
			if (p && !p->free && (p->v.flags & FL_CLIENT)) receivers[nrec++] = i;
		}
	}
	for (int rr = 0; rr < nrec; rr++)
		DecodeAndCapture(receivers[rr], g_MsgType, g_MsgBuf, g_MsgBufLen);
}

static void track(const void *data, int n) {
	if (!g_MsgActive) return;
	if (g_MsgBufLen + n > (int)sizeof(g_MsgBuf)) { g_MsgActive = 0; return; }
	memcpy(g_MsgBuf + g_MsgBufLen, data, n);
	g_MsgBufLen += n;
}

static void OnWriteByte_PRE(int v)    { gpMetaGlobals->mres = MRES_IGNORED; track(&v, 1); }
static void OnWriteChar_PRE(int v)    { gpMetaGlobals->mres = MRES_IGNORED; char c = (char)v; track(&c, 1); }
static void OnWriteShort_PRE(int v)   { gpMetaGlobals->mres = MRES_IGNORED; short s = (short)v; track(&s, 2); }
static void OnWriteLong_PRE(int v)    { gpMetaGlobals->mres = MRES_IGNORED; track(&v, 4); }
static void OnWriteAngle_PRE(float v) { gpMetaGlobals->mres = MRES_IGNORED; track(&v, 4); }
static void OnWriteCoord_PRE(float v) { gpMetaGlobals->mres = MRES_IGNORED; track(&v, 4); }
static void OnWriteEntity_PRE(int v)  { gpMetaGlobals->mres = MRES_IGNORED; track(&v, 2); }
static void OnWriteString_PRE(const char *s) {
	gpMetaGlobals->mres = MRES_IGNORED;
	if (!s) { char z = 0; track(&z, 1); return; }
	int l = (int)strlen(s);
	if (l > 255) l = 255;
	track(s, l);
	char z = 0; track(&z, 1);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int FindPlayerByNameOrId(const char *arg) {
	if (!arg || !arg[0]) return 0;
	if (arg[0] == '#') {
		int u = atoi(arg + 1);
		for (int i = 1; i <= gpGlobals->maxclients; i++)
			if (g_engfuncs.pfnGetPlayerUserId(i) == u) return i;
		return 0;
	}
	int slot = atoi(arg);
	if (slot >= 1 && slot <= gpGlobals->maxclients) {
		edict_t *p = INDEXENT(slot);
		if (p && !p->free && (p->v.flags & FL_CLIENT)) return slot;
	}
	int best = 0, hits = 0;
	for (int i = 1; i <= gpGlobals->maxclients; i++) {
		edict_t *p = INDEXENT(i);
		if (!p || p->free || !(p->v.flags & FL_CLIENT)) continue;
		if (stristr(STRING(p->v.netname), arg)) { best = i; hits++; }
	}
	return (hits == 1) ? best : 0;
}

static void DumpRecent(int toIdx, int target, int lines) {
	if (target < 1 || target > SC_MAX_CLIENTS) return;
	int cap = g_Cfg.hist_lines;
	int cnt = g_HistCount[target];
	int first = (g_HistHead[target] - cnt + cap * 2) % cap;
	int shown = 0;
	for (int i = 0; i < cnt && shown < lines; i++) {
		int k = (first + i) % cap;
		if (toIdx == 0)
			ConPrint("[SC-hist #%d] %s\n", target, g_Hist[target][k].line);
		else
			TellClient(toIdx, "[SC-hist #%d] %s", target, g_Hist[target][k].line);
		shown++;
	}
	if (shown == 0) {
		if (toIdx == 0) ConPrint("[SC] no captured lines for #%d yet\n", target);
		else TellClient(toIdx, "[SC] no captured lines for #%d yet", target);
	}
}

#define RESP(...) do { if (idx == 0) ConPrint(__VA_ARGS__); \
		else TellClient(idx, __VA_ARGS__); } while (0)

static bool HandleCommand(int idx /*0=console*/, const char *text) {
	char arg1[64] = "", arg2[64] = "";
	if (sscanf(text, "%63s %63s", arg1, arg2) < 1) return false;

	if (!stricmp(arg1, "sc_watch")) {
		if (idx == 0) { RESP("[SC] use sc_watch from an admin CLIENT (chat) for live feed\n"); return true; }
		int t = FindPlayerByNameOrId(arg2);
		if (!t) { RESP("[SC] player not found\n"); return true; }
		if (g_WatchSlot[idx]) g_WatchedBy[g_WatchSlot[idx]] = 0;
		g_WatchSlot[idx] = t; g_WatchedBy[t] = idx;
		RESP("[SC] watching #%d <%s> - live screen feed ON\n", t, PlayerName(t));
		return true;
	}
	if (!stricmp(arg1, "sc_unwatch")) {
		if (idx >= 1 && g_WatchSlot[idx]) {
			g_WatchedBy[g_WatchSlot[idx]] = 0;
			g_WatchSlot[idx] = 0;
		}
		RESP("[SC] unwatched\n");
		return true;
	}
	if (!stricmp(arg1, "sc_shot")) {
		int t = FindPlayerByNameOrId(arg2);
		if (!t) { RESP("[SC] player not found\n"); return true; }
		RESP("[SC] ===== SCREEN SNAPSHOT #%d <%s> =====\n", t, PlayerName(t));
		DumpRecent(idx, t, 24);
		return true;
	}
	if (!stricmp(arg1, "sc_dump")) {
		int t = FindPlayerByNameOrId(arg2);
		if (!t) { RESP("[SC] player not found\n"); return true; }
		DumpRecent(idx, t, SC_HIST_CAP);
		return true;
	}
	if (!stricmp(arg1, "sc_list")) {
		int any = 0;
		for (int i = 1; i <= SC_MAX_CLIENTS; i++) if (g_WatchSlot[i]) {
			RESP("[SC] watcher #%d -> target #%d <%s>\n", i, g_WatchSlot[i], PlayerName(g_WatchSlot[i]));
			any = 1;
		}
		if (!any) RESP("[SC] no active watches\n");
		return true;
	}
	if (!stricmp(arg1, "sc_stats")) {
		static const char *tn[] = {"SayText","TextMsg","HudText","HudMsg","StatusVal","StatusTxt","ScoreInfo"};
		for (int i = 0; i < 7; i++)
			RESP("[SC] %-9s %u\n", tn[i], g_TypeCnt[i]);
		RESP("[SC] suspect %u | log %s\n", g_TypeCnt[7], g_LogName);
		return true;
	}
	if (!stricmp(arg1, "sc_on"))  { g_Cfg.enabled = 1; RESP("[SC] enabled\n"); return true; }
	if (!stricmp(arg1, "sc_off")) { g_Cfg.enabled = 0; RESP("[SC] disabled\n"); return true; }
	return false;
}

/* ServerCmd hook: console/rcon "sc_xxx ..." and admin chat "say sc_xxx ..." */
static void OnServerCmd_PRE(void) {
	gpMetaGlobals->mres = MRES_IGNORED;
	const char *argv1 = CMD_ARGV(1);
	if (!argv1 || !argv1[0]) return;

	if (!strncmp(argv1, "sc_", 3)) {
		if (HandleCommand(0, argv1))
			gpMetaGlobals->mres = MRES_SUPERCEDE;
		return;
	}
	if (!stricmp(argv1, "say")) {
		const char *msg = CMD_ARGV(2);
		if (msg && !strncmp(msg, "sc_", 3)) {
			int idx = CURRENT_PLAYER;
			if (idx >= 1 && IsAdminIdx(idx)) {
				if (HandleCommand(idx, msg))
					gpMetaGlobals->mres = MRES_SUPERCEDE; /* swallow chat line */
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* StartFrame: hot reload                                              */
/* ------------------------------------------------------------------ */

static void OnStartFrame_POST(void) {
	gpMetaGlobals->mres = MRES_IGNORED;

	double now = NowSec();
	if (now - g_LastCfgCheck >= 2.0) {
		g_LastCfgCheck = now;
		struct stat st;
		if (g_CfgPath[0]) {
			if (stat(g_CfgPath, &st) == 0 && st.st_mtime != g_CfgMtime) {
				if (LoadConfig(true)) ConPrint(SC_TAG " config reloaded\n");
			}
		}
	}
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
	pFunctionTable->pfnServerCmd = OnServerCmd_PRE;
	return TRUE;
}

C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion) {
	if (!pFunctionTable || !interfaceVersion || *interfaceVersion != INTERFACE_VERSION) {
		if (interfaceVersion) *interfaceVersion = INTERFACE_VERSION;
		return FALSE;
	}
	memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
	pFunctionTable->pfnStartFrame = OnStartFrame_POST;
	return TRUE;
}

C_DLLEXPORT int GetEngineFunctions(enginefuncs_t *pengfuncsFromEngine, int *interfaceVersion) {
	if (!pengfuncsFromEngine || !interfaceVersion) return FALSE;
	if (*interfaceVersion != ENGINE_INTERFACE_VERSION) {
		*interfaceVersion = ENGINE_INTERFACE_VERSION;
		return FALSE;
	}
	memset(pengfuncsFromEngine, 0, sizeof(enginefuncs_t));
	pengfuncsFromEngine->pfnMessageBegin = OnMessageBegin_PRE;
	pengfuncsFromEngine->pfnMessageEnd = OnMessageEnd_PRE;
	pengfuncsFromEngine->pfnWriteByte = OnWriteByte_PRE;
	pengfuncsFromEngine->pfnWriteChar = OnWriteChar_PRE;
	pengfuncsFromEngine->pfnWriteShort = OnWriteShort_PRE;
	pengfuncsFromEngine->pfnWriteLong = OnWriteLong_PRE;
	pengfuncsFromEngine->pfnWriteAngle = OnWriteAngle_PRE;
	pengfuncsFromEngine->pfnWriteCoord = OnWriteCoord_PRE;
	pengfuncsFromEngine->pfnWriteEntity = OnWriteEntity_PRE;
	pengfuncsFromEngine->pfnWriteString = OnWriteString_PRE;
	ConPrint(SC_TAG " engine message hooks registered\n");
	return TRUE;
}

META_FUNCTIONS gMetaFunctionTable = {
	NULL,                 /* pfnGetEntityAPI */
	NULL,                 /* pfnGetEntityAPI_Post */
	GetEntityAPI2,        /* pfnGetEntityAPI2 */
	GetEntityAPI2_Post,   /* pfnGetEntityAPI2_Post */
	NULL, NULL,           /* pfnGetNewDLLFunctions(_Post) */
	GetEngineFunctions,   /* pfnGetEngineFunctions */
	NULL                  /* pfnGetEngineFunctions_Post */
};

C_DLLEXPORT void GiveFnptrsToDll(enginefuncs_t *pengfuncs, globalvars_t *pGlobals) {
	memcpy(&g_engfuncs, pengfuncs, sizeof(enginefuncs_t));
	gpGlobals = pGlobals;
}

C_DLLEXPORT int Meta_Query(char *interfaceVersion, plugin_info_t **plinfo, mutil_funcs_t *pMetaUtilFuncs) {
	if (!interfaceVersion || !plinfo) return FALSE;
	if (strcmp(interfaceVersion, META_INTERFACE_VERSION) != 0) return FALSE;
	g_pMetaUtil = pMetaUtilFuncs;
	*plinfo = &Plugin_info;
	return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS *pFunctionTable,
							meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs) {
	if (!pMGlobals || !pFunctionTable) return FALSE;
	if (now > SC_LOAD_TIME) return FALSE;

	gpMetaGlobals = pMGlobals;
	gpGamedllFuncs = pGamedllFuncs;
	memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));

	bool found = LoadConfig(false);
	ResolveUserMsgIds();

	memset(g_WatchSlot, 0, sizeof(g_WatchSlot));
	memset(g_WatchedBy, 0, sizeof(g_WatchedBy));
	memset(g_HistHead, 0, sizeof(g_HistHead));
	memset(g_HistCount, 0, sizeof(g_HistCount));
	memset(g_MsgBurst, 0, sizeof(g_MsgBurst));
	memset(g_TotalCap, 0, sizeof(g_TotalCap));
	memset(g_TypeCnt, 0, sizeof(g_TypeCnt));
	g_LastCfgCheck = NowSec();

	EnsureLog();

	ConPrint(SC_TAG " v1.0 loaded (%s) admins=%d log=%s\n",
			 found ? g_CfgPath : "defaults", g_Cfg.num_admins, g_LogName);
	return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason) {
	g_Cfg.enabled = 0;
	CloseLog();
	return TRUE;
}
