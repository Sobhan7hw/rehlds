/*
 * ============================================================================
 *  ClientShot v1.0 - CLIENT MODULE (Windows, compiled as clientshot.dll)
 * ----------------------------------------------------------------------------
 *  WHAT THIS IS:
 *   A tiny win32 DLL that runs INSIDE the CS 1.6 client process. It is NOT a
 *   metamod plugin and NOT a cl_dll replacement. You load it with one of:
 *     a) your own launcher: CreateProcess("hl.exe ...") then inject, or simply
 *        launch "game.exe -insecure +cl_loadclientshot"-style helper exe that
 *        LoadLibrary()s it before starting the engine;
 *     b) rename to something innocuous and place in cstrike folder, loaded via
 *        a custom cl_dll proxy (advanced - beyond this file's scope).
 *   IMPORTANT: HL file download sends MODELS/SPR/SND/etc only - the engine
 *   NEVER executes downloaded .dll files (that would be a security hole every
 *   cheater would exploit). So "auto-download a dll on connect" is not
 *   possible through legit engine channels. The realistic deployment is a
 *   server-branded launcher (like most Iranian/EU anti-cheat servers do).
 *
 *  WHAT IT DOES when the server sends usermsg "CShRq":
 *    REQ_HWID(4):   reads MachineGuid from registry -> "cs_hwid <guid>"
 *    REQ_PROC(2):   Toolhelp32 snapshot -> "cs_proclist name1,name2,..."
 *    REQ_SCREEN(1): BitBlt screen -> downscale to 8-bit BMP -> chunked
 *                   "cs_shot_begin <bytes> <chunks>" /
 *                   "cs_shot_chunk <seq> <hex>" xN / "cs_shot_end"
 *  Commands go out with __argv engine cvar? No - we call the client's own
 *  "cmdline" by writing into the HL console via GetProcAddress on the engine
 *  export stubs... Simpler & standard: use the exported CL_Functions table?
 *  For portability we instead use the classic trick: post the commands through
 *  the game's client console using keybind injection is unreliable, so this
 *  module uses the HL1 client-side engine function pfnClientCommand exposed by
 *  the VGUI/hlclient exports (gEngfuncs->pCmd_*) IF you compile against the CS
 *  1.6 SDK (cl_exports.h). The code below assumes the standard HL SDK cl_dll
 *  environment (enginefuncs.h from the client side). If you build it outside a
 *  cl_dll, replace SendCmd() with your launcher's TCP side-channel instead.
 *
 *  CHEAT-DETECTION HONESTY:
 *   - This lists processes and grabs the real desktop. Overlays like ESP are
 *     visible in the screenshot. Launchers/injectors appear in proc list.
 *   - Kernel-mode cheats and renamed procs defeat it. Use as evidence only.
 * ============================================================================
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* ---- if building inside a cl_dll, uncomment and include the client SDK:
#include "hud.h"          // CHud, GetClientTools()->Cmd() etc.
------------------------------------------------------------------------ */

#define CSH_CHUNK_BYTES 4096          /* must match server side */
#define MSG_CSH_REQ "CShRq"
enum { REQ_SCREENSHOT=1, REQ_PROC=2, REQ_HWID=4 };

/* ------------------------------------------------------------------ */
/* Outgoing command channel.                                           */
/* In a cl_dll build: gEngfuncs.pfnClientCommand("cs_...");           */
/* Standalone build:  the simplest working transport is to write the  */
/* command into the game console via SendMessage to the HL window     */
/* ("CMDLINE" edit) - fragile. RECOMMENDED: implement SendCmd() with   */
/* your launcher's TCP relay. A stub is provided that prints locally.  */
static void SendCmd(const char *fmt, ...) {
char buf[CSH_CHUNK_BYTES*2+64];
va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
#ifdef CSH_IN_CLDLL
gEngfuncs.pfnClientCommand(buf);      /* uncomment for cl_dll builds */
#else
OutputDebugStringA("[CSH-client] "); OutputDebugStringA(buf);
/* TODO: forward 'buf' through your launcher socket here */
#endif
}

/* ------------------------------------------------------------------ */
/* HWID                                                                */
static void GetHWID(char*out,size_t cap){
out[0]=0;
HKEY hk; if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,
"SOFTWARE\\Microsoft\\Cryptography",0,KEY_READ|KEY_WOW64_64KEY,&hk)==ERROR_SUCCESS){
DWORD t=REG_SZ,sz=(DWORD)cap;
if(RegQueryValueExA(hk,"MachineGuid",NULL,&t,(LPBYTE)out,&sz)!=ERROR_SUCCESS) out[0]=0;
RegCloseKey(hk);
}
if(!out[0]){ DWORD sz=(DWORD)cap; GetComputerNameA(out,&sz); }
for(char*p=out;*p;p++) if(*p==' '||*p==',') *p='_';
}

/* ------------------------------------------------------------------ */
/* Process list                                                        */
static void ProcList(){
static char names[3500];              /* keep under ~3.5KB per cmd arg */
size_t used=0; names[0]=0;
HANDLE h=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
if(h==INVALID_HANDLE_VALUE)return;
PROCESSENTRY32W pe; pe.dwSize=sizeof(pe);
if(Process32FirstW(h,&pe)){
do{
char a[64]; WideCharToMultiByte(CP_UTF8,0,pe.szExeFile,-1,a,sizeof(a),NULL,NULL);
for(char*p=a;*p;p++) if(*p==' ') *p='_';
int need=(int)strlen(a)+1;
if(used+need<sizeof(names)-1){ strcpy(names+used,a); used+=need-1; names[used++]=','; names[used]=0; }
}while(Process32NextW(h,&pe));
}
CloseHandle(h);
if(used&&names[used-1]==',')names[used-1]=0;
SendCmd("cs_proclist %s",names);
}

/* ------------------------------------------------------------------ */
/* Screenshot -> 8-bit BMP (downscaled to max 1280 wide)               */
static unsigned char *g_bmp=NULL; static size_t g_bmpLen=0;

static bool GrabScreenBMP(){
int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
if(sw<=0||sh<=0)return false;
int dw=sw,dh=sh;
if(dw>1280){ dh=(int)((double)dh*1280.0/dw+0.5); dw=1280; }

HDC sdc=GetDC(NULL), mdc=CreateCompatibleDC(sdc);
HBITMAP bmp=CreateCompatibleBitmap(sdc,sw,sh);
HGDIOBJ old=SelectObject(mdc,bmp);
BitBlt(mdc,0,0,sw,sh,sdc,0,0,SRCCOPY);

/* read 24-bit DIB */
BITMAPINFO bi; memset(&bi,0,sizeof(bi));
bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
bi.bmiHeader.biWidth=sw; bi.bmiHeader.biHeight=-sh;
bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=24; bi.bmiHeader.biCompression=BI_RGB;
unsigned char *rgb=(unsigned char*)malloc((size_t)sw*sh*3);
if(GetDIBits(sdc,bmp,0,sh,rgb,&bi,DIB_RGB_COLORS)==0){ free(rgb); rgb=NULL; }

/* simple nearest-filter downscale + grayscale-ish 8bit palette quantization */
if(rgb){
size_t rowBytes=((dw+3)/4)*4;
size_t pix=(size_t)rowBytes*dh;
size_t hdr=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)+256*sizeof(RGBQUAD);
free(g_bmp); g_bmp=(unsigned char*)malloc(hdr+pix); g_bmpLen=hdr+pix;
if(g_bmp){
BITMAPFILEHEADER fh; memset(&fh,0,sizeof(fh));
fh.bfType=0x4D42; fh.bfSize=(DWORD)(hdr+pix); fh.bfOffBits=(DWORD)hdr;
BITMAPINFOHEADER bh; memset(&bh,0,sizeof(bh));
bh.biSize=sizeof(bh); bh.biWidth=dw; bh.biHeight=dh; bh.biPlanes=1;
bh.biBitCount=8; bh.biCompression=BI_RGB; bh.biSizeImage=(DWORD)pix;
RGBQUAD pal[256];
for(int i=0;i<256;i++){ pal[i].rgbBlue=pal[i].rgbGreen=pal[i].rgbRed=(BYTE)i; pal[i].rgbReserved=0; }
memcpy(g_bmp,&fh,sizeof(fh));
memcpy(g_bmp+sizeof(fh),&bh,sizeof(bh));
memcpy(g_bmp+sizeof(fh)+sizeof(bh),pal,sizeof(pal));
unsigned char*d=g_bmp+hdr;
memset(d,0,pix);
for(int y=0;y<dh;y++){
int sy=(int)((double)y*sh/dh); if(sy>=sh)sy=sh-1;
for(int x=0;x<dw;x++){
int sx=(int)((double)x*sw/dw); if(sx>=sw)sx=sw-1;
unsigned char*p=rgb+((size_t)sy*sw+sx)*3;
d[(size_t)y*rowBytes+x]=(unsigned char)((p[2]*77+p[1]*150+p[0]*29)>>8);
}
}
}
free(rgb);
}
SelectObject(mdc,old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(NULL,sdc);
return g_bmp!=NULL;
}

static void SendScreenshot(){
if(!GrabScreenBMP())return;
int chunks=(int)((g_bmpLen+CSH_CHUNK_BYTES-1)/CSH_CHUNK_BYTES);
SendCmd("cs_shot_begin %lu %d",(unsigned long)g_bmpLen,chunks);
static const char hx[]="0123456789abcdef";
char hexline[CSH_CHUNK_BYTES*2+1];
for(int c=0;c<chunks;c++){
size_t off=(size_t)c*CSH_CHUNK_BYTES;
size_t n=g_bmpLen-off; if(n>CSH_CHUNK_BYTES)n=CSH_CHUNK_BYTES;
for(size_t i=0;i<n;i++){ hexline[i*2]=hx[g_bmp[off+i]>>4]; hexline[i*2+1]=hx[g_bmp[off+i]&15]; }
hexline[n*2]=0;
SendCmd("cs_shot_chunk %d %s",c,hexline);
Sleep(40); /* pace: ~100KB/s effective, avoid choke */
}
SendCmd("cs_shot_end");
free(g_bmp); g_bmp=NULL; g_bmpLen=0;
}

/* ------------------------------------------------------------------ */
/* Request dispatch. Called by your integration layer when usermsg     */
/* CShRq arrives (in cl_dll: HUD hook msg handler; standalone: the     */
/* launcher forwards the byte flags over its named pipe).              */
extern "C" __declspec(dllexport)
void ClientShot_HandleRequest(int flags,long nonce){
(void)nonce;
if(flags&REQ_HWID){ char id[128]; GetHWID(id,sizeof(id)); SendCmd("cs_hwid %s",id); }
if(flags&REQ_PROC)  ProcList();
if(flags&REQ_SCREENSHOT) SendScreenshot();
}

/* optional auto-start scan once connected (cl_dll build only) */
extern "C" __declspec(dllexport) int InitClientshot(){ return 1; }

BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){ (void)h;(void)r; return TRUE; }
