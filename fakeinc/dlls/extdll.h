/* minimal stand-in for HLSDK extdll.h, only for syntax checking */
#ifndef EXTDLL_H
#define EXTDLL_H
#include <stddef.h>
typedef int BOOL; typedef int qboolean; typedef char byte; typedef unsigned short u_short; typedef unsigned long ulong;
#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)
struct entvars { int flags; float deadflag; char noz[512]; };
typedef struct edict_s { int free; entvars v; } edict_t;
enum { FL_CLIENT=32768, FL_FAKECLIENT=8192, FL_PROXY=33554432, DEAD_NO=0 };
typedef struct globalvars { float time; int maxClients; const char *pStringBase; } globalvars_t;
typedef struct cvar_s { const char *name; const char *string; float value; int flags; void (*next)(struct cvar_s); } cvar_t;
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
const char* (*pfnGetPlayerAuthId)(edict_t*);
void (*pfnClientCommand)(edict_t*, char*, ...);
void (*pfnGetPlayerStats)(const edict_t*, int*, int*);
void (*pfnServerCommand)(char*);
void (*pfnCVarSetString)(const char*, const char*);
qboolean (*pfnClientConnect)(edict_t*, const char*, const char*, char[128]);
} enginefuncs_t;
#define INDEXENT(x) (&pEdictListPlusOne[(x)])
extern edict_t *pEdictListPlusOne;
#define STRING(x) (x)
typedef struct NEW_DLL_FUNCTIONS { void *dummy; } NEW_DLL_FUNCTIONS;
typedef struct DLL_FUNCTIONS { void (*pfnStartFrame)(void); qboolean (*pfnClientConnect)(edict_t*, const char*, const char*, char[128]); void (*pfnClientCommand)(edict_t*); void (*pfnClientPutInServer)(edict_t*); void (*pfnClientDisconnect)(edict_t*); void (*pfnServerActivate)(edict_t*,int,int); void (*pfnServerDeactivate)(void); } DLL_FUNCTIONS;
#define INTERFACE_VERSION 140
typedef struct mutil_funcs { void (*pfnAddCommand)(const char*, void(*)()); } mutil_funcs_t;
typedef enum PLUG_LOADTIME { PT_STARTUP, PT_ANYTIME } PLUG_LOADTIME;
typedef enum PL_UNLOAD_REASON { PNL_CMDUNLOAD } PL_UNLOAD_REASON;
typedef struct plugin_info { const char *interfaceVer; const char *name; const char *version; const char *date;
const char *author; const char *url; const char *logtag; PLUG_LOADTIME load;
PLUG_LOADTIME unload; } plugin_info_t;
#define META_INTERFACE_VERSION "5:14"
typedef struct gamedll_funcs { DLL_FUNCTIONS *dllapi_table; DLL_FUNCTIONS *newapi_table; } gamedll_funcs_t;
typedef struct meta_globals { int mres; } meta_globals_t;
enum { MRES_IGNORED, MRES_SUPERCEDE };
/* forward typedefs matching metamod h_export.h */
typedef int (*FN_PfnGetEntityAPI)(DLL_FUNCTIONS*, int*);
typedef int (*FN_PfnGetEntityAPI_Post)(DLL_FUNCTIONS*, int*);
typedef int (*FN_PfnGetEntityAPI2)(DLL_FUNCTIONS*, int*);
typedef int (*FN_PfnGetEntityAPI2_Post)(DLL_FUNCTIONS*, int*);
typedef int (*FN_PfnGetNewDLLFunctions)(NEW_DLL_FUNCTIONS*, int*);
typedef int (*FN_PfnGetNewDLLFunctions_Post)(NEW_DLL_FUNCTIONS*, int*);
typedef int (*FN_PfnGetEngineFunctions)(enginefuncs_t*, int*);
typedef int (*FN_PfnGetEngineFunctions_Post)(enginefuncs_t*, int*);
typedef struct META_FUNCTIONS {
FN_PfnGetEntityAPI pfnGetEntityAPI;
FN_PfnGetEntityAPI_Post pfnGetEntityAPI_Post;
FN_PfnGetEntityAPI2 pfnGetEntityAPI2;
FN_PfnGetEntityAPI2_Post pfnGetEntityAPI2_Post;
FN_PfnGetNewDLLFunctions pfnGetNewDLLFunctions;
FN_PfnGetNewDLLFunctions_Post pfnGetNewDLLFunctions_Post;
FN_PfnGetEngineFunctions pfnGetEngineFunctions;
FN_PfnGetEngineFunctions_Post pfnGetEngineFunctions_Post;
} META_FUNCTIONS;
#define C_DLLEXPORT extern "C" __attribute__((visibility("default")))
#define MSG_ONE 1
#define MESSAGE_BEGIN(t,i,x,e) gpMetaGlobals,(t),(i),(x),(e),g_engfuncs.pfnMessageBegin((t),(i),(x),(e))
#define WRITE_BYTE(x) g_engfuncs.pfnWriteByte(x)
#define MESSAGE_END() g_engfuncs.pfnMessageEnd()
#endif
