/* minimal stand-in for HLSDK extdll.h, only for syntax checking */
#ifndef EXTDLL_H
#define EXTDLL_H
#include <stddef.h>
typedef int BOOL; typedef char byte; typedef unsigned short u_short; typedef unsigned long ulong;
#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)
struct entvars { int flags; float deadflag; char netname[32]; };
typedef int string_t;
typedef struct edict_s { entvars v; } edict_t;
enum { FL_CLIENT=32768, FL_FAKECLIENT=8192, DEAD_NO=0 };
typedef struct globalvars { float time; int maxClients; const char *pStringBase; } globalvars_t;
typedef struct cvar { float value; } cvar_t;
typedef struct TraceResult { int fAllSolid; } TraceResult;
typedef struct enginefuncs {
void (*pfnServerPrint)(const char*);
int (*pfnCmd_Argc)(void);
const char* (*pfnCmd_Argv)(int);
edict_t* (*pfnPEntityOfEntIndex)(int);
int (*pfnIndexOfEdict)(const edict_t*);
int (*pfnGetPlayerUserId)(edict_t*);
void (*pfnClientPrintf)(edict_t*, int, const char*);
void (*pfnMessageBegin)(int, int, const int*, edict_t*);
void (*pfnMessageEnd)(void);
void (*pfnWriteByte)(int);
cvar_t* (*pfnCVarGetPointer)(const char*);
void (*pfnCVarSetFloat)(const char*, float);
void (*pfnAddCommand)(const char*, void(*)());
} enginefuncs_t;
#define STRING(x) ((const char*)(x))
typedef struct DLL_FUNCTIONS { void (*pfnStartFrame)(void); void (*pfnClientCommand)(edict_t*); void (*pfnClientPutInServer)(edict_t*); void (*pfnClientDisconnect)(edict_t*); void (*pfnServerActivate)(edict_t*,int,int); } DLL_FUNCTIONS;
#define INTERFACE_VERSION 140
typedef struct mutil_funcs { int dummy; } mutil_funcs_t;
typedef enum PL_UNLOAD_REASON { PNL_CMDUNLOAD } PL_UNLOAD_REASON;
typedef enum PLUG_LOADTIME { PT_STARTUP, PT_ANYTIME } PLUG_LOADTIME;
typedef struct plugin_info { const char *name; const char *version; const char *date; const char *author; const char *url; const char *logtag; PLUG_LOADTIME loadable; PLUG_LOADTIME unloadable; } plugin_info_t;
#define META_INTERFACE_VERSION "5:14"
typedef struct gamedll_funcs { DLL_FUNCTIONS *dllapi_table; DLL_FUNCTIONS *newapi_table; } gamedll_funcs_t;
typedef struct meta_globals { int mres; } meta_globals_t;
enum { MRES_IGNORED, MRES_SUPERCEDE };
typedef struct META_FUNCTIONS { const void *f[8]; } META_FUNCTIONS;
#define C_DLLEXPORT extern "C" __attribute__((visibility("default")))
#define MSG_ONE 1
#define MESSAGE_BEGIN(t,i,x,e) gpMetaGlobals,(t),(i),(x),(e),g_engfuncs.pfnMessageBegin((t),(i),(x),(e))
#define WRITE_BYTE(x) g_engfuncs.pfnWriteByte(x)
#define MESSAGE_END() g_engfuncs.pfnMessageEnd()
#endif
