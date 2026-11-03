/* minimal stand-in for ReHLDS hookchain types, syntax-check only */
#ifndef REHLDS_IFACE_H
#define REHLDS_IFACE_H
#include <stddef.h>
typedef struct sizebuf_s { const char *buffername; unsigned char *data; int maxsize; int cursize; int flags; } sizebuf_t;
class IGameClient { public: virtual bool IsActive(); virtual edict_t* GetEdict(); };
class IRehldsHook_SV_EmitPings { public: virtual void callNext(IGameClient*, sizebuf_t*); };
class IRehldsHookchains { public: virtual IRehldsHook_SV_EmitPings* SV_EmitPings(); };
struct RehldsFuncs_t {
void (*MSG_WriteByte)(sizebuf_t*, int);
void (*MSG_StartBitWriting)(sizebuf_t*);
void (*MSG_WriteBits)(unsigned int, int);
void (*MSG_EndBitWriting)(sizebuf_t*);
};
class IRehldsServerStatic { public: virtual IGameClient* GetClient(int); };
class IRehldsApi { public: virtual int GetMajorVersion(); virtual int GetMinorVersion();
virtual const RehldsFuncs_t* GetFuncs(); virtual IRehldsHookchains* GetHookchains();
virtual IRehldsServerStatic* GetServerStatic(); };
#define VREHLDS_HLDS_API_VERSION "VRehlds001"
#define REHLDS_API_VERSION_MAJOR 1
#define REHLDS_API_VERSION_MINOR 0
#endif
