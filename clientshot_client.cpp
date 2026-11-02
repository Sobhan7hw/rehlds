/*
 * ClientShot 1.0 - CLIENT module (Windows, CS 1.6)
 * by FaTaL-[Hit] - https://t.me/Neo_Norm
 * Build (Linux cross-compile):
 *   sudo apt install g++-mingw-w64-i686
 *   i686-w64-mingw32-g++ -O2 -shared -static -s -o clientshot.dll \
 *       clientshot_client.cpp -ladvapi32
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
