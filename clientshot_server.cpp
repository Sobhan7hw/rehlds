/*
 * ClientShot 1.0 - SERVER metamod plugin (CS 1.6 / ReHLDS / revEmu)
 * by FaTaL-[Hit] - https://t.me/Neo_Norm
 */

#include <extdll.h>
#include <meta_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define CSH_TAG "[ClientShot]"
#define CSH_MAX_CLIENTS 32
#define CSH_CHUNK_BYTES 4096          /* must match client side */

/* outgoing usermsg name (registered by BOTH sides) */
#define MSG_CSH_REQ "CShRq"

enum { REQ_SCREENSHOT = 1, REQ_PROC = 2, REQ_HWID = 4 };

struct CshCfg {
int   auto_scan_on_join;
int   interval_sec;
char  watchlist[512];
int   max_shot_kb;
char  save_dir[256];
int   debug;
} g_Cfg;

static char   g_CfgPath[256] = "";
static time_t g_CfgMtime = 0;
static int    g_On = 0;
static double g_LastCfgCheck = 0.0;
static double g_NextAutoScan = 0.0;

struct SlotState {
int   connected;
float joinTime;
int   hasModule;        /* got at least one cs_* command from this slot */
int   pendingReq;
double reqSentAt;
unsigned char *buf;
size_t bufCap;
size_t highWater;       /* highest byte written so far */
int   chunksGot, chunksExpected;
};
static SlotState g_Slot[CSH_MAX_CLIENTS + 1];

/* ------------------------------------------------------------------ */
static double NowSec() {
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

static void trim(char *s) {
char *p = s;
while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
if (p!=s) memmove(s,p,strlen(p)+1);
size_t n=strlen(s);
while(n>0&&(s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n]=0;
}

/* ---------------- config ------------------------------------------- */
static int try_load_config(const char *path) {
FILE *f=fopen(path,"r"); if(!f) return 0;
char line[600];
while(fgets(line,sizeof(line),f)){
trim(line);
if(!line[0]||line[0]==';'||line[0]=='#') continue;
char *eq=strchr(line,'='); if(!eq) continue;
*eq=0; char *k=line,*v=eq+1;
for(char*c=v;*c;c++) if(*c==';'||*c=='#'){*c=0;break;}
trim(k); trim(v);
if      (!strcmp(k,"auto_scan_on_join")) g_Cfg.auto_scan_on_join=atoi(v);
else if (!strcmp(k,"interval_sec"))      g_Cfg.interval_sec=atoi(v);
else if (!strcmp(k,"watchlist"))         strncpy(g_Cfg.watchlist,v,sizeof(g_Cfg.watchlist)-1);
else if (!strcmp(k,"max_shot_kb"))       g_Cfg.max_shot_kb=atoi(v);
else if (!strcmp(k,"save_dir"))          strncpy(g_Cfg.save_dir,v,sizeof(g_Cfg.save_dir)-1);
else if (!strcmp(k,"debug"))             g_Cfg.debug=atoi(v);
}
fclose(f);
strncpy(g_CfgPath,path,sizeof(g_CfgPath)-1); g_CfgPath[255]=0;
struct stat st; g_CfgMtime=(stat(path,&st)==0)?st.st_mtime:0;
return 1;
}

static void SetDefaults() {
memset(&g_Cfg,0,sizeof(g_Cfg));
g_Cfg.auto_scan_on_join=1;
g_Cfg.interval_sec=300;
strcpy(g_Cfg.watchlist,"cheat,hack,wallhack,aimbot,esp,trainer,wgleak,sensui,protest,artemis,evolution,xigncode,knights,infinite");
g_Cfg.max_shot_kb=800;
strcpy(g_Cfg.save_dir,"cstrike/clientshot");
g_Cfg.debug=1;
}

static bool LoadConfig(bool reload) {
SetDefaults();
if(reload&&g_CfgPath[0]) return try_load_config(g_CfgPath)!=0;
const char *paths[]={
"addons/clientshot/clientshot.ini",
"cstrike/addons/clientshot/clientshot.ini",
"../cstrike/addons/clientshot/clientshot.ini",
"clientshot.ini"};
for(size_t i=0;i<sizeof(paths)/sizeof(paths[0]);i++)
if(try_load_config(paths[i])) return true;
return false;
}

/* ---------------- storage ------------------------------------------- */
static void EnsureDir(const char *d) {
#ifdef _WIN32
mkdir(d);
#else
mkdir(d,0755);
#endif
}

static void SaveShot(int idx,const unsigned char*data,size_t len) {
EnsureDir(g_Cfg.save_dir);
char path[512]; time_t t=time(NULL); struct tm tv; localtime_r(&t,&tv);
snprintf(path,sizeof(path),"%s/p%02d_%04d%02d%02d_%02d%02d%02d.bmp",
g_Cfg.save_dir,idx,tv.tm_year+1900,tv.tm_mon+1,tv.tm_mday,
tv.tm_hour,tv.tm_min,tv.tm_sec);
FILE*f=fopen(path,"wb");
if(f){fwrite(data,1,len,f);fclose(f);ConPrint(CSH_TAG " saved %s (%lu bytes)\n",path,(unsigned long)len);}
else ConPrint(CSH_TAG " FAILED writing %s\n",path);
}

/* ---------------- request out (server -> client module) -------------- */
static void SendRequest(int idx,int flags) {
edict_t *pl=INDEXENT(idx+1);
if(!pl||pl->free||!(pl->v.flags&FL_CLIENT)) return;
int msgId=GET_USER_MSG_ID(PLID,MSG_CSH_REQ,NULL);
if(msgId<=0){ConPrint(CSH_TAG " usermsg '%s' not registered\n",MSG_CSH_REQ);return;}
MESSAGE_BEGIN(MSG_ONE,msgId,NULL,pl);
WRITE_BYTE(flags);
WRITE_LONG((long)time(NULL));
MESSAGE_END();
g_Slot[idx].pendingReq=flags;
g_Slot[idx].reqSentAt=NowSec();
if(g_Cfg.debug) ConPrint(CSH_TAG " -> req flags=%d slot=%d\n",flags,idx);
}

/* ---------------- incoming data (client module -> server) ----------- */
static void OnClientCommand(edict_t *pl) {
const char *cmd=CMD_ARGV(0);
if(strncmp(cmd,"cs_",3)!=0) return;               /* our private namespace */
int idx=INDEX(pl);
if(idx<1||idx>CSH_MAX_CLIENTS) return;
SlotState *s=&g_Slot[idx];
s->hasModule=1; s->pendingReq=0;

if(!strcmp(cmd,"cs_hwid")) {
ConPrint(CSH_TAG " HWID slot %d: %s\n",idx,CMD_ARGV(1));
} else if(!strcmp(cmd,"cs_proclist")) {
const char *names=CMD_ARGV(1);
ConPrint(CSH_TAG " PROCS slot %d: %s\n",idx,names);
char wl[512]; strncpy(wl,g_Cfg.watchlist,sizeof(wl)-1); wl[511]=0;
char *tok=strtok(wl,",");
while(tok){
while(*tok==' ')tok++;
if(*tok&&strstr(names,tok))
ConPrint(CSH_TAG " !! WATCHLIST HIT slot %d ('%s')\n",idx,tok);
tok=strtok(NULL,",");
}
} else if(!strcmp(cmd,"cs_shot_begin")) {
long total=atol(CMD_ARGV(1)); int chunks=atoi(CMD_ARGV(2));
free(s->buf); s->buf=NULL; s->bufCap=0; s->highWater=0; s->chunksGot=0;
if(total>0&&total<=(long)g_Cfg.max_shot_kb*1024){
s->buf=(unsigned char*)malloc(total); s->bufCap=(size_t)total;
s->chunksExpected=chunks;
ConPrint(CSH_TAG " shot begin slot %d: %ld B / %d chunks\n",idx,total,chunks);
} else ConPrint(CSH_TAG " shot REJECTED (size %ld) slot %d\n",total,idx);
} else if(!strcmp(cmd,"cs_shot_chunk")) {
if(!s->buf) return;
int seq=atoi(CMD_ARGV(1)); const char *hex=CMD_ARGV(2);
size_t need=strlen(hex)/2;
size_t off=(size_t)seq*CSH_CHUNK_BYTES;
if(seq>=0&&off+need<=s->bufCap){
for(size_t i=0;i<need;i++){
char bb[3]={hex[i*2],hex[i*2+1],0};
s->buf[off+i]=(unsigned char)strtol(bb,NULL,16);
}
if(off+need>s->highWater) s->highWater=off+need;
s->chunksGot++;
if(s->chunksGot>=s->chunksExpected)
SaveShot(idx,s->buf,s->highWater);
}
} else if(!strcmp(cmd,"cs_shot_end")) {
ConPrint(CSH_TAG " shot end slot %d (%d/%d)\n",idx,s->chunksGot,s->chunksExpected);
}
}

/* ---------------- lifecycle ----------------------------------------- */
static void OnClientPutInServer_Post(edict_t *pl) {
int idx=INDEX(pl);
if(idx<1||idx>CSH_MAX_CLIENTS) return;
free(g_Slot[idx].buf);
memset(&g_Slot[idx],0,sizeof(SlotState));
g_Slot[idx].connected=1;
g_Slot[idx].joinTime=gpGlobals->time;
if(g_Cfg.auto_scan_on_join)
SendRequest(idx,REQ_HWID|REQ_PROC);
}

static void OnClientDisconnect_Post(edict_t *pl) {
int idx=INDEX(pl);
if(idx<1||idx>CSH_MAX_CLIENTS) return;
if(g_Slot[idx].connected&&!g_Slot[idx].hasModule)
ConPrint(CSH_TAG " slot %d left WITHOUT ever answering (module blocked?)\n",idx);
free(g_Slot[idx].buf); g_Slot[idx].buf=NULL;
g_Slot[idx].connected=0;
}

static void OnStartFrame() {
if(!g_On) return;
double now=NowSec();
if(now-g_LastCfgCheck>=2.0){
g_LastCfgCheck=now;
struct stat st;
if(g_CfgPath[0]&&stat(g_CfgPath,&st)==0&&st.st_mtime!=g_CfgMtime){
if(LoadConfig(true)) ConPrint(CSH_TAG " config reloaded\n");
else g_CfgMtime=st.st_mtime;
}
}
if(g_Cfg.interval_sec>0&&now>=g_NextAutoScan){
g_NextAutoScan=now+g_Cfg.interval_sec;
for(int i=1;i<=CSH_MAX_CLIENTS;i++)
if(g_Slot[i].connected) SendRequest(i,REQ_SCREENSHOT|REQ_PROC);
}
for(int i=1;i<=CSH_MAX_CLIENTS;i++){
if(g_Slot[i].pendingReq&&now-g_Slot[i].reqSentAt>20.0){
ConPrint(CSH_TAG " TIMEOUT slot %d no answer (client module missing/blocked)\n",i);
g_Slot[i].pendingReq=0;
}
}
}

/* ---------------- admin commands ------------------------------------- */
static edict_t *FindUserid(int userid) {
for(int i=1;i<=CSH_MAX_CLIENTS;i++){
edict_t*e=INDEXENT(i);
if(e&&!e->free&&(e->v.flags&FL_CLIENT)&&GET_USERID(e)==userid) return e;
}
return NULL;
}
static void Cmd_Shot(const char *args) {
int u=atoi(args); edict_t*e=u?FindUserid(u):NULL;
if(!e){ConPrint(CSH_TAG " usage: clientshot_shot <userid>\n");return;}
SendRequest(INDEX(e),REQ_SCREENSHOT|REQ_PROC);
}
static void Cmd_Proc(const char *args) {
int u=atoi(args); edict_t*e=u?FindUserid(u):NULL;
if(!e){ConPrint(CSH_TAG " usage: clientshot_proc <userid>\n");return;}
SendRequest(INDEX(e),REQ_PROC);
}
static void Cmd_List(const char *args) {
(void)args;
for(int i=1;i<=CSH_MAX_CLIENTS;i++){
edict_t*e=INDEXENT(i);
if(e&&!e->free&&(e->v.flags&FL_CLIENT))
ConPrint(CSH_TAG " slot %d userid %d module=%s\n",i,GET_USERID(e),
g_Slot[i].hasModule?"YES":"NO");
}
}

/* ---------------- metamod glue ---------------------------------------- */
plugin_info_t Plugin_info = {
META_INTERFACE_VERSION,"ClientShot","1.0",__DATE__,"FaTaL-[Hit]",
"https://t.me/Neo_Norm","CSH",PT_STARTUP,PT_ANYTIME
};
enginefuncs_t g_engfuncs; globalvars_t *gpGlobals=NULL;
meta_globals_t *gpMetaGlobals=NULL; gamedll_funcs_t *gpGamedllFuncs=NULL;

C_DLLEXPORT void GiveFnptrsToDll(enginefuncs_t *peng,globalvars_t *pg){
memcpy(&g_engfuncs,peng,sizeof(enginefuncs_t)); gpGlobals=pg;
}
C_DLLEXPORT int Meta_Query(char *iv,plugin_info_t **pi,mutil_funcs_t *mu){
(void)mu;
if(!iv||strcmp(iv,META_INTERFACE_VERSION)!=0) return FALSE;
*pi=&Plugin_info; return TRUE;
}
C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS*t,int*v){
if(!t||!v||*v!=INTERFACE_VERSION)return FALSE;
memset(t,0,sizeof(*t));
t->pfnStartFrame=OnStartFrame;
t->pfnClientCommand=OnClientCommand;
return TRUE;
}
C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS*t,int*v){
if(!t||!v||*v!=INTERFACE_VERSION)return FALSE;
memset(t,0,sizeof(*t));
t->pfnClientPutInServer=OnClientPutInServer_Post;
t->pfnClientDisconnect=OnClientDisconnect_Post;
return TRUE;
}
META_FUNCTIONS gMetaFunctionTable={
NULL,NULL,GetEntityAPI2,GetEntityAPI2_Post,NULL,NULL,NULL,NULL
};

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now,META_FUNCTIONS*ft,
meta_globals_t *mg,gamedll_funcs_t *gd){
if(!ft||!mg) return FALSE;
if(now>Plugin_info.load) return FALSE;
gpMetaGlobals=mg; gpGamedllFuncs=gd;
memcpy(ft,&gMetaFunctionTable,sizeof(META_FUNCTIONS));

bool found=LoadConfig(false);
memset(g_Slot,0,sizeof(g_Slot));
g_LastCfgCheck=NowSec(); g_NextAutoScan=0.0;

REG_USER_MSG(PLID,MSG_CSH_REQ,"cl",TRUE,FALSE); /* byte flags + long nonce */
pfnAddCommand("clientshot_shot",Cmd_Shot);
pfnAddCommand("clientshot_proc",Cmd_Proc);
pfnAddCommand("clientshot_list",Cmd_List);

g_On=1;
ConPrint(CSH_TAG " Version: %s Linux 'Counter-Strike 1.6'\n", Plugin_info.version);
ConPrint(CSH_TAG " Success: Configuration executed%s.\n", found ? "" : " (no ini file, defaults in use)");
ConPrint(CSH_TAG " Author: t.me/Neo_Norm\n");
return TRUE;
}
C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now,PL_UNLOAD_REASON r){
(void)now;(void)r; g_On=0; return TRUE;
}
