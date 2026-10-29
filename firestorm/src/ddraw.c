/* firestorm-shim: a ddraw.dll that sits beside MW4.exe.
 *
 * Firestorm was built for a pod PC with three graphics cards: the main view on
 * one, the five MFDs on a card running 1280x480, the radar on another. Each
 * display device takes its adapter in exclusive full-screen mode with its own
 * Direct3D 7 device. This DLL lets it run on one ordinary PC:
 *
 *  - the game is offered the real primary display plus two virtual adapters,
 *    "MFD" and "radar", which it picks by the 1280x480 mode they offer;
 *  - every exclusive full-screen device is run windowed instead: the mode
 *    change is recorded, not made, and the primary surface the game asks for
 *    is an off-screen surface of the mode's size and depth;
 *  - each Flip copies the back buffer into that device's window - the MFD
 *    surface into five, one per monitor, the way the pod's splitter did.
 *
 * Everything else goes to the system ddraw.dll. The functions the shim does
 * not touch are forwarded by stubs that jump through a pointer resolved from
 * the real DLL; they all have to exist, because d3dim700.dll imports
 * AcquireDDThreadLock and friends from whatever module is loaded as ddraw.dll.
 *
 * Interfaces are hooked by patching the real objects' vtables, which the real
 * DLL shares between objects; each hook looks its object up and passes
 * anything that is not ours straight through.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include "mfdstream.h"

static HMODULE g_real;
static FILE *g_log;
static CRITICAL_SECTION g_loglock, g_lock;
static char g_dir[MAX_PATH];		/* the game's directory, with a trailing slash */
static char g_ini[MAX_PATH];
static char g_tag[32];			/* "" for the console, "ctcl2-" for a pod ... */

static void say(const char *fmt, ...)
{
	va_list ap;

	if (!g_log) return;
	EnterCriticalSection(&g_loglock);
	fprintf(g_log, "%8lu  ", GetTickCount());
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
	LeaveCriticalSection(&g_loglock);
}

static const char *guidstr(const GUID *g, char *buf)
{
	if (!g) return "NULL (primary)";
	if ((ULONG_PTR)g < 0x10000) { sprintf(buf, "special %lu", (unsigned long)(ULONG_PTR)g); return buf; }
	sprintf(buf, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
		g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
	return buf;
}

/* This copy's role - "console", or "ctcl2" for a pod and so on - so one
 * shim.ini can serve every machine in a centre: [main.ctcl2] overrides
 * [main] for pods only. */
static char g_role[16] = "console";
static int g_cockpit;			/* -tmfds given: this copy drives MFDs and a radar */
static int g_launcher;			/* loaded into VWE's Launcher.exe, not the game */

static int cfg_int(const char *sec, const char *key, int def)
{
	char rsec[64];
	snprintf(rsec, sizeof rsec, "%s.%s", sec, g_role);
	return GetPrivateProfileIntA(rsec, key, GetPrivateProfileIntA(sec, key, def, g_ini), g_ini);
}

static void cfg_str(const char *sec, const char *key, const char *def, char *out, int n)
{
	char rsec[64], base[256];
	snprintf(rsec, sizeof rsec, "%s.%s", sec, g_role);
	GetPrivateProfileStringA(sec, key, def, base, sizeof base, g_ini);
	GetPrivateProfileStringA(rsec, key, base, out, n, g_ini);
}

/* ------------------------------------------------ forwarded exports */

#define FORWARDED(X) \
	X(AcquireDDThreadLock) X(CompleteCreateSysmemSurface) X(D3DParseUnknownCommand) \
	X(DDGetAttachedSurfaceLcl) X(DDInternalLock) X(DDInternalUnlock) X(DSoundHelp) \
	X(DirectDrawCreateClipper) X(DirectDrawEnumerateExW) X(DirectDrawEnumerateW) \
	X(DllCanUnloadNow) X(DllGetClassObject) X(GetDDSurfaceLocal) X(GetOLEThunkData) \
	X(GetSurfaceFromDC) X(RegisterSpecialCase) X(ReleaseDDThreadLock) X(SetAppCompatData)

#define DECLARE_PTR(n) void *p_##n;
FORWARDED(DECLARE_PTR)

#define STUB(n) __asm__(".globl _s_" #n "\n_s_" #n ":\n\tjmp *_p_" #n "\n");
FORWARDED(STUB)

typedef HRESULT (WINAPI *DDCreate_t)(GUID *, LPDIRECTDRAW *, IUnknown *);
typedef HRESULT (WINAPI *DDCreateEx_t)(GUID *, LPVOID *, REFIID, IUnknown *);
typedef HRESULT (WINAPI *DDEnumA_t)(LPDDENUMCALLBACKA, LPVOID);
typedef HRESULT (WINAPI *DDEnumExA_t)(LPDDENUMCALLBACKEXA, LPVOID, DWORD);

static DDCreate_t   r_DirectDrawCreate;
static DDCreateEx_t r_DirectDrawCreateEx;
static DDEnumA_t    r_DirectDrawEnumerateA;
static DDEnumExA_t  r_DirectDrawEnumerateExA;

static int load_real(void)
{
	char path[MAX_PATH];

	if (g_real) return 1;
	GetSystemDirectoryA(path, sizeof path);	/* SysWOW64, for a 32-bit process */
	strcat(path, "\\ddraw.dll");
	g_real = LoadLibraryA(path);
	if (!g_real) { say("cannot load %s (%lu)", path, GetLastError()); return 0; }
#define RESOLVE(n) p_##n = (void *)GetProcAddress(g_real, #n);
	FORWARDED(RESOLVE)
	r_DirectDrawCreate = (DDCreate_t)GetProcAddress(g_real, "DirectDrawCreate");
	r_DirectDrawCreateEx = (DDCreateEx_t)GetProcAddress(g_real, "DirectDrawCreateEx");
	r_DirectDrawEnumerateA = (DDEnumA_t)GetProcAddress(g_real, "DirectDrawEnumerateA");
	r_DirectDrawEnumerateExA = (DDEnumExA_t)GetProcAddress(g_real, "DirectDrawEnumerateExA");
	say("real ddraw: %s", path);
	return 1;
}

/* ------------------------------------------------ vtable patching */

#define MAX_VTBLS 16
#define MAX_SLOTS 50
struct vtpatch { void **vt; void *orig[MAX_SLOTS]; };
static struct vtpatch g_ddvt[MAX_VTBLS], g_sfvt[MAX_VTBLS];
static int g_nddvt, g_nsfvt;

static void *orig_in(struct vtpatch *set, int n, void *obj, int slot)
{
	void **vt = *(void ***)obj;
	int i;
	for (i = 0; i < n; i++)
		if (set[i].vt == vt) return set[i].orig[slot];
	return vt[slot];		/* never patched: the method is the real one */
}

static void patch_slot(void **vt, int slot, void *fn)
{
	DWORD old;
	VirtualProtect(&vt[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
	vt[slot] = fn;
	VirtualProtect(&vt[slot], sizeof(void *), old, &old);
}

/* Record a vtable's originals and patch the given slots.
 *
 * Windows' compatibility engine (apphelp.dll) patches the same vtables for
 * old DirectDraw programs, and does it again inside every DirectDrawCreateEx -
 * which silently unhooked everything after the first object. So this runs
 * after every creation: a slot that no longer holds our function is taken
 * over again, and whatever it held becomes the original we chain to. */
static void hook_vtable(struct vtpatch *set, int *n, void *obj, int nslots,
			const int *slots, void *const *fns, const char *what)
{
	void **vt = *(void ***)obj;
	int i, k;

	for (i = 0; i < *n; i++)
		if (set[i].vt == vt) {
			for (k = 0; k < nslots; k++)
				if (vt[slots[k]] != fns[k]) {
					set[i].orig[slots[k]] = vt[slots[k]];
					patch_slot(vt, slots[k], fns[k]);
					say("%s vtable %p slot %d was re-patched under us; taken back",
					    what, (void *)vt, slots[k]);
				}
			return;
		}
	if (*n == MAX_VTBLS) { say("too many %s vtables", what); return; }
	set[*n].vt = vt;
	for (i = 0; i < MAX_SLOTS; i++) set[*n].orig[i] = vt[i];
	(*n)++;
	for (i = 0; i < nslots; i++) patch_slot(vt, slots[i], fns[i]);
	say("hooked %s vtable %p", what, (void *)vt);
}

/* ------------------------------------------------ the devices */

enum kind { K_NONE, K_MAIN, K_MFD, K_RADAR };
static const char *KIND[] = { "-", "main", "mfd", "radar" };

/* The virtual adapters. Firestorm picks the MFD adapter as the first one
 * offering 1280x480 and the radar as the second, so both offer it. */
static const GUID FS_GUID_MFD   = { 0x46534D46, 0x0001, 0x4654, { 'F','I','R','E','S','T','R','M' } };
static const GUID FS_GUID_RADAR = { 0x46534D46, 0x0002, 0x4654, { 'F','I','R','E','S','T','R','M' } };

struct dev {
	void *dd;
	int kind;
	int emulated;			/* asked for exclusive full screen; run windowed */
	int serial;			/* 1, 2 ... per kind, for window names */
	HWND hwnd;			/* where frames go: the game's window, or ours */
	DWORD w, h, bpp;
	LPDIRECTDRAWSURFACE7 prim, back;
	uint32_t *rgb;			/* the last frame, 0x00RRGGBB, w*h */
	unsigned frames;
	DWORD snapped;			/* the snapshot request it last answered */
	int lockfail;
	LPDIRECTDRAWSURFACE7 screen;	/* the real primary, for [main] gpu_present */
	LPDIRECTDRAWCLIPPER clip;
};
#define MAX_DEVS 16
static struct dev g_dev[MAX_DEVS];
static int g_serial[4];

static struct dev *dev_of(void *dd)
{
	int i;
	for (i = 0; i < MAX_DEVS; i++)
		if (g_dev[i].dd == dd) return &g_dev[i];
	return NULL;
}

static struct dev *dev_of_prim(void *s)
{
	int i;
	for (i = 0; i < MAX_DEVS; i++)
		if (g_dev[i].dd && g_dev[i].prim == s) return &g_dev[i];
	return NULL;
}

/* A new object can reuse a freed one's address, so whatever the address
 * meant before is forgotten. */
static struct dev *dev_new(void *dd, int kind)
{
	struct dev *d = dev_of(dd);
	int i;

	if (!d)
		for (i = 0; i < MAX_DEVS && !d; i++)
			if (!g_dev[i].dd) d = &g_dev[i];
	if (!d) { say("too many DirectDraw objects"); return NULL; }
	free(d->rgb);
	memset(d, 0, sizeof *d);
	d->dd = dd;
	d->kind = kind;
	return d;
}

static void forget_surface(void *s)
{
	int i;
	for (i = 0; i < MAX_DEVS; i++) {
		if (g_dev[i].prim == s) g_dev[i].prim = NULL;
		if (g_dev[i].back == s) g_dev[i].back = NULL;
	}
}

static void pixfmt(DDPIXELFORMAT *pf, DWORD bpp)
{
	memset(pf, 0, sizeof *pf);
	pf->dwSize = sizeof *pf;
	pf->dwFlags = DDPF_RGB;
	pf->dwRGBBitCount = bpp;
	if (bpp == 16) {
		pf->dwRBitMask = 0xF800; pf->dwGBitMask = 0x07E0; pf->dwBBitMask = 0x001F;
	} else {
		pf->dwRBitMask = 0xFF0000; pf->dwGBitMask = 0xFF00; pf->dwBBitMask = 0xFF;
	}
}

/* ------------------------------------------------ windows */

/* The shim's windows live on a thread of their own with its own message
 * loop, so they stay responsive whatever the game's loop does, and they never
 * take focus: the game's keyboard and joystick stay with its window. Frames
 * are drawn into them from the game's thread through GetDC. */

#define WM_FS_CREATE (WM_APP + 1)
static void rio_button(int id, int down);
static int mfd_click_at(HWND w, int x, int y);
static int radar_button_at(HWND w, int x, int y);
static HWND g_ctl;
static HWND g_radar_win;			/* one for every radar device */
static HANDLE g_uiready;

struct winreq { const char *title, *section; int x, y, w, h; HWND out; };

static LRESULT CALLBACK win_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m) {
	case WM_CLOSE: return 0;			/* the game owns their lifetime */
	case WM_ERASEBKGND: return 1;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: {
		static int held = -1;
		int id = m != WM_LBUTTONDOWN ? held
			 : w == g_radar_win ? radar_button_at(w, (short)LOWORD(lp), (short)HIWORD(lp))
			 : mfd_click_at(w, (short)LOWORD(lp), (short)HIWORD(lp));
		if (id >= 0) rio_button(id, m == WM_LBUTTONDOWN);
		held = m == WM_LBUTTONDOWN ? id : -1;
		return 0;
	}
	case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(w, &ps); EndPaint(w, &ps); return 0; }
	}
	return DefWindowProcA(w, m, wp, lp);
}

static LRESULT CALLBACK ctl_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	if (m == WM_FS_CREATE) {
		struct winreq *r = (struct winreq *)lp;
		int borderless = cfg_int(r->section, "borderless", 0);
		DWORD style = borderless ? WS_POPUP : (WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME);
		DWORD ex = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
		RECT rc;
		int x = cfg_int(r->section, "x", r->x), y = cfg_int(r->section, "y", r->y);
		int cw = cfg_int(r->section, "w", r->w), ch = cfg_int(r->section, "h", r->h);

		SetRect(&rc, 0, 0, cw, ch);
		AdjustWindowRectEx(&rc, style, FALSE, ex);
		r->out = CreateWindowExA(ex, "FirestormShimWindow", r->title, style,
					 x, y, rc.right - rc.left,
					 rc.bottom - rc.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
		if (r->out) ShowWindow(r->out, SW_SHOWNOACTIVATE);
		return 0;
	}
	return DefWindowProcA(w, m, wp, lp);
}

static DWORD WINAPI ui_main(LPVOID p)
{
	WNDCLASSA wc = { 0 };
	MSG msg;

	wc.lpfnWndProc = win_proc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = "FirestormShimWindow";
	RegisterClassA(&wc);
	wc.lpfnWndProc = ctl_proc;
	wc.lpszClassName = "FirestormShimControl";
	RegisterClassA(&wc);
	g_ctl = CreateWindowExA(0, "FirestormShimControl", "", 0, 0, 0, 0, 0, HWND_MESSAGE,
				NULL, wc.hInstance, NULL);
	SetEvent(g_uiready);
	while (GetMessageA(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}
	return 0;
}

/* Where the shim's windows go when shim.ini does not say: the second monitor
 * if there is one, else the primary's right-hand side. */
static RECT g_area;
static int g_have_area;

static BOOL CALLBACK mon_cb(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
	MONITORINFO mi = { sizeof mi };
	GetMonitorInfoA(m, &mi);
	if (!(mi.dwFlags & MONITORINFOF_PRIMARY) && !g_have_area) {
		g_area = mi.rcWork;
		g_have_area = 1;
	}
	return TRUE;
}

static void layout_area(void)
{
	if (g_have_area) return;
	EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
	if (!g_have_area) {
		SystemParametersInfoA(SPI_GETWORKAREA, 0, &g_area, 0);
		g_area.left = g_area.right - 1000 > g_area.left ? g_area.right - 1000 : g_area.left;
		g_have_area = 1;
	}
}

static HWND make_window(const char *title, const char *section, int dx, int dy, int w, int h)
{
	struct winreq r;

	if (!g_ctl) {
		g_uiready = CreateEventA(NULL, TRUE, FALSE, NULL);
		CreateThread(NULL, 0, ui_main, NULL, 0, NULL);
		WaitForSingleObject(g_uiready, 5000);
	}
	layout_area();
	r.title = title; r.section = section;
	r.x = g_area.left + dx; r.y = g_area.top + dy; r.w = w; r.h = h; r.out = NULL;
	SendMessageA(g_ctl, WM_FS_CREATE, 0, (LPARAM)&r);
	say("window \"%s\" [%s] -> %p", title, section, (void *)r.out);
	return r.out;
}

/* ------------------------------------------------ the MFDs */

/* The pod's MFD card ran 1280x480: two 640x480 VGA heads, split by a cable
 * into five monochrome monitors. Which part of the surface is which monitor
 * is configuration - [mfd] tiles - until a capture settles it. Each tile is
 * "L" or "R" for the head, then "R", "G", "B" for a colour channel or "C"
 * for the head in colour. */
#define MFD_TILES 6
static struct { int head, chan; HWND hwnd; } g_tile[MFD_TILES];
static int g_ntiles = -1;
static HWND g_mfd_whole;
static int g_wall_cols, g_wall_rows;	/* [mfd] layout = wall: one window */

/* Where in its window a tile is drawn: all of it, or its cell of the wall. */
static void tile_rect(int i, RECT *r)
{
	GetClientRect(g_tile[i].hwnd, r);
	if (g_wall_cols) {
		int cw = r->right / g_wall_cols, ch = r->bottom / g_wall_rows;
		SetRect(r, (i % g_wall_cols) * cw, (i / g_wall_cols) * ch,
			(i % g_wall_cols + 1) * cw, (i / g_wall_cols + 1) * ch);
	}
}
static RGBQUAD g_tint[256];

static void mfd_setup(void)
{
	char buf[256], *t;
	int i, tr, tg, tb;

	if (g_ntiles >= 0) return;
	g_ntiles = 0;
	if (!cfg_int("shim", "cockpit_windows", g_cockpit)) return;
	cfg_str("mfd", "tiles", "L:R,L:G,L:B,R:R,R:G", buf, sizeof buf);
	for (t = strtok(buf, ", "); t && g_ntiles < MFD_TILES; t = strtok(NULL, ", ")) {
		if (strlen(t) < 3) continue;
		g_tile[g_ntiles].head = (t[0] == 'R' || t[0] == 'r');
		g_tile[g_ntiles].chan = strchr("RrGgBb", t[2]) ? (int)(strchr("RGB", toupper(t[2])) - "RGB") : 3;
		g_ntiles++;
	}
	/* Grey by default; [mfd] tint = RRGGBB colours the monitors, the way a
	 * green phosphor would. */
	cfg_str("mfd", "tint", "FFFFFF", buf, sizeof buf);
	sscanf(buf, "%2x%2x%2x", &tr, &tg, &tb);
	for (i = 0; i < 256; i++) {
		g_tint[i].rgbRed = (BYTE)(i * tr / 255);
		g_tint[i].rgbGreen = (BYTE)(i * tg / 255);
		g_tint[i].rgbBlue = (BYTE)(i * tb / 255);
	}
	/* [mfd] layout = wall puts the five in one window, [mfd_wall] cols
	 * across (default 5) - for a video-wall controller behind one output,
	 * or one wide screen under the main view. */
	cfg_str("mfd", "layout", "windows", buf, sizeof buf);
	if (!_stricmp(buf, "wall")) {
		HWND wall;
		g_wall_cols = cfg_int("mfd_wall", "cols", g_ntiles);
		if (g_wall_cols < 1) g_wall_cols = 1;
		g_wall_rows = (g_ntiles + g_wall_cols - 1) / g_wall_cols;
		wall = make_window("MFDs", "mfd_wall", 0, 0, g_wall_cols * 320, g_wall_rows * 240);
		for (i = 0; i < g_ntiles; i++) g_tile[i].hwnd = wall;
	} else
		for (i = 0; i < g_ntiles; i++) {
			char title[32], sec[16];
			sprintf(title, "MFD %d", i + 1);
			sprintf(sec, "mfd%d", i + 1);
			g_tile[i].hwnd = make_window(title, sec, (i % 3) * 330, (i / 3) * 280, 320, 240);
		}
	if (cfg_int("mfd", "show_surface", 1))
		g_mfd_whole = make_window("MFD surface (1280x480, as the card saw it)", "mfd_surface",
					  0, 560, 640, 240);
}

static void draw_rgb_in(HWND w, const RECT *to, const uint32_t *px, int sw, int sh,
			int x0, int y0, int cw, int ch)
{
	BITMAPINFO bi;
	RECT rc;
	HDC dc;

	if (!w || !px) return;
	memset(&bi, 0, sizeof bi);
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = sw;
	bi.bmiHeader.biHeight = -sh;			/* top-down */
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	if (to) rc = *to;
	else GetClientRect(w, &rc);
	dc = GetDC(w);
	SetStretchBltMode(dc, COLORONCOLOR);
	StretchDIBits(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
		      x0, sh - y0 - ch, cw, ch, px, &bi, DIB_RGB_COLORS, SRCCOPY);
	ReleaseDC(w, dc);
}

static void draw_rgb(HWND w, const uint32_t *px, int sw, int sh, int x0, int y0, int cw, int ch)
{
	draw_rgb_in(w, NULL, px, sw, sh, x0, y0, cw, ch);
}

/* Where in its window a w x h view is drawn: all of the client area, or with
 * [main] keep_aspect (the default) the largest part of it of the view's
 * shape, centred - the game's modes are all 4:3, most screens are not. */
static void view_box(HWND hw, DWORD w, DWORD h, RECT *r)
{
	GetClientRect(hw, r);
	static int keep = -1;

	if (keep < 0) keep = cfg_int("main", "keep_aspect", 1);
	if (keep && w && h && r->right > 0 && r->bottom > 0) {
		int bw = r->right, bh = r->bottom;
		if (bw * (int)h > bh * (int)w) bw = bh * (int)w / (int)h;
		else bh = bw * (int)h / (int)w;
		SetRect(r, (r->right - bw) / 2, (r->bottom - bh) / 2, (r->right - bw) / 2 + bw, (r->bottom - bh) / 2 + bh);
	}
}

static void draw_view(HWND hw, const uint32_t *px, DWORD w, DWORD h)
{
	RECT box, rc;

	if (!hw || !px) return;
	view_box(hw, w, h, &box);
	GetClientRect(hw, &rc);
	if (box.left > 0 || box.top > 0) {		/* the bars */
		HDC dc = GetDC(hw);
		HBRUSH k = (HBRUSH)GetStockObject(BLACK_BRUSH);
		RECT b;
		SetRect(&b, 0, 0, rc.right, box.top);			FillRect(dc, &b, k);
		SetRect(&b, 0, box.bottom, rc.right, rc.bottom);	FillRect(dc, &b, k);
		SetRect(&b, 0, box.top, box.left, box.bottom);		FillRect(dc, &b, k);
		SetRect(&b, box.right, box.top, rc.right, box.bottom);	FillRect(dc, &b, k);
		ReleaseDC(hw, dc);
	}
	draw_rgb_in(hw, &box, px, w, h, 0, 0, w, h);
}

static void mfd_present(struct dev *d)
{
	static uint8_t *grey;
	static struct { BITMAPINFOHEADER h; RGBQUAD pal[256]; } bi;
	int i, x, y, half = (int)d->w / 2;

	mfd_setup();
	if (g_mfd_whole) draw_rgb(g_mfd_whole, d->rgb, d->w, d->h, 0, 0, d->w, d->h);
	if (!grey) grey = malloc(half * d->h);
	bi.h.biSize = sizeof bi.h;
	bi.h.biWidth = half;
	bi.h.biHeight = -(LONG)d->h;
	bi.h.biPlanes = 1;
	bi.h.biBitCount = 8;
	bi.h.biClrUsed = 256;
	memcpy(bi.pal, g_tint, sizeof g_tint);
	for (i = 0; i < g_ntiles; i++) {
		RECT rc;
		HDC dc;
		int shift = 16 - 8 * g_tile[i].chan;

		if (!g_tile[i].hwnd) continue;
		tile_rect(i, &rc);
		if (g_tile[i].chan == 3) {		/* the head, in colour */
			draw_rgb_in(g_tile[i].hwnd, &rc, d->rgb, d->w, d->h, g_tile[i].head * half, 0, half, d->h);
			continue;
		}
		for (y = 0; y < (int)d->h; y++) {
			const uint32_t *s = d->rgb + y * d->w + g_tile[i].head * half;
			uint8_t *o = grey + y * half;
			for (x = 0; x < half; x++) o[x] = (uint8_t)(s[x] >> shift);
		}
		dc = GetDC(g_tile[i].hwnd);
		SetStretchBltMode(dc, COLORONCOLOR);
		StretchDIBits(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0, 0, half, d->h, grey,
			      (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
		ReleaseDC(g_tile[i].hwnd, dc);
	}
}

/* ------------------------------------------------ streaming the MFDs
 *
 * [stream] enable = 1 serves the MFD pictures on a local TCP port ([stream]
 * port, default 9110) for tools/bridge.py to take to hardware modules -
 * docs/MFD_STREAMING_PROTOCOL.md. Off by default, when all it costs is one
 * test a frame; the windows are drawn as before either way. The game's
 * thread only copies the finished MFD surface (a fraction of a millisecond,
 * and only while someone is taking the stream) and wakes the streaming
 * thread, which takes each display's channel, compares it with what it last
 * sent, and codes and sends the changed 16x16 tiles (mfdstream.c). If it
 * falls behind it skips to the newest frame: the game never waits for it.
 * Display n is the nth of [mfd] tiles - the window "MFD n". [stream]
 * displays = 1,2,3,4,5 picks which; tile = 16; stats = 1 logs the rates
 * every 10 s. */
#define ST_MAX MFD_TILES
static double ms_since(LARGE_INTEGER *t);
static struct {
	int on;				/* -1: not read yet */
	CRITICAL_SECTION cs;
	HANDLE wake;
	uint32_t *stage, *spare;	/* the newest MFD surface, and the one the thread works from */
	DWORD w, h;
	int fresh;
	uint32_t t_us;
	volatile LONG client;		/* someone is taking the stream */
	int n;
	struct { int head, chan, on, want_key; } d[ST_MAX];
	double copy_ms, copy_max;	/* the game thread's part, for the statistics */
	int copies;
} g_st = { -1 };
static SOCKET g_st_cli = INVALID_SOCKET;
static uint8_t g_st_seq;
static uint8_t g_st_wire[2 * (MS_HEADER + MS_MAX_BODY) + 16];

static uint32_t qpc_us(void)
{
	static LARGE_INTEGER f;
	LARGE_INTEGER c;
	if (!f.QuadPart) QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (uint32_t)((c.QuadPart / f.QuadPart) * 1000000 + (c.QuadPart % f.QuadPart) * 1000000 / f.QuadPart);
}

static void st_drop(const char *why)
{
	if (g_st_cli == INVALID_SOCKET) return;
	InterlockedExchange(&g_st.client, 0);
	closesocket(g_st_cli);
	g_st_cli = INVALID_SOCKET;
	say("MFD stream: the client went (%s)", why);
}

static void st_send(uint8_t type, const uint8_t *payload, int len)
{
	int n = mp_pack(type, ++g_st_seq, payload, len, g_st_wire), at = 0;

	while (g_st_cli != INVALID_SOCKET && at < n) {
		int k = send(g_st_cli, (const char *)g_st_wire + at, n - at, 0);
		if (k <= 0) { st_drop("send failed"); return; }
		at += k;
	}
}

static void st_emit(void *ctx, const uint8_t *payload, int len)
{
	st_send(MP_FRAME, payload, len);
}

static void st_hello(int w, int h)
{
	uint8_t p[96];
	const char *cls = "mfd-source", *fw = "firestorm-shim";
	char uid[48];
	int n = 0, k;

	snprintf(uid, sizeof uid, "firestorm-%s%s", g_tag[0] ? g_tag : "", g_cockpit ? "pod" : "console");
	p[n++] = 0;					/* module protocol version */
	k = (int)strlen(cls); p[n++] = (uint8_t)k; memcpy(p + n, cls, k); n += k;
	k = (int)strlen(uid); p[n++] = (uint8_t)k; memcpy(p + n, uid, k); n += k;
	k = (int)strlen(fw); p[n++] = (uint8_t)k; memcpy(p + n, fw, k); n += k;
	p[n++] = 0; p[n++] = 0; p[n++] = 0;		/* no keys, no lamps */
	p[n++] = (uint8_t)(w >> 8); p[n++] = (uint8_t)w;
	p[n++] = (uint8_t)(h >> 8); p[n++] = (uint8_t)h;
	p[n++] = 3;					/* 8-bit grey */
	st_send(MP_HELLO, p, n);
}

/* What the bridge says: HELLO? and SYNC (a whole frame for a display). */
static int st_read(uint8_t *buf, int *have, int cap)
{
	u_long avail = 0;
	uint8_t msg[64];
	int k, z;

	if (ioctlsocket(g_st_cli, FIONREAD, &avail) || !avail) return 0;
	k = recv(g_st_cli, (char *)buf + *have, cap - *have, 0);
	if (k <= 0) { st_drop("closed"); return -1; }
	*have += k;
	for (;;) {
		for (z = 0; z < *have && buf[z]; z++)
			;
		if (z == *have) {
			if (*have == cap) *have = 0;	/* nothing that long is ours */
			return 0;
		}
		k = z ? mp_unpack(buf, z, msg, sizeof msg) : -1;
		if (k >= 2 && msg[0] == MP_HELLO_Q)
			st_hello(g_st.w / 2, g_st.h);
		else if (k >= 3 && msg[0] == MP_SYNC && msg[2] >= 1 && msg[2] <= g_st.n)
			g_st.d[msg[2] - 1].want_key = 1;
		memmove(buf, buf + z + 1, *have - z - 1);
		*have -= z + 1;
	}
}

static DWORD WINAPI stream_main(LPVOID p)
{
	static struct ms_enc enc[ST_MAX];
	static uint8_t rx[512];
	int port = cfg_int("stream", "port", 9110), tile = cfg_int("stream", "tile", 16);
	int stats = cfg_int("stream", "stats", 0), max_fps = cfg_int("stream", "max_fps", 0);
	int i, rxn = 0;
	uint8_t *grey = NULL;
	uint32_t stream_id = 0, last_us = 0;
	SOCKET lis;
	struct sockaddr_in a;
	WSADATA wsa;
	LARGE_INTEGER t_stat;
	double enc_ms[ST_MAX] = { 0 }, enc_max[ST_MAX] = { 0 };
	int enc_n[ST_MAX] = { 0 };

	WSAStartup(MAKEWORD(2, 2), &wsa);
	lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((u_short)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (lis == INVALID_SOCKET || bind(lis, (struct sockaddr *)&a, sizeof a) || listen(lis, 1)) {
		say("MFD stream: port %d is taken; not streaming", port);
		return 0;
	}
	say("MFD stream: tcp://127.0.0.1:%d, %d display(s), %dx%d tiles", port, g_st.n, tile, tile);
	QueryPerformanceCounter(&t_stat);
	for (;;) {
		int have_frame = 0;
		uint32_t t_us = 0;

		if (g_st_cli == INVALID_SOCKET) {
			/* Small: when the bridge cannot keep up, frames are skipped here
			 * (the newest wins) rather than queued, so the delay stays short. */
			int one = 1, buf = 256 * 1024;
			SOCKET c = accept(lis, NULL, NULL);
			if (c == INVALID_SOCKET) { Sleep(100); continue; }
			setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
			setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char *)&buf, sizeof buf);
			/* A new stream for each client: its first update is a whole frame. */
			stream_id = stream_id * 2654435761u + GetTickCount() + (GetCurrentProcessId() << 16) + 1;
			for (i = 0; i < g_st.n; i++) {
				ms_free(&enc[i]);
				ms_init(&enc[i], i + 1, 0, 0, tile, stream_id);
				g_st.d[i].want_key = 0;
			}
			g_st_cli = c;
			rxn = 0;
			InterlockedExchange(&g_st.client, 1);
			say("MFD stream: a client; stream %08lX", (unsigned long)stream_id);
			continue;
		}
		WaitForSingleObject(g_st.wake, 50);
		if (st_read(rx, &rxn, sizeof rx) < 0) continue;

		EnterCriticalSection(&g_st.cs);
		if (g_st.fresh && (!max_fps || (uint32_t)(g_st.t_us - last_us) >= 1000000u / max_fps)) {
			uint32_t *s = g_st.stage;
			g_st.stage = g_st.spare;		/* the game copies into the other one next */
			g_st.spare = s;
			g_st.fresh = 0;
			t_us = last_us = g_st.t_us;
			have_frame = 1;
		}
		LeaveCriticalSection(&g_st.cs);

		if (have_frame) {
			int half = (int)g_st.w / 2, hh = (int)g_st.h, x, y;
			if (!grey) grey = malloc((size_t)half * hh);
			for (i = 0; i < g_st.n && grey && g_st_cli != INVALID_SOCKET; i++) {
				int shift = 16 - 8 * g_st.d[i].chan;
				LARGE_INTEGER t0;
				double ms;
				if (!g_st.d[i].on) continue;
				if (enc[i].w != half || enc[i].h != hh) {
					ms_free(&enc[i]);
					ms_init(&enc[i], i + 1, half, hh, tile, stream_id);
				}
				QueryPerformanceCounter(&t0);
				for (y = 0; y < hh; y++) {
					const uint32_t *src = g_st.spare + y * g_st.w + g_st.d[i].head * half;
					uint8_t *o = grey + y * half;
					for (x = 0; x < half; x++) o[x] = (uint8_t)(src[x] >> shift);
				}
				ms_update(&enc[i], grey, t_us, st_emit, NULL);
				ms = ms_since(&t0);
				enc_n[i]++;
				enc_ms[i] += ms;
				if (ms > enc_max[i]) enc_max[i] = ms;
			}
		}
		for (i = 0; i < g_st.n && g_st_cli != INVALID_SOCKET; i++)
			if (g_st.d[i].want_key) {
				g_st.d[i].want_key = 0;
				ms_key(&enc[i], qpc_us(), st_emit, NULL);
			}
		if (stats) {
			LARGE_INTEGER now, f;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&f);
			if (now.QuadPart - t_stat.QuadPart >= f.QuadPart * 10) {
				double el = (double)(now.QuadPart - t_stat.QuadPart) / f.QuadPart;
				for (i = 0; i < g_st.n; i++) {
					struct ms_enc *e = &enc[i];
					if (!g_st.d[i].on) continue;
					say("MFD stream: display %d: %.1f updates/s, %u whole, %.1f KB/s, %.0f B/update;"
					    " coding %.2f ms a frame (worst %.2f)", i + 1, e->updates / el, e->keys,
					    e->bytes / el / 1024, e->updates ? (double)e->bytes / (e->updates + e->keys) : 0.0,
					    enc_n[i] ? enc_ms[i] / enc_n[i] : 0.0, enc_max[i]);
					e->updates = e->keys = e->parts = e->bytes = e->rects = 0;
					enc_ms[i] = enc_max[i] = 0;
					enc_n[i] = 0;
				}
				EnterCriticalSection(&g_st.cs);
				say("MFD stream: the game's copy %.3f ms a frame (worst %.3f)",
				    g_st.copies ? g_st.copy_ms / g_st.copies : 0.0, g_st.copy_max);
				g_st.copy_ms = g_st.copy_max = 0;
				g_st.copies = 0;
				LeaveCriticalSection(&g_st.cs);
				t_stat = now;
			}
		}
	}
	return 0;
}

static void stream_start(void)
{
	char buf[256], *t;
	int n = 0;

	g_st.on = cfg_int("stream", "enable", 0);
	if (!g_st.on) return;
	/* The displays: [mfd] tiles, as the windows read it. */
	cfg_str("mfd", "tiles", "L:R,L:G,L:B,R:R,R:G", buf, sizeof buf);
	for (t = strtok(buf, ", "); t && n < ST_MAX; t = strtok(NULL, ", ")) {
		if (strlen(t) < 3) continue;
		g_st.d[n].head = (t[0] == 'R' || t[0] == 'r');
		g_st.d[n].chan = strchr("RrGgBb", t[2]) ? (int)(strchr("RGB", toupper(t[2])) - "RGB") : 3;
		n++;
	}
	g_st.n = n;
	cfg_str("stream", "displays", "1,2,3,4,5,6", buf, sizeof buf);
	for (t = strtok(buf, ", "); t; t = strtok(NULL, ", ")) {
		int k = atoi(t);
		if (k >= 1 && k <= n) g_st.d[k - 1].on = g_st.d[k - 1].chan != 3;	/* not a head in colour */
	}
	InitializeCriticalSection(&g_st.cs);
	g_st.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	CreateThread(NULL, 0, stream_main, NULL, 0, NULL);
}

/* The game's thread: the finished MFD surface, for the streaming thread. */
static void stream_offer(struct dev *d)
{
	LARGE_INTEGER t;

	if (g_st.on < 0) stream_start();
	if (!g_st.on || !g_st.client || !d->rgb) return;
	ms_since(&t);
	EnterCriticalSection(&g_st.cs);
	if (!g_st.stage) {
		g_st.stage = malloc((size_t)d->w * d->h * 4);
		g_st.spare = malloc((size_t)d->w * d->h * 4);
		g_st.w = d->w;
		g_st.h = d->h;
	} else if (g_st.w != d->w || g_st.h != d->h) {
		/* The thread may be reading the other buffer: never freed under it.
		 * The MFD surface is 1280x480 for good, so this does not happen. */
		say("MFD stream: the MFD surface changed size; not streaming");
		g_st.on = 0;
	}
	if (g_st.on && g_st.stage && g_st.spare) {
		memcpy(g_st.stage, d->rgb, (size_t)d->w * d->h * 4);
		g_st.fresh = 1;
		g_st.t_us = qpc_us();
	}
	{
		double ms = ms_since(&t);
		g_st.copy_ms += ms;
		if (ms > g_st.copy_max) g_st.copy_max = ms;
		g_st.copies++;
	}
	LeaveCriticalSection(&g_st.cs);
	SetEvent(g_st.wake);
}

/* ------------------------------------------------ presenting a frame */

static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
	BITMAPFILEHEADER fh = { 0 };
	BITMAPINFOHEADER ih = { 0 };
	FILE *f = fopen(path, "wb");

	if (!f) return;
	ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32;
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof fh + sizeof ih;
	fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
	fwrite(&fh, sizeof fh, 1, f);
	fwrite(&ih, sizeof ih, 1, f);
	fwrite(px, 4, (size_t)w * h, f);
	fclose(f);
}

/* Snapshots, for looking at what each device drew: automatic at a few frame
 * counts, and whenever a file called "snap" appears in the game's directory. */
static DWORD g_snapreq;

/* Whether frame n of d is to be snapshotted. */
static int snap_due(struct dev *d, unsigned n)
{
	char snapfile[MAX_PATH];

	if ((n & 31) == 0) {
		snprintf(snapfile, sizeof snapfile, "%ssnap", g_dir);
		if (GetFileAttributesA(snapfile) != INVALID_FILE_ATTRIBUTES) {
			DeleteFileA(snapfile);
			g_snapreq = GetTickCount();
		}
	}
	return n == 120 || n == 1200 || (g_snapreq && d->snapped != g_snapreq);
}

static void snapshot(struct dev *d)
{
	char path[MAX_PATH];

	if (!snap_due(d, d->frames)) return;
	d->snapped = g_snapreq;
	if (!cfg_int("shim", "snapshots", 1)) return;
	snprintf(path, sizeof path, "%ssnap-%s%s%d-%05u.bmp", g_dir, g_tag, KIND[d->kind], d->serial, d->frames);
	write_bmp(path, d->rgb, d->w, d->h);
	say("snapshot %s", path);
}

static int mask_shift(DWORD m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mask_bits(DWORD m) { int b = 0; m >>= mask_shift(m); while (m & 1) { m >>= 1; b++; } return b; }

/* Where the copying goes, for [shim] frame_stats: ms spent reading frames
 * back and converting them, and drawing them, the main view and the rest. */
static double g_t_copy[2], g_t_draw[2];
static LARGE_INTEGER g_t_freq;

/* Uncapped, the main view runs at hundreds of frames a second on a modern
 * card, for nothing but heat; capped by the clock at a rate the monitor does
 * not run at, frames are shown for one refresh and then two, and anything
 * moving judders - on a 144 Hz monitor at 120 fps the view seemed to shake
 * while the Mech walked. So by default each frame waits for the desktop's
 * next composition (DwmFlush): one frame per refresh, evenly spaced, as a
 * vsynced game would be; vsync = 2 waits two, for a steady half rate when
 * a frame cannot always be made in one refresh. [shim] vsync = 0 paces by
 * the clock instead, at max_fps (default 120; 0: no cap); with vsync max_fps
 * is only a ceiling, for when the desktop composes nothing (minimised).
 * [shim] frame_stats = 1 logs the frame rate and frame times every 10 s. */
static void frame_cap(void)
{
	static LARGE_INTEGER freq, next, prev, t_stat;
	static int fps = -1, vsync, stats;
	static HRESULT (WINAPI *flush)(void);
	static double worst, sum, busy, busy_max;
	static int n;
	LARGE_INTEGER now, start;
	int n_flush;

	if (fps < 0) {
		HMODULE wm = LoadLibraryA("winmm.dll"), dwm = LoadLibraryA("dwmapi.dll");
		UINT (WINAPI *tbp)(UINT) = wm ? (UINT (WINAPI *)(UINT))GetProcAddress(wm, "timeBeginPeriod") : NULL;
		vsync = cfg_int("shim", "vsync", 1);
		fps = cfg_int("shim", "max_fps", vsync ? 300 : 120);
		stats = cfg_int("shim", "frame_stats", 0);
		if (dwm) flush = (HRESULT (WINAPI *)(void))GetProcAddress(dwm, "DwmFlush");
		if (!flush) vsync = 0;
		if (tbp) tbp(1);			/* Sleep(1) sleeps about 1 ms */
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&next);
		prev = t_stat = next;
		if (vsync < 0 || vsync > 4) vsync = 1;
		say("frame pacing: %s, at most %d fps", vsync ? "the desktop's refresh (DwmFlush)" : "the clock", fps);
	}
	QueryPerformanceCounter(&start);
	for (n_flush = 0; n_flush < vsync; n_flush++) flush();
	if (fps > 0) {
		next.QuadPart += freq.QuadPart / fps;
		QueryPerformanceCounter(&now);
		if (now.QuadPart > next.QuadPart + (vsync ? 0 : freq.QuadPart / 10))
			next = now;			/* behind: start again */
		while (now.QuadPart < next.QuadPart) {
			LONGLONG left = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
			Sleep(left > 1 ? (DWORD)(left - 1) : 0);
			QueryPerformanceCounter(&now);
		}
	}
	if (!stats) return;
	QueryPerformanceCounter(&now);
	{
		double dt = (double)(now.QuadPart - prev.QuadPart) * 1000 / freq.QuadPart;
		double b = (double)(start.QuadPart - prev.QuadPart) * 1000 / freq.QuadPart;
		prev = now;
		sum += dt; if (dt > worst) worst = dt;
		busy += b; if (b > busy_max) busy_max = b;
		n++;
	}
	if (now.QuadPart - t_stat.QuadPart >= freq.QuadPart * 10) {
		say("frames: %.1f fps, frame %.2f ms (worst %.2f), game and copy %.2f ms (worst %.2f);"
		    " per frame: main view read %.2f draw %.2f, other displays read %.2f draw %.2f ms",
		    n * 1000 / sum, sum / n, worst, busy / n, busy_max,
		    g_t_copy[0] / n, g_t_draw[0] / n, g_t_copy[1] / n, g_t_draw[1] / n);
		t_stat = now; n = 0; sum = worst = busy = busy_max = 0;
		memset(g_t_copy, 0, sizeof g_t_copy); memset(g_t_draw, 0, sizeof g_t_draw);
	}
}

/* The radar monitor stood on its side in the pod, and the game draws the
 * radar landscape with its labels in rotated text. [radar] rotate = 90 (or
 * 270) turns it upright, for a monitor mounted portrait as the pod's was. */
static int radar_rotation(void)
{
	static int r = -1;
	if (r < 0) {
		r = cfg_int("radar", "rotate", 0);
		if (r != 90 && r != 270) r = 0;
	}
	return r;
}

static void radar_present(struct dev *d)
{
	static uint32_t *rot;
	static size_t have;
	DWORD x, y, w = d->w, h = d->h;

	if (have < (size_t)w * h) {
		free(rot);
		rot = malloc((size_t)w * h * 4);
		have = rot ? (size_t)w * h : 0;
	}
	if (!rot) return;
	/* The turned picture is h wide and w high. */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint32_t px = d->rgb[y * w + x];
			if (radar_rotation() == 90) rot[x * h + (h - 1 - y)] = px;
			else rot[(w - 1 - x) * h + y] = px;
		}
	draw_rgb(d->hwnd, rot, h, w, 0, 0, h, w);
}

static double ms_since(LARGE_INTEGER *t)
{
	LARGE_INTEGER now;
	double ms;
	if (!g_t_freq.QuadPart) QueryPerformanceFrequency(&g_t_freq);
	QueryPerformanceCounter(&now);
	ms = (double)(now.QuadPart - t->QuadPart) * 1000 / g_t_freq.QuadPart;
	*t = now;
	return ms;
}

/* 16-bit pixels to 32, by table: a shift and a lookup per pixel cost the
 * game's thread milliseconds a frame at 1600x1200. */
static void conv16(uint32_t *o, const uint16_t *in, DWORD n, const DDPIXELFORMAT *pf)
{
	static uint32_t lut[65536];
	static DWORD rm, gm, bm;
	DWORD i;

	if (pf->dwRBitMask != rm || pf->dwGBitMask != gm || pf->dwBBitMask != bm) {
		int rs = mask_shift(pf->dwRBitMask), gs = mask_shift(pf->dwGBitMask), bs = mask_shift(pf->dwBBitMask);
		int rb = mask_bits(pf->dwRBitMask), gb = mask_bits(pf->dwGBitMask), bb = mask_bits(pf->dwBBitMask);
		for (i = 0; i < 65536; i++) {
			DWORD r = (i & pf->dwRBitMask) >> rs, g = (i & pf->dwGBitMask) >> gs, b = (i & pf->dwBBitMask) >> bs;
			r = (r << (8 - rb)) | (r >> (2 * rb - 8));
			g = (g << (8 - gb)) | (g >> (2 * gb - 8));
			b = (b << (8 - bb)) | (b >> (2 * bb - 8));
			lut[i] = (r << 16) | (g << 8) | b;
		}
		rm = pf->dwRBitMask; gm = pf->dwGBitMask; bm = pf->dwBBitMask;
	}
	for (i = 0; i < n; i++) o[i] = lut[in[i]];
}

enum { DD_CreateSurface = 6, DD_EnumDisplayModes = 8, DD_FlipToGDISurface = 10,
       DD_GetDisplayMode = 12, DD_RestoreDisplayMode = 19, DD_SetCooperativeLevel = 20,
       DD_SetDisplayMode = 21, DD_TestCooperativeLevel = 26, DD_GetDeviceIdentifier = 27 };

#define DD_ORIG(obj, slot) orig_in(g_ddvt, g_nddvt, obj, slot)

typedef HRESULT (WINAPI *CreateSurface_t)(void *, LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE7 *, IUnknown *);

/* The main view by the card: its back buffer blitted to the screen through a
 * clipper on the window, never read back - the copy that costs the game's
 * thread a millisecond and more a frame at 1600x1200 is gone, and the card
 * scales. [main] gpu_present = 0 reads it back and draws it as the other
 * displays are; so does a snapshot's frame, and anything the card refuses. */
static int gpu_present(struct dev *d, LPDIRECTDRAWSURFACE7 src)
{
	static int off = -1;
	RECT box, rc;
	HRESULT hr;

	if (off < 0) off = !cfg_int("main", "gpu_present", 1);
	if (off || !d->hwnd || !IsWindow(d->hwnd)) return 0;
	if (!d->screen) {
		CreateSurface_t create = (CreateSurface_t)DD_ORIG(d->dd, DD_CreateSurface);
		DDSURFACEDESC2 sd;

		memset(&sd, 0, sizeof sd);
		sd.dwSize = sizeof sd;
		sd.dwFlags = DDSD_CAPS;
		sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
		hr = create(d->dd, &sd, &d->screen, NULL);
		if (SUCCEEDED(hr)) hr = IDirectDraw7_CreateClipper((LPDIRECTDRAW7)d->dd, 0, &d->clip, NULL);
		if (SUCCEEDED(hr)) hr = IDirectDrawClipper_SetHWnd(d->clip, 0, d->hwnd);
		if (SUCCEEDED(hr)) hr = IDirectDrawSurface7_SetClipper(d->screen, d->clip);
		if (FAILED(hr)) {
			say("main view: the card cannot present it (%08lX); reading it back instead", (unsigned long)hr);
			off = 1;
			return 0;
		}
		say("main view: presented by the card");
	}
	view_box(d->hwnd, d->w, d->h, &box);
	GetClientRect(d->hwnd, &rc);
	if (box.left > 0 || box.top > 0) {		/* the bars */
		HDC dc = GetDC(d->hwnd);
		HBRUSH k = (HBRUSH)GetStockObject(BLACK_BRUSH);
		RECT b;
		SetRect(&b, 0, 0, rc.right, box.top);			FillRect(dc, &b, k);
		SetRect(&b, 0, box.bottom, rc.right, rc.bottom);	FillRect(dc, &b, k);
		SetRect(&b, 0, box.top, box.left, box.bottom);		FillRect(dc, &b, k);
		SetRect(&b, box.right, box.top, rc.right, box.bottom);	FillRect(dc, &b, k);
		ReleaseDC(d->hwnd, dc);
	}
	if (box.right <= box.left || box.bottom <= box.top) return 1;	/* minimised */
	MapWindowPoints(d->hwnd, NULL, (POINT *)&box, 2);
	hr = IDirectDrawSurface7_Blt(d->screen, &box, src, NULL, DDBLT_WAIT, NULL);
	if (hr == DDERR_SURFACELOST) {
		IDirectDrawSurface7_Restore(d->screen);
		hr = IDirectDrawSurface7_Blt(d->screen, &box, src, NULL, DDBLT_WAIT, NULL);
	}
	if (FAILED(hr)) {
		say("main view: the card refused the blit (%08lX); reading it back instead", (unsigned long)hr);
		off = 1;
		return 0;
	}
	return 1;
}

static void present(struct dev *d)
{
	LARGE_INTEGER t;
	LPDIRECTDRAWSURFACE7 src = d->back ? d->back : d->prim;
	DDSURFACEDESC2 sd;
	HRESULT hr;
	DWORD x, y;

	if (!src || !d->w) return;
	ms_since(&t);
	if (d->kind == K_MAIN && !snap_due(d, d->frames + 1) && gpu_present(d, src)) {
		if (++d->frames == 1)
			say("%s%d: first frame, %lux%lu", KIND[d->kind], d->serial, d->w, d->h);
		g_t_draw[0] += ms_since(&t);
		frame_cap();
		return;
	}
	if (!d->rgb) d->rgb = calloc((size_t)d->w * d->h, 4);
	memset(&sd, 0, sizeof sd);
	sd.dwSize = sizeof sd;
	hr = IDirectDrawSurface7_Lock(src, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_NOSYSLOCK, NULL);
	if (FAILED(hr)) {
		if (d->lockfail++ < 5) say("%s%d: cannot lock the back buffer (%08lX)", KIND[d->kind], d->serial, (unsigned long)hr);
		return;
	}
	{
		DDPIXELFORMAT *pf = &sd.ddpfPixelFormat;
		int bpp = pf->dwRGBBitCount;
		DWORD w = sd.dwWidth < d->w ? sd.dwWidth : d->w, h = sd.dwHeight < d->h ? sd.dwHeight : d->h;

		for (y = 0; y < h; y++) {
			const uint8_t *row = (const uint8_t *)sd.lpSurface + y * sd.lPitch;
			uint32_t *o = d->rgb + y * d->w;
			if (bpp == 32) {
				for (x = 0; x < w; x++) o[x] = ((const uint32_t *)row)[x] & 0xFFFFFF;
			} else if (bpp == 16)
				conv16(o, (const uint16_t *)row, w, pf);
		}
	}
	IDirectDrawSurface7_Unlock(src, NULL);
	g_t_copy[d->kind != K_MAIN] += ms_since(&t);
	d->frames++;
	if (d->frames == 1)
		say("%s%d: first frame, %lux%lu", KIND[d->kind], d->serial, d->w, d->h);

	if (d->kind == K_MFD) {
		mfd_present(d);
		stream_offer(d);
	} else if (d->kind == K_MAIN) draw_view(d->hwnd, d->rgb, d->w, d->h);
	else if (d->kind == K_RADAR && radar_rotation()) radar_present(d);
	else draw_rgb(d->hwnd, d->rgb, d->w, d->h, 0, 0, d->w, d->h);
	g_t_draw[d->kind != K_MAIN] += ms_since(&t);
	snapshot(d);
	if (d->kind == K_MAIN) frame_cap();
}

/* ------------------------------------------------ IDirectDrawSurface7 */

enum { SF_EnumAttachedSurfaces = 9, SF_Flip = 11, SF_GetAttachedSurface = 12, SF_GetCaps = 14,
       SF_GetSurfaceDesc = 22 };

#define SF_ORIG(obj, slot) orig_in(g_sfvt, g_nsfvt, obj, slot)

typedef HRESULT (WINAPI *Flip_t)(void *, LPDIRECTDRAWSURFACE7, DWORD);
typedef HRESULT (WINAPI *GetAtt_t)(void *, LPDDSCAPS2, LPDIRECTDRAWSURFACE7 *);
typedef HRESULT (WINAPI *EnumAtt_t)(void *, LPVOID, LPDDENUMSURFACESCALLBACK7);
typedef HRESULT (WINAPI *GetCaps_t)(void *, LPDDSCAPS2);
typedef HRESULT (WINAPI *GetDesc_t)(void *, LPDDSURFACEDESC2);

static void fake_caps(struct dev *d, void *s, DWORD *caps)
{
	if (s == d->prim) {
		*caps &= ~DDSCAPS_OFFSCREENPLAIN;
		*caps |= DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE |
			 (d->back ? DDSCAPS_FLIP | DDSCAPS_COMPLEX : 0);
	}
}

static HRESULT WINAPI h_Flip(void *s, LPDIRECTDRAWSURFACE7 target, DWORD flags)
{
	struct dev *d = dev_of_prim(s);
	static int foreign;
	if (!d) {
		if (foreign++ < 3) say("Flip on surface %p, not one of ours", s);
		return ((Flip_t)SF_ORIG(s, SF_Flip))(s, target, flags);
	}
	present(d);
	return DD_OK;
}

static HRESULT WINAPI h_GetAttachedSurface(void *s, LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7 *out)
{
	struct dev *d = dev_of_prim(s);
	if (d && d->back && (caps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP))) {
		IDirectDrawSurface7_AddRef(d->back);
		*out = d->back;
		return DD_OK;
	}
	return ((GetAtt_t)SF_ORIG(s, SF_GetAttachedSurface))(s, caps, out);
}

static HRESULT WINAPI h_EnumAttachedSurfaces(void *s, LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb)
{
	struct dev *d = dev_of_prim(s);
	if (d && d->back) {
		DDSURFACEDESC2 sd;
		memset(&sd, 0, sizeof sd);
		sd.dwSize = sizeof sd;
		((GetDesc_t)SF_ORIG(d->back, SF_GetSurfaceDesc))(d->back, &sd);
		sd.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
		IDirectDrawSurface7_AddRef(d->back);
		cb(d->back, &sd, ctx);
		return DD_OK;
	}
	return ((EnumAtt_t)SF_ORIG(s, SF_EnumAttachedSurfaces))(s, ctx, cb);
}

static HRESULT WINAPI h_GetCaps(void *s, LPDDSCAPS2 caps)
{
	HRESULT hr = ((GetCaps_t)SF_ORIG(s, SF_GetCaps))(s, caps);
	struct dev *d = dev_of_prim(s);
	if (SUCCEEDED(hr) && d) fake_caps(d, s, &caps->dwCaps);
	return hr;
}

static HRESULT WINAPI h_GetSurfaceDesc(void *s, LPDDSURFACEDESC2 sd)
{
	HRESULT hr = ((GetDesc_t)SF_ORIG(s, SF_GetSurfaceDesc))(s, sd);
	struct dev *d = dev_of_prim(s);
	if (SUCCEEDED(hr) && d) {
		fake_caps(d, s, &sd->ddsCaps.dwCaps);
		if (d->back) { sd->dwFlags |= DDSD_BACKBUFFERCOUNT; sd->dwBackBufferCount = 1; }
	}
	return hr;
}

static void hook_surface(void *s)
{
	static const int slots[] = { SF_EnumAttachedSurfaces, SF_Flip, SF_GetAttachedSurface,
				     SF_GetCaps, SF_GetSurfaceDesc };
	static void *const fns[] = { h_EnumAttachedSurfaces, h_Flip, h_GetAttachedSurface,
				     h_GetCaps, h_GetSurfaceDesc };
	hook_vtable(g_sfvt, &g_nsfvt, s, 5, slots, fns, "IDirectDrawSurface7");
}

/* ------------------------------------------------ IDirectDraw7 */

typedef HRESULT (WINAPI *EnumModes_t)(void *, DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMMODESCALLBACK2);
typedef HRESULT (WINAPI *NoArg_t)(void *);
typedef HRESULT (WINAPI *GetMode_t)(void *, LPDDSURFACEDESC2);
typedef HRESULT (WINAPI *SetCoop_t)(void *, HWND, DWORD);
typedef HRESULT (WINAPI *SetMode_t)(void *, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *GetId_t)(void *, LPDDDEVICEIDENTIFIER2, DWORD);

static int g_ncreate;

/* The primary the game asks for becomes an off-screen surface of the
 * emulated mode, with its back buffer beside it rather than attached: the
 * hooks above make the pair look like a flipping chain. */
static HRESULT fake_primary(struct dev *d, LPDDSURFACEDESC2 want, LPDIRECTDRAWSURFACE7 *out)
{
	CreateSurface_t create = (CreateSurface_t)DD_ORIG(d->dd, DD_CreateSurface);
	DDSURFACEDESC2 sd;
	HRESULT hr;
	int i, nsurf = (want->dwFlags & DDSD_BACKBUFFERCOUNT) && want->dwBackBufferCount ? 2 : 1;
	LPDIRECTDRAWSURFACE7 s[2] = { NULL, NULL };

	if (!d->w) {			/* no mode set: the desktop's size */
		d->w = GetSystemMetrics(SM_CXSCREEN);
		d->h = GetSystemMetrics(SM_CYSCREEN);
		d->bpp = 32;
	}
	for (i = 0; i < nsurf; i++) {
		memset(&sd, 0, sizeof sd);
		sd.dwSize = sizeof sd;
		sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
		sd.dwWidth = d->w;
		sd.dwHeight = d->h;
		pixfmt(&sd.ddpfPixelFormat, d->bpp);
		sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY |
				    (want->ddsCaps.dwCaps & DDSCAPS_3DDEVICE);
		hr = create(d->dd, &sd, &s[i], NULL);
		if (FAILED(hr)) {
			sd.ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
			hr = create(d->dd, &sd, &s[i], NULL);
		}
		if (FAILED(hr)) {
			say("%s%d: cannot make the %s surface %lux%lu %lu bpp (%08lX)", KIND[d->kind],
			    d->serial, i ? "back" : "primary", d->w, d->h, d->bpp, (unsigned long)hr);
			if (s[0]) IDirectDrawSurface7_Release(s[0]);
			return hr;
		}
		forget_surface(s[i]);
		hook_surface(s[i]);
	}
	d->prim = s[0];
	d->back = s[1];
	*out = s[0];
	say("%s%d: primary %p, back buffer %p, %lux%lu %lu bpp, standing in for caps %08lX",
	    KIND[d->kind], d->serial, (void *)s[0], (void *)s[1], d->w, d->h, d->bpp,
	    want->ddsCaps.dwCaps);
	return DD_OK;
}

static HRESULT WINAPI h_CreateSurface(void *dd, LPDDSURFACEDESC2 sd, LPDIRECTDRAWSURFACE7 *out, IUnknown *u)
{
	struct dev *d = dev_of(dd);
	HRESULT hr;

	DDSURFACEDESC2 own;

	if (d && d->emulated && (sd->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
		return fake_primary(d, sd, out);
	/* A surface asked for without a pixel format gets the display's - the
	 * desktop's 32 bits, not the 16 the game set and was told it has - and
	 * the game writes 16-bit pixels into it: the MFD's picture of the
	 * target came out striped, half of it off the side. Such surfaces get
	 * the mode's format. */
	if (d && d->emulated && d->bpp && !(sd->dwFlags & DDSD_PIXELFORMAT) &&
	    (sd->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) == (DDSD_WIDTH | DDSD_HEIGHT) && sd->dwHeight &&
	    !(sd->ddsCaps.dwCaps & (DDSCAPS_ZBUFFER | 0x00800000 /* DDSCAPS_EXECUTEBUFFER */))) {
		HDC dc = GetDC(NULL);
		DWORD desk = (DWORD)GetDeviceCaps(dc, BITSPIXEL);
		ReleaseDC(NULL, dc);
		if (desk != d->bpp) {
			static int told;
			own = *sd;
			own.dwFlags |= DDSD_PIXELFORMAT;
			pixfmt(&own.ddpfPixelFormat, d->bpp);
			if (told++ < 20)
				say("%s%d: a %lux%lu surface (caps %08lX) made %lu bpp, the mode's, not the desktop's %lu",
				    KIND[d->kind], d->serial, sd->dwWidth, sd->dwHeight, sd->ddsCaps.dwCaps, d->bpp, desk);
			sd = &own;
		}
	}
	hr = ((CreateSurface_t)DD_ORIG(dd, DD_CreateSurface))(dd, sd, out, u);
	if (SUCCEEDED(hr)) { forget_surface(*out); hook_surface(*out); }
	if (g_ncreate++ < 60 || FAILED(hr))
		say("dd %p CreateSurface flags %08lX caps %08lX %lux%lu -> %08lX", dd, sd->dwFlags,
		    sd->ddsCaps.dwCaps, sd->dwWidth, sd->dwHeight, (unsigned long)hr);
	return hr;
}

/* What the virtual adapters offer: 1280x480 first, since that is the mode
 * the game looks for, and 16-bit first, since that is the depth it asks for. */
static const DWORD VMODES[][2] = { { 1280, 480 }, { 640, 480 }, { 800, 600 }, { 1024, 768 } };

static HRESULT WINAPI h_EnumDisplayModes(void *dd, DWORD flags, LPDDSURFACEDESC2 match, LPVOID ctx, LPDDENUMMODESCALLBACK2 cb)
{
	struct dev *d = dev_of(dd);
	int i, b;

	if (!d || (d->kind != K_MFD && d->kind != K_RADAR))
		return ((EnumModes_t)DD_ORIG(dd, DD_EnumDisplayModes))(dd, flags, match, ctx, cb);
	for (b = 0; b < 2; b++)
		for (i = 0; i < (int)(sizeof VMODES / sizeof VMODES[0]); i++) {
			DDSURFACEDESC2 sd;
			DWORD bpp = b ? 32 : 16;
			if (match && (match->dwFlags & DDSD_WIDTH) && match->dwWidth != VMODES[i][0]) continue;
			if (match && (match->dwFlags & DDSD_HEIGHT) && match->dwHeight != VMODES[i][1]) continue;
			if (match && (match->dwFlags & DDSD_PIXELFORMAT) &&
			    match->ddpfPixelFormat.dwRGBBitCount != bpp) continue;
			memset(&sd, 0, sizeof sd);
			sd.dwSize = sizeof sd;
			sd.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
			sd.dwWidth = VMODES[i][0];
			sd.dwHeight = VMODES[i][1];
			sd.lPitch = VMODES[i][0] * bpp / 8;
			sd.dwRefreshRate = 60;
			pixfmt(&sd.ddpfPixelFormat, bpp);
			if (cb(&sd, ctx) == DDENUMRET_CANCEL) return DD_OK;
		}
	say("dd %p (%s): offered the virtual modes", dd, KIND[d->kind]);
	return DD_OK;
}

static HRESULT WINAPI h_SetCooperativeLevel(void *dd, HWND w, DWORD flags)
{
	struct dev *d = dev_of(dd);
	DWORD keep = flags & (DDSCL_MULTITHREADED | DDSCL_FPUSETUP | DDSCL_FPUPRESERVE);
	HRESULT hr;

	/* Only a device going full screen is emulated. The game also opens the
	 * virtual adapters windowed, to enumerate them; that needs nothing. */
	if (!(flags & DDSCL_FULLSCREEN) && !(d && d->emulated)) {
		hr = ((SetCoop_t)DD_ORIG(dd, DD_SetCooperativeLevel))(dd, w, flags);
		say("dd %p SetCooperativeLevel hwnd %p flags %08lX (passed through) -> %08lX", dd,
		    (void *)w, flags, (unsigned long)hr);
		return hr;
	}

	if (!d) d = dev_new(dd, K_MAIN);
	if (!d) return DDERR_GENERIC;
	if (!d->emulated) {
		d->emulated = 1;
		d->serial = ++g_serial[d->kind];
		if (d->kind == K_MAIN) {
			d->hwnd = w;
		} else if (d->kind == K_RADAR && cfg_int("shim", "cockpit_windows", g_cockpit)) {
			/* The game makes a new radar device for each mission; they
			 * all draw into the one window. */
			char lay[16];
			cfg_str("mfd", "layout", "windows", lay, sizeof lay);
			if (!g_radar_win)		/* beside the MFD windows, or under the wall */
				g_radar_win = make_window("Radar", "radar", _stricmp(lay, "wall") ? 990 : 0,
							  _stricmp(lay, "wall") ? 0 : 290,
							  radar_rotation() ? 480 : 640, radar_rotation() ? 640 : 480);
			d->hwnd = g_radar_win;
		}
		/* The MFD device draws into the five MFD windows; it needs a
		 * window of its own only for DirectDraw's sake. */
	}
	hr = ((SetCoop_t)DD_ORIG(dd, DD_SetCooperativeLevel))(dd, d->kind == K_MAIN ? w : NULL,
								DDSCL_NORMAL | keep);
	say("dd %p (%s%d) SetCooperativeLevel hwnd %p flags %08lX: windowed instead -> %08lX",
	    dd, KIND[d->kind], d->serial, (void *)w, flags, (unsigned long)hr);
	return hr;
}

/* A pod's game is always in front; on a PC it often is not, and GameOS stops
 * - the cockpit freezes, a pod in the background misses the start of a
 * mission - whenever its window loses focus. With [shim] always_active (the
 * default) the main window is told it is active whatever happens, and
 * GetForegroundWindow, which GameOS also asks, says the same. */
static WNDPROC g_main_proc;
static HWND g_main_hwnd;

static LRESULT CALLBACK main_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m) {
	case WM_ACTIVATEAPP:
		if (!wp) wp = TRUE;
		break;
	case WM_ACTIVATE:
		if (LOWORD(wp) == WA_INACTIVE) wp = MAKEWPARAM(WA_ACTIVE, HIWORD(wp));
		break;
	case WM_KILLFOCUS:
		return 0;
	}
	return CallWindowProcA(g_main_proc, w, m, wp, lp);
}

static void keep_active(HWND w)
{
	if (g_main_hwnd == w || !cfg_int("shim", "always_active", 1)) return;
	g_main_hwnd = w;
	g_main_proc = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)main_proc);
	say("main window %p kept active whatever has focus", (void *)w);
}

struct monfind { int want, n; RECT r; };

static BOOL CALLBACK monitor_cb(HMONITOR hm, HDC dc, LPRECT r, LPARAM p)
{
	struct monfind *f = (struct monfind *)p;
	MONITORINFO mi;

	mi.cbSize = sizeof mi;
	GetMonitorInfoA(hm, &mi);
	if (mi.dwFlags & MONITORINFOF_PRIMARY) return TRUE;	/* the primary is 1 */
	if (++f->n == f->want) { f->r = mi.rcMonitor; return FALSE; }
	return TRUE;
}

static void monitor_rect(int n, RECT *r)
{
	struct monfind f;

	memset(&f, 0, sizeof f);
	f.want = n;
	f.n = 1;
	SetRect(&f.r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
	if (n > 1) EnumDisplayMonitors(NULL, NULL, monitor_cb, (LPARAM)&f);
	*r = f.r;
}

static HRESULT WINAPI h_SetDisplayMode(void *dd, DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD flags)
{
	struct dev *d = dev_of(dd);

	if (!d || !d->emulated)
		return ((SetMode_t)DD_ORIG(dd, DD_SetDisplayMode))(dd, w, h, bpp, hz, flags);
	d->w = w; d->h = h; d->bpp = bpp == 16 ? 16 : 32;
	free(d->rgb);
	d->rgb = NULL;
	if (d->kind == K_MAIN && d->hwnd) {
		RECT rc;
		keep_active(d->hwnd);
		DWORD style = GetWindowLongA(d->hwnd, GWL_STYLE), ex = GetWindowLongA(d->hwnd, GWL_EXSTYLE);
		int x = cfg_int("main", "x", 0), y = cfg_int("main", "y", 0);
		int cw = cfg_int("main", "w", w), ch = cfg_int("main", "h", h);

		if (cfg_int("main", "fullscreen", 0)) {
			/* [main] fullscreen: borderless over a whole monitor -
			 * [main] monitor, 1 the primary, 2 the next... */
			RECT m;
			monitor_rect(cfg_int("main", "monitor", 1), &m);
			SetWindowLongA(d->hwnd, GWL_STYLE, (style & ~(WS_OVERLAPPEDWINDOW | WS_CAPTION)) | WS_POPUP);
			SetWindowPos(d->hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
				     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			say("main view: full screen on %ld,%ld %ldx%ld", m.left, m.top, m.right - m.left, m.bottom - m.top);
			return DD_OK;
		}
		if (style & WS_POPUP) {			/* a full-screen window: give it a frame */
			style = (style & ~WS_POPUP) | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
			SetWindowLongA(d->hwnd, GWL_STYLE, style);
		}
		ex &= ~WS_EX_TOPMOST;
		SetWindowLongA(d->hwnd, GWL_EXSTYLE, ex);
		SetRect(&rc, 0, 0, cw, ch);
		AdjustWindowRectEx(&rc, style, GetMenu(d->hwnd) != NULL, ex);
		SetWindowPos(d->hwnd, HWND_NOTOPMOST, x, y,
			     rc.right - rc.left, rc.bottom - rc.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	}
	say("dd %p (%s%d) SetDisplayMode %lux%lu %lu bpp %lu Hz: emulated", dd, KIND[d->kind],
	    d->serial, w, h, bpp, hz);
	return DD_OK;
}

static HRESULT WINAPI h_GetDisplayMode(void *dd, LPDDSURFACEDESC2 sd)
{
	struct dev *d = dev_of(dd);

	if (!d || !d->emulated || !d->w)
		return ((GetMode_t)DD_ORIG(dd, DD_GetDisplayMode))(dd, sd);
	sd->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
	sd->dwWidth = d->w;
	sd->dwHeight = d->h;
	sd->lPitch = d->w * d->bpp / 8;
	sd->dwRefreshRate = 60;
	pixfmt(&sd->ddpfPixelFormat, d->bpp);
	return DD_OK;
}

static HRESULT WINAPI h_RestoreDisplayMode(void *dd)
{
	struct dev *d = dev_of(dd);
	if (d && d->emulated) return DD_OK;
	return ((NoArg_t)DD_ORIG(dd, DD_RestoreDisplayMode))(dd);
}

static HRESULT WINAPI h_FlipToGDISurface(void *dd)
{
	struct dev *d = dev_of(dd);
	if (d && d->emulated) return DD_OK;
	return ((NoArg_t)DD_ORIG(dd, DD_FlipToGDISurface))(dd);
}

static HRESULT WINAPI h_TestCooperativeLevel(void *dd)
{
	struct dev *d = dev_of(dd);
	if (d && d->emulated) return DD_OK;
	return ((NoArg_t)DD_ORIG(dd, DD_TestCooperativeLevel))(dd);
}

/* The game drops any adapter whose identifier matches the primary's, so a
 * virtual adapter on the same card has to say it is a different one. */
static HRESULT WINAPI h_GetDeviceIdentifier(void *dd, LPDDDEVICEIDENTIFIER2 id, DWORD flags)
{
	HRESULT hr = ((GetId_t)DD_ORIG(dd, DD_GetDeviceIdentifier))(dd, id, flags);
	struct dev *d = dev_of(dd);

	if (SUCCEEDED(hr) && d && (d->kind == K_MFD || d->kind == K_RADAR)) {
		id->guidDeviceIdentifier.Data1 ^= 0x46530000 | d->kind;
		id->dwDeviceId ^= d->kind;
		char real[MAX_DDDEVICEID_STRING];
		memcpy(real, id->szDescription, sizeof real);
		real[sizeof real - 1] = 0;
		snprintf(id->szDescription, sizeof id->szDescription, "Firestorm %s (virtual, on %.400s)",
			 d->kind == K_MFD ? "MFD" : "radar", real);
	}
	return hr;
}

static void hook_dd7(void *obj)
{
	static const int slots[] = { DD_CreateSurface, DD_EnumDisplayModes, DD_FlipToGDISurface,
				     DD_GetDisplayMode, DD_RestoreDisplayMode, DD_SetCooperativeLevel,
				     DD_SetDisplayMode, DD_TestCooperativeLevel, DD_GetDeviceIdentifier };
	static void *const fns[] = { h_CreateSurface, h_EnumDisplayModes, h_FlipToGDISurface,
				     h_GetDisplayMode, h_RestoreDisplayMode, h_SetCooperativeLevel,
				     h_SetDisplayMode, h_TestCooperativeLevel, h_GetDeviceIdentifier };
	hook_vtable(g_ddvt, &g_nddvt, obj, 9, slots, fns, "IDirectDraw7");
}

/* ------------------------------------------------ the exports we provide */

static int virtual_kind(const GUID *g)
{
	if (!g || (ULONG_PTR)g < 0x10000) return K_NONE;
	if (IsEqualGUID(g, &FS_GUID_MFD)) return K_MFD;
	if (IsEqualGUID(g, &FS_GUID_RADAR)) return K_RADAR;
	return K_NONE;
}

HRESULT WINAPI sh_DirectDrawCreateEx(GUID *g, LPVOID *out, REFIID iid, IUnknown *u)
{
	char a[64];
	int kind = virtual_kind(g);
	HRESULT hr;

	if (!load_real()) return DDERR_GENERIC;
	hr = r_DirectDrawCreateEx(kind ? NULL : g, out, iid, u);
	say("DirectDrawCreateEx adapter %s%s -> %08lX dd %p", guidstr(g, a),
	    kind ? (kind == K_MFD ? " (virtual MFD)" : " (virtual radar)") : "", (unsigned long)hr,
	    out ? *out : NULL);
	if (SUCCEEDED(hr) && IsEqualGUID(iid, &IID_IDirectDraw7)) {
		EnterCriticalSection(&g_lock);
		if (kind) dev_new(*out, kind);
		else { struct dev *d = dev_of(*out); if (d) d->dd = NULL; }
		LeaveCriticalSection(&g_lock);
		hook_dd7(*out);
	}
	return hr;
}

HRESULT WINAPI sh_DirectDrawCreate(GUID *g, LPDIRECTDRAW *out, IUnknown *u)
{
	char a[64];
	HRESULT hr;

	if (!load_real()) return DDERR_GENERIC;
	hr = r_DirectDrawCreate(virtual_kind(g) ? NULL : g, out, u);
	say("DirectDrawCreate adapter %s -> %08lX", guidstr(g, a), (unsigned long)hr);
	return hr;
}

/* Only the primary display is passed on: Firestorm would drop the monitors
 * of the same card as duplicates anyway, and the virtual adapters stand in
 * for the pod's other cards. */
struct enumctx { LPDDENUMCALLBACKEXA cb; LPDDENUMCALLBACKA cba; LPVOID ctx; int n, stop; };

static BOOL WINAPI enum_cb(GUID *g, LPSTR desc, LPSTR name, LPVOID p, HMONITOR mon)
{
	struct enumctx *e = p;
	char a[64];

	if (g && !cfg_int("shim", "real_adapters", 0)) {
		say("  hidden: %s \"%s\" \"%s\"", guidstr(g, a), desc, name);
		return TRUE;
	}
	say("  adapter %d: %s \"%s\" \"%s\"", e->n++, guidstr(g, a), desc, name);
	if (e->cb ? !e->cb(g, desc, name, e->ctx, mon) : !e->cba(g, desc, name, e->ctx)) e->stop = 1;
	return !e->stop;
}

static BOOL WINAPI enum_cb_a(GUID *g, LPSTR desc, LPSTR name, LPVOID p)
{
	return enum_cb(g, desc, name, p, NULL);
}

static void enum_virtual(struct enumctx *e)
{
	static char d1[] = "Firestorm MFD (virtual)", n1[] = "\\\\.\\FSMFD";
	static char d2[] = "Firestorm radar (virtual)", n2[] = "\\\\.\\FSRADAR";
	HMONITOR m = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

	if (e->stop) return;
	say("  adapter %d: virtual MFD", e->n++);
	if (e->cb ? !e->cb((GUID *)&FS_GUID_MFD, d1, n1, e->ctx, m) : !e->cba((GUID *)&FS_GUID_MFD, d1, n1, e->ctx)) return;
	say("  adapter %d: virtual radar", e->n++);
	if (e->cb) e->cb((GUID *)&FS_GUID_RADAR, d2, n2, e->ctx, m);
	else e->cba((GUID *)&FS_GUID_RADAR, d2, n2, e->ctx);
}

HRESULT WINAPI sh_DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD flags)
{
	struct enumctx e = { cb, NULL, ctx, 0, 0 };
	HRESULT hr;

	if (!load_real()) return DDERR_GENERIC;
	say("DirectDrawEnumerateExA flags %08lX", flags);
	hr = r_DirectDrawEnumerateExA(enum_cb, &e, flags);
	enum_virtual(&e);
	say("DirectDrawEnumerateExA -> %08lX, %d adapters offered", (unsigned long)hr, e.n);
	return hr;
}

HRESULT WINAPI sh_DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx)
{
	struct enumctx e = { NULL, cb, ctx, 0, 0 };
	HRESULT hr;

	if (!load_real()) return DDERR_GENERIC;
	say("DirectDrawEnumerateA");
	hr = r_DirectDrawEnumerateA(enum_cb_a, &e);
	enum_virtual(&e);
	return hr;
}

/* ------------------------------------------------ c:\ctcl.ini */

/* On a pod, postinstall.bat copies the site's ctcl.ini - every pod's address
 * and pilot name - to c:\ctcl.ini, and the game refuses to run without one
 * ("no valid tesla infos!!"). The site also kept a folder c:\mw4files, shared
 * on the network, where the console writes a daily report
 * (c:\mw4files\MM_DD_YYYY.rpt); without it the console retries the write for
 * ever and hangs on LOAD. An ordinary account cannot create files in the root
 * of C:, so MW4.exe's imports of the file and profile functions are
 * redirected: c:\ctcl.ini becomes ctcl.ini beside the game, and c:\mw4files
 * becomes mw4files beside the game. */
static char g_ctcl[MAX_PATH], g_files[MAX_PATH];

static const char *redir_buf(const char *p, char *buf)
{
	if (!p || (ULONG_PTR)p < 0x10000) return p;
	if (!_stricmp(p, "c:\\ctcl.ini")) return g_ctcl;
	if (!_strnicmp(p, "c:\\mw4files", 11) && (p[11] == 0 || p[11] == '\\' || p[11] == '/')) {
		snprintf(buf, MAX_PATH, "%s%s", g_files, p + 11);
		return buf;
	}
	/* The console reads a pod's records over the network, from the pod's
	 * c:\mw4files shared as \\pod\mw4files; on one PC that is the pod's
	 * folder here ([shim] pod_files, default ctcl2). */
	if (p[0] == '\\' && p[1] == '\\') {
		const char *q = strchr(p + 2, '\\');
		if (q && !_strnicmp(q, "\\mw4files", 9) && (q[9] == 0 || q[9] == '\\')) {
			char pod[32];
			cfg_str("shim", "pod_files", "ctcl2", pod, sizeof pod);
			snprintf(buf, MAX_PATH, "%smw4files\\%s%s", g_dir, pod, q + 9);
			return buf;
		}
	}
	return p;
}

/* One buffer per thread is enough: each hook uses its result before returning. */
static __thread char t_redir[2][MAX_PATH];
#define redir(p)  redir_buf((p), t_redir[0])
#define redir2(p) redir_buf((p), t_redir[1])

typedef DWORD (WINAPI *GPPS_t)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
typedef UINT (WINAPI *GPPI_t)(LPCSTR, LPCSTR, INT, LPCSTR);
typedef BOOL (WINAPI *WPPS_t)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
typedef DWORD (WINAPI *GFA_t)(LPCSTR);
typedef HANDLE (WINAPI *CFA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef FILE *(__cdecl *fopen_t)(const char *, const char *);
typedef int (__cdecl *open_t)(const char *, int, int);
static GPPS_t o_GPPS; static GPPI_t o_GPPI; static WPPS_t o_WPPS; static GFA_t o_GFA;
static CFA_t o_CFA; static fopen_t o_fopen; static open_t o_open;

static DWORD WINAPI i_GPPS(LPCSTR s, LPCSTR k, LPCSTR d, LPSTR o, DWORD n, LPCSTR f) { return o_GPPS(s, k, d, o, n, redir(f)); }
static UINT WINAPI i_GPPI(LPCSTR s, LPCSTR k, INT d, LPCSTR f) { return o_GPPI(s, k, d, redir(f)); }
static BOOL WINAPI i_WPPS(LPCSTR s, LPCSTR k, LPCSTR v, LPCSTR f) { return o_WPPS(s, k, v, redir(f)); }
static DWORD WINAPI i_GFA(LPCSTR f) { return o_GFA(redir(f)); }
static HANDLE rio_open(const char *name);
static HANDLE plasma_open(const char *name);
static int is_plasma_port(const char *f);
static void plasma_draw(void);
static HANDLE WINAPI i_CFA(LPCSTR f, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD fl, HANDLE t)
{
	/* "COMn" is the RIO board's port; the plasma sign's is "\\.\comN". */
	if (is_plasma_port(f)) return plasma_open(f);
	if (f && (ULONG_PTR)f >= 0x10000 && !_strnicmp(f, "COM", 3) && isdigit((unsigned char)f[3]) &&
	    cfg_int("rio", "enabled", g_cockpit))
		return rio_open(f);
	if (g_launcher && f && (ULONG_PTR)f >= 0x10000) {	/* the helpers open few files: say which */
		HANDLE h = o_CFA(redir(f), a, sh, sa, c, fl, t);
		say("open %s -> %s", f, h == INVALID_HANDLE_VALUE ? "failed" : redir(f));
		return h;
	}
	return o_CFA(redir(f), a, sh, sa, c, fl, t);
}
static FILE *__cdecl i_fopen(const char *f, const char *m)
{
	FILE *r = o_fopen(redir(f), m);
	if (g_launcher && f) say("fopen %s (%s) -> %s", f, m, r ? redir(f) : "failed");
	return r;
}
static int __cdecl i_open(const char *f, int fl, int pm) { return o_open(redir(f), fl, pm); }

typedef BOOL (WINAPI *DFA_t)(LPCSTR);
typedef BOOL (WINAPI *SFA_t)(LPCSTR, DWORD);
typedef BOOL (WINAPI *MFA_t)(LPCSTR, LPCSTR);
typedef BOOL (WINAPI *CDA_t)(LPCSTR, LPSECURITY_ATTRIBUTES);
typedef HANDLE (WINAPI *FFFA_t)(LPCSTR, LPWIN32_FIND_DATAA);
typedef BOOL (WINAPI *CPFA_t)(LPCSTR, LPCSTR, BOOL);
static DFA_t o_DeleteFileA; static SFA_t o_SetFileAttributesA; static MFA_t o_MoveFileA;
static CDA_t o_CreateDirectoryA; static FFFA_t o_FindFirstFileA; static CPFA_t o_CopyFileA;
static BOOL WINAPI i_DeleteFileA(LPCSTR f) { return o_DeleteFileA(redir(f)); }
static BOOL WINAPI i_SetFileAttributesA(LPCSTR f, DWORD a) { return o_SetFileAttributesA(redir(f), a); }
static BOOL WINAPI i_MoveFileA(LPCSTR a, LPCSTR b) { return o_MoveFileA(redir(a), redir2(b)); }
static BOOL WINAPI i_CreateDirectoryA(LPCSTR f, LPSECURITY_ATTRIBUTES sa) { return o_CreateDirectoryA(redir(f), sa); }
static HANDLE WINAPI i_FindFirstFileA(LPCSTR f, LPWIN32_FIND_DATAA fd) { return o_FindFirstFileA(redir(f), fd); }
static BOOL WINAPI i_CopyFileA(LPCSTR a, LPCSTR b, BOOL x) { return o_CopyFileA(redir(a), redir2(b), x); }

/* Every path-taking file function MW4.exe imports goes through the same
 * redirect, the C runtime's included: the report writer checks its
 * destination with _access, and if that one function sees a different
 * c:\mw4files from MoveFileA the writer retries for ever and the pod hangs on
 * LAUNCH. */
typedef int (__cdecl *access_t)(const char *, int);
typedef int (__cdecl *remove_t)(const char *);
typedef DWORD (WINAPI *GFPN_t)(LPCSTR, DWORD, LPSTR, LPSTR *);
typedef BOOL (WINAPI *RDA_t)(LPCSTR);
static access_t o_access; static remove_t o_remove; static GFPN_t o_GetFullPathNameA; static RDA_t o_RemoveDirectoryA;
static int __cdecl i_access(const char *f, int m) { return o_access(redir(f), m); }
static int __cdecl i_remove(const char *f) { return o_remove(redir(f)); }
static DWORD WINAPI i_GetFullPathNameA(LPCSTR f, DWORD n, LPSTR b, LPSTR *p) { return o_GetFullPathNameA(redir(f), n, b, p); }
static BOOL WINAPI i_RemoveDirectoryA(LPCSTR f) { return o_RemoveDirectoryA(redir(f)); }

/* Point one import of the game's executable at our function. */
static void iat_hook(const char *dll, const char *fn, void *hook, void **orig)
{
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);

	for (; imp->Name; imp++) {
		IMAGE_THUNK_DATA *names, *addrs;
		if (_stricmp((char *)(base + imp->Name), dll)) continue;
		names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
		addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
		for (; names->u1.AddressOfData; names++, addrs++) {
			IMAGE_IMPORT_BY_NAME *ibn;
			DWORD old;
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
				if (fn[0] != '#' || IMAGE_ORDINAL(names->u1.Ordinal) != (ULONG_PTR)atoi(fn + 1)) continue;
			} else {
				if (fn[0] == '#') continue;
				ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
				if (strcmp((char *)ibn->Name, fn)) continue;
			}
			*orig = (void *)addrs->u1.Function;
			VirtualProtect(&addrs->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
			addrs->u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&addrs->u1.Function, sizeof(void *), old, &old);
			return;
		}
	}
	say("import %s!%s not found", dll, fn);
}

/* Who ends the process, logged with the caller's address: a pod that quits
 * silently says where it decided to. */
typedef void (WINAPI *ExitProcess_t)(UINT);
typedef void (__cdecl *exit_t)(int);
static ExitProcess_t o_ExitProcess;
static exit_t o_exit, o__exit;

static void WINAPI i_ExitProcess(UINT c) { say("ExitProcess(%u) from %p", c, __builtin_return_address(0)); o_ExitProcess(c); }
static void __cdecl i_exit(int c) { say("exit(%d) from %p", c, __builtin_return_address(0)); o_exit(c); }
static void __cdecl i__exit(int c) { say("_exit(%d) from %p", c, __builtin_return_address(0)); o__exit(c); }

/* GameOS lets one copy run: it looks for a window whose class and title are
 * both its own name, and if it finds one it brings it forward and quits. A
 * centre on one PC needs a console and a pod side by side, so that lookup -
 * and only that one; the game also looks for mw4print - finds nothing. */
typedef HWND (WINAPI *FindWindowA_t)(LPCSTR, LPCSTR);
static FindWindowA_t o_FindWindowA;

static char g_selfname[128];		/* the name GameOS checks itself by */

static HWND WINAPI i_FindWindowA(LPCSTR cls, LPCSTR title)
{
	if (cls && title && (ULONG_PTR)cls >= 0x10000 && !strcmp(cls, title) &&
	    cfg_int("shim", "allow_multiple", 1)) {
		snprintf(g_selfname, sizeof g_selfname, "%s", cls);
		say("single-instance check for \"%s\" answered: none running", cls);
		return NULL;
	}
	return o_FindWindowA(cls, title);
}

/* ... and then it makes a file mapping of that name and quits if the name is
 * taken. Each copy gets a name of its own. */
typedef HANDLE (WINAPI *CFM_t)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
static CFM_t o_CreateFileMappingA;

static HANDLE WINAPI i_CreateFileMappingA(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi,
					  DWORD lo, LPCSTR name)
{
	char own[160];
	if (name && g_selfname[0] && !strcmp(name, g_selfname)) {
		snprintf(own, sizeof own, "%s-%lu", name, GetCurrentProcessId());
		say("instance mapping \"%s\" made private as \"%s\"", name, own);
		name = own;
	}
	return o_CreateFileMappingA(f, sa, prot, hi, lo, name);
}

/* One game per PC is assumed twice over. MW4.exe has an 8-byte .SHARED_
 * section - memory every running copy shares - and quits at 0x40DFE0 if
 * its second word says a copy is already up; then it asks ctcls.dll, whose
 * state is shared the same way, for the application type, and quits if
 * another copy has set one. A console and a pod on one PC need neither, so
 * the word is cleared before the game checks it (fsrun loads the shim that
 * early), and the ten CTCL values become per process. They are how VWE's
 * launcher watched the game; there is no launcher here. */
static int ctcl_appl_state, ctcl_appl_type, ctcl_is_server, ctcl_game_state, ctcl_game_time;
static int __cdecl c_GetApplState(void) { return ctcl_appl_state; }
static void __cdecl c_SetApplState(int v) { ctcl_appl_state = v; }
static int __cdecl c_GetApplType(void) { static int n; if (n++ < 3) say("CTCL_GetApplType -> %d (from %p)", ctcl_appl_type, __builtin_return_address(0)); return ctcl_appl_type; }
static void __cdecl c_SetApplType(int v) { ctcl_appl_type = v; say("CTCL application type %d (private to this process)", v); }
static int (__cdecl *o_GetApplType)(void);
static int __cdecl c_StartupApplType(void)
{
	if (__builtin_return_address(0) == (void *)0x0040E003) {	/* WinMain's check */
		say("start-up check of the shared CTCL application type (%d) answered 0", o_GetApplType());
		return 0;
	}
	return o_GetApplType();
}
static int __cdecl c_GetGameIsServer(void) { return ctcl_is_server; }
static void __cdecl c_SetGameIsServer(int v) { ctcl_is_server = v; }
static int __cdecl c_GetGameState(void) { return ctcl_game_state; }
static void __cdecl c_SetGameState(int v) { if (v != ctcl_game_state) say("CTCL game state %d", v); ctcl_game_state = v; }
static int __cdecl c_GetGameTime(void) { return ctcl_game_time; }
static void __cdecl c_SetGameTime(int v) { ctcl_game_time = v; }

static void private_instance(void)
{
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	int i;
	void *dummy;

	if (g_launcher || !cfg_int("shim", "allow_multiple", 1)) return;
	for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
		if (!memcmp(sec[i].Name, ".SHARED_", 8) && sec[i].Misc.VirtualSize >= 8) {
			DWORD *running = (DWORD *)(base + sec[i].VirtualAddress + 4);
			say(".SHARED_ at %p: running flag %lu%s", (void *)running, *running,
			    *running ? " - another copy is up; cleared so this one can start" : "");
			*running = 0;
		}
	/* A pod's MW4 shares this state with its launcher, which is how the
	 * launcher watches it; only the console keeps its own. Either way the
	 * start-up check "is a CTCL program already registered?" is answered no,
	 * so a pod that was killed and restarted is not refused. */
	if (!cfg_int("shim", "private_ctcl", !strcmp(g_role, "ctcl1") || !strcmp(g_role, "console"))) {
		iat_hook("ctcls.dll", "?CTCL_GetApplType@@YAHXZ", c_StartupApplType, (void **)&o_GetApplType);
		return;
	}
	iat_hook("ctcls.dll", "?CTCL_GetApplState@@YAHXZ", c_GetApplState, &dummy);
	iat_hook("ctcls.dll", "?CTCL_SetApplState@@YAXH@Z", c_SetApplState, &dummy);
	iat_hook("ctcls.dll", "?CTCL_GetApplType@@YAHXZ", c_GetApplType, &dummy);
	iat_hook("ctcls.dll", "?CTCL_SetApplType@@YAXH@Z", c_SetApplType, &dummy);
	iat_hook("ctcls.dll", "?CTCL_GetGameIsServer@@YAHXZ", c_GetGameIsServer, &dummy);
	iat_hook("ctcls.dll", "?CTCL_SetGameIsServer@@YAXH@Z", c_SetGameIsServer, &dummy);
	iat_hook("ctcls.dll", "?CTCL_GetGameState@@YAHXZ", c_GetGameState, &dummy);
	iat_hook("ctcls.dll", "?CTCL_SetGameState@@YAXH@Z", c_SetGameState, &dummy);
	iat_hook("ctcls.dll", "?CTCL_GetGameTime@@YAHXZ", c_GetGameTime, &dummy);
	iat_hook("ctcls.dll", "?CTCL_SetGameTime@@YAXH@Z", c_SetGameTime, &dummy);
}

/* The game's listening sockets, logged: two copies on one PC cannot both
 * listen on one port. WSOCK32 is imported by ordinal; 2 is bind. */
typedef int (WINAPI *bind_t)(UINT_PTR, const void *, int);
static bind_t o_bind;

static int WINAPI i_bind(UINT_PTR so, const void *sa, int len)
{
	int r = o_bind(so, sa, len);
	const BYTE *b = (const BYTE *)sa;
	if (len >= 8)
		say("bind %u.%u.%u.%u:%u from %p -> %d%s", b[4], b[5], b[6], b[7], (b[2] << 8) | b[3],
		    __builtin_return_address(0), r, r ? " (failed: port in use?)" : "");
	return r;
}

/* The CTCL link between console and pods: who connects to whom. */
typedef int (WINAPI *WSAConnect_t)(UINT_PTR, const void *, int, void *, void *, void *, void *);
typedef UINT_PTR (WINAPI *WSAAccept_t)(UINT_PTR, void *, int *, void *, DWORD_PTR);
static WSAConnect_t o_WSAConnect;
static WSAAccept_t o_WSAAccept;
typedef int (WINAPI *lasterr_t)(void);

static int WINAPI i_WSAConnect(UINT_PTR so, const void *sa, int len, void *a, void *b, void *c, void *d)
{
	static int n;
	int r = o_WSAConnect(so, sa, len, a, b, c, d);
	const BYTE *p = sa;
	lasterr_t le = (lasterr_t)GetProcAddress(GetModuleHandleA("ws2_32.dll"), "WSAGetLastError");
	if (n++ < 40 && len >= 8)
		say("WSAConnect %u.%u.%u.%u:%u -> %d (error %d)", p[4], p[5], p[6], p[7], (p[2] << 8) | p[3],
		    r, r ? le() : 0);
	return r;
}

static UINT_PTR WINAPI i_WSAAccept(UINT_PTR so, void *sa, int *len, void *cond, DWORD_PTR data)
{
	UINT_PTR r = o_WSAAccept(so, sa, len, cond, data);
	if (r != (UINT_PTR)~0 && sa && len && *len >= 8) {
		const BYTE *p = sa;
		say("WSAAccept from %u.%u.%u.%u:%u", p[4], p[5], p[6], p[7], (p[2] << 8) | p[3]);
	}
	return r;
}

/* [shim] log_net = 1: what console and pods say to each other on the CTCL
 * ports (1000, 1001), a line per message - for learning the protocol. */
typedef int (WINAPI *recv_t)(UINT_PTR, char *, int, int);
typedef int (WINAPI *WSASend_t)(UINT_PTR, void *, DWORD, LPDWORD, DWORD, void *, void *);
static recv_t o_recv;
static WSASend_t o_WSASend;
typedef struct { ULONG len; char *buf; } wsabuf_t;

static int ctcl_port(UINT_PTR so)
{
	typedef int (WINAPI *name_t)(UINT_PTR, void *, int *);
	static name_t peer, self;
	BYTE sa[16];
	int n = sizeof sa, port;

	if (!peer) {
		HMODULE ws = GetModuleHandleA("ws2_32.dll");
		peer = (name_t)GetProcAddress(ws, "getpeername");
		self = (name_t)GetProcAddress(ws, "getsockname");
	}
	if (peer(so, sa, &n)) return 0;
	port = (sa[2] << 8) | sa[3];
	if (port == 1000 || port == 1001) return port;
	n = sizeof sa;
	if (self(so, sa, &n)) return 0;
	port = (sa[2] << 8) | sa[3];
	return port == 1000 || port == 1001 ? port : 0;
}

static void net_log(const char *dir, UINT_PTR so, const char *b, int n)
{
	static int lines, on = -1;
	char hex[3 * 96 + 1], txt[97];
	int i, k, port;

	if (on < 0) on = cfg_int("shim", "log_net", 0);
	if (!on || n <= 0 || lines > 4000 || !(port = ctcl_port(so))) return;
	lines++;
	say("net %s %u (%d bytes)", dir, port, n);
	for (k = 0; k < n && k < 4096; k += 32) {	/* the whole message, 32 bytes a line */
		for (i = 0; i < 32 && k + i < n; i++) {
			sprintf(hex + 3 * i, "%02X ", (BYTE)b[k + i]);
			txt[i] = b[k + i] >= 32 && b[k + i] < 127 ? b[k + i] : '.';
		}
		txt[i] = 0;
		say("net   %04X: %-96s| %s", k, hex, txt);
	}
}

static int WINAPI i_recv(UINT_PTR so, char *b, int n, int fl)
{
	int r = o_recv(so, b, n, fl);
	net_log("in ", so, b, r);
	return r;
}

/* Profiles. When the console loads a match it sends each pod its setup on
 * 1001: a message (2-byte big-endian length, then type 0x14) ending with the
 * pod's own slot,
 *
 *   <pilot> 00 | 00 00 <index, 2 bytes big-endian> ff ff 00 xx |
 *   <Mech, or "Mech Variant" - a Resource\Variants file's name> 00 |
 *   7 zero bytes | 03 <the pod's IPv4 address> ...
 *
 * - "Loki" is the stock Loki, "Loki B" its B variant. The index is what the
 * pod goes by: the Mech's place, from 1, in the console's list - every
 * chassis in alphabetical order, each followed by its Resource\Variants
 * files (Loki 208, Loki B 211, Zeus 357). With a pilot assigned to that pod
 * in profiles\assign.ini beside the game -
 *
 *   [192.168.1.132]
 *   name=Adam
 *   mech=Mad Cat Pryde
 *   mech_index=226
 *
 * written by the launcher when a pilot signs in at a pod - the console's shim
 * puts them in, whatever the operator's slot said. */
static int peer_ip(UINT_PTR so, BYTE ip[4])
{
	typedef int (WINAPI *name_t)(UINT_PTR, void *, int *);
	static name_t peer;
	BYTE sa[16];
	int n = sizeof sa;

	if (!peer) peer = (name_t)GetProcAddress(GetModuleHandleA("ws2_32.dll"), "getpeername");
	if (peer(so, sa, &n)) return 0;
	memcpy(ip, sa + 4, 4);
	return (sa[2] << 8) | sa[3];
}

/* The pod's slot in a setup message, rewritten into out; its length, or 0. */
static int assign_slot(const BYTE *m, int n, const BYTE ip[4], BYTE *out, int room)
{
	char ini[MAX_PATH], sec[20], name[64], mech[64];
	int idx;
	BYTE mark[5] = { 3, ip[0], ip[1], ip[2], ip[3] };
	int p, i, mech_end, mech_start, name_end, name_start, k, nl, ml, len;

	if (n < 40 || m[2] != 0x14) return 0;
	snprintf(ini, sizeof ini, "%sprofiles\\assign.ini", g_dir);
	snprintf(sec, sizeof sec, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
	GetPrivateProfileStringA(sec, "name", "", name, sizeof name, ini);
	GetPrivateProfileStringA(sec, "mech", "", mech, sizeof mech, ini);
	idx = (int)GetPrivateProfileIntA(sec, "mech_index", 0, ini);
	if (mech[0] && idx <= 0) mech[0] = 0;		/* the pod goes by the index: without one, no change */
	if (!name[0] && !mech[0]) return 0;
	for (p = n - 5; p > 20; p--)			/* the last record for this address */
		if (!memcmp(m + p, mark, 5)) break;
	if (p <= 20) { say("profiles: no slot for %s in the setup", sec); return 0; }
	mech_end = p - 8;			/* its 00, then seven more */
	for (i = mech_end; i < p; i++)
		if (m[i]) { say("profiles: the slot for %s is not as expected", sec); return 0; }
	for (mech_start = mech_end; mech_start > 0 && m[mech_start - 1]; mech_start--) ;
	name_end = mech_start - 9;
	if (name_end < 1 || m[name_end]) { say("profiles: the slot for %s is not as expected", sec); return 0; }
	for (name_start = name_end; name_start > 0 && m[name_start - 1]; name_start--) ;

	nl = name[0] ? (int)strlen(name) : name_end - name_start;
	ml = mech[0] ? (int)strlen(mech) : mech_end - mech_start;
	len = n - (name_end - name_start) - (mech_end - mech_start) + nl + ml;
	if (len > room) return 0;
	k = 0;
	memcpy(out + k, m, name_start); k += name_start;
	memcpy(out + k, name[0] ? (const BYTE *)name : m + name_start, nl); k += nl;
	memcpy(out + k, m + name_end, mech_start - name_end); k += mech_start - name_end;
	memcpy(out + k, mech[0] ? (const BYTE *)mech : m + mech_start, ml); k += ml;
	memcpy(out + k, m + mech_end, n - mech_end); k += n - mech_end;
	if (mech[0]) {
		int at = name_start + nl + 3;		/* in the bytes after the name */
		out[at] = (BYTE)(idx >> 8);
		out[at + 1] = (BYTE)idx;
	}
	out[0] = (BYTE)((len - 2) >> 8);
	out[1] = (BYTE)(len - 2);
	say("profiles: pod %s gets %.*s in a %.*s (the console said %.*s in a %.*s)", sec,
	    nl, (const char *)out + name_start, ml, (const char *)out + name_start + nl + (mech_start - name_end),
	    name_end - name_start, (const char *)m + name_start, mech_end - mech_start, (const char *)m + mech_start);
	return len;
}

static int WINAPI i_WSASend(UINT_PTR so, void *bufs, DWORD nb, LPDWORD sent, DWORD fl, void *ov, void *cr)
{
	static BYTE out[8192];
	wsabuf_t *b = bufs, mine;
	BYTE ip[4];
	DWORD i;
	int len, r;

	for (i = 0; i < nb; i++) net_log("out", so, b[i].buf, (int)b[i].len);
	/* The game sends overlapped; a message this size goes at once, and the
	 * game is told the length it gave, however long the rewrite. (The buffer
	 * is static: it must outlive a send that does not finish at once.) */
	if (nb == 1 && b[0].len > 2 && (BYTE)b[0].buf[2] == 0x14 && peer_ip(so, ip) == 1001 &&
	    (len = assign_slot((const BYTE *)b[0].buf, (int)b[0].len, ip, out, sizeof out)) > 0) {
		mine.len = (ULONG)len;
		mine.buf = (char *)out;
		r = o_WSASend(so, &mine, 1, sent, fl, ov, cr);
		if (!r) {
			if (sent) *sent = b[0].len;
			if (ov) ((OVERLAPPED *)ov)->InternalHigh = b[0].len;
		} else
			say("profiles: the rewritten setup did not go at once (%d)", r);
		return r;
	}
	return o_WSASend(so, bufs, nb, sent, fl, ov, cr);
}

/* The mouse. Full screen, the screen and the game's window are the same
 * rectangle, and GameOS uses screen coordinates as its own. Windowed they are
 * not, so the cursor is translated - and scaled, if the window has been
 * resized - between the screen and the main view's emulated mode. */
typedef BOOL (WINAPI *GetCursorPos_t)(LPPOINT);
typedef BOOL (WINAPI *SetCursorPos_t)(int, int);
typedef BOOL (WINAPI *ClipCursor_t)(const RECT *);
static GetCursorPos_t o_GetCursorPos;
static SetCursorPos_t o_SetCursorPos;
static ClipCursor_t o_ClipCursor;

/* A pod has no mouse, and a cockpit's game still steals it - it recentres
 * and confines the cursor every frame of a mission - so the MFD and radar
 * windows could never be clicked. A cockpit therefore leaves the mouse free
 * ([shim] capture_mouse = 0, the default for a cockpit; 1 elsewhere): the
 * game is told the cursor sits where it last put it. Scroll Lock toggles. */
static int g_mouse_free = -1;
static POINT g_mouse_at;
static int g_mouse_at_set;

static int mouse_free(void)
{
	if (g_mouse_free < 0) g_mouse_free = !cfg_int("shim", "capture_mouse", !g_cockpit);
	return g_mouse_free;
}

static void mouse_toggle(void)
{
	g_mouse_free = !mouse_free();
	if (g_mouse_free && o_ClipCursor) o_ClipCursor(NULL);
	say("mouse %s", g_mouse_free ? "free (the game sees it still)" : "captured by the game");
}

static struct dev *main_dev(void)
{
	int i;
	for (i = 0; i < MAX_DEVS; i++)
		if (g_dev[i].dd && g_dev[i].kind == K_MAIN && g_dev[i].emulated && g_dev[i].hwnd && g_dev[i].w)
			return &g_dev[i];
	return NULL;
}

static void client_box(struct dev *d, RECT *b)
{
	view_box(d->hwnd, d->w, d->h, b);
	if (b->right <= b->left || b->bottom <= b->top) SetRect(b, 0, 0, d->w, d->h);
}

static BOOL WINAPI i_GetCursorPos(LPPOINT p)
{
	struct dev *d = main_dev();
	BOOL r = o_GetCursorPos(p);
	RECT b;

	if (r && d && mouse_free()) {
		if (g_mouse_at_set) *p = g_mouse_at;
		else { p->x = d->w / 2; p->y = d->h / 2; }
		return r;
	}
	if (r && d) {
		ScreenToClient(d->hwnd, p);
		client_box(d, &b);
		p->x = (p->x - b.left) * (LONG)d->w / (b.right - b.left);
		p->y = (p->y - b.top) * (LONG)d->h / (b.bottom - b.top);
	}
	return r;
}

static BOOL WINAPI i_SetCursorPos(int x, int y)
{
	struct dev *d = main_dev();
	POINT p = { x, y };
	RECT b;

	if (d && mouse_free()) {
		g_mouse_at = p;
		g_mouse_at_set = 1;
		return TRUE;
	}
	if (d) {
		/* Only while the game has focus: it recentres the cursor every frame,
		 * which would pin the mouse while you work in another window. */
		if (GetForegroundWindow() != d->hwnd) return TRUE;
		client_box(d, &b);
		p.x = b.left + x * (b.right - b.left) / (LONG)d->w;
		p.y = b.top + y * (b.bottom - b.top) / (LONG)d->h;
		ClientToScreen(d->hwnd, &p);
	}
	return o_SetCursorPos(p.x, p.y);
}

static BOOL WINAPI i_ClipCursor(const RECT *r)
{
	struct dev *d = main_dev();
	RECT c, bx;
	POINT a, b;
	int cw, ch;

	if (!r || !d || mouse_free()) return o_ClipCursor(NULL);
	if (GetForegroundWindow() != d->hwnd) return o_ClipCursor(NULL);
	client_box(d, &bx);
	cw = bx.right - bx.left; ch = bx.bottom - bx.top;
	a.x = bx.left + r->left * cw / (LONG)d->w;  a.y = bx.top + r->top * ch / (LONG)d->h;
	b.x = bx.left + r->right * cw / (LONG)d->w; b.y = bx.top + r->bottom * ch / (LONG)d->h;
	ClientToScreen(d->hwnd, &a);
	ClientToScreen(d->hwnd, &b);
	SetRect(&c, a.x, a.y, b.x, b.y);
	return o_ClipCursor(&c);
}

typedef HWND (WINAPI *GetForegroundWindow_t)(void);
static GetForegroundWindow_t o_GetForegroundWindow;
static HWND WINAPI i_GetForegroundWindow(void)
{
	return g_main_hwnd ? g_main_hwnd : o_GetForegroundWindow();
}

/* mw4print prints each battle report to the default printer without asking
 * - at a site, the paper printer by the console. A printer that saves to a
 * file instead (Microsoft Print to PDF, the XPS writer) asks where, every
 * job, and while it asks mw4print's timer starts the job again, and again.
 * So for those each job is given its file: [print] folder (default
 * "reports" beside the game), named by the time. [print] to_file = 1 always
 * does this, 0 never; by default it is done for a printer named *PDF* or
 * *XPS*. */
typedef int (WINAPI *StartDocA_t)(HDC, const DOCINFOA *);
static StartDocA_t o_StartDocA;

static int WINAPI i_StartDocA(HDC dc, const DOCINFOA *di)
{
	BOOL (WINAPI *getdef)(LPSTR, LPDWORD) = NULL;
	HMODULE ws = LoadLibraryA("winspool.drv");
	char printer[256] = "", folder[MAX_PATH], path[MAX_PATH];
	DWORD n = sizeof printer;
	int to_file = cfg_int("print", "to_file", -1), pdf;
	DOCINFOA mine;
	SYSTEMTIME t;

	if (ws) getdef = (BOOL (WINAPI *)(LPSTR, LPDWORD))GetProcAddress(ws, "GetDefaultPrinterA");
	if (getdef) getdef(printer, &n);
	pdf = strstr(printer, "PDF") != NULL;
	if (to_file < 0) to_file = pdf || strstr(printer, "XPS");
	if (!to_file || !di || di->lpszOutput) {
		say("print: \"%s\" to %s", di && di->lpszDocName ? di->lpszDocName : "", printer);
		return o_StartDocA(dc, di);
	}
	cfg_str("print", "folder", "reports", folder, sizeof folder);
	if (!strchr(folder, ':')) {			/* relative: beside the game */
		char rel[MAX_PATH];
		snprintf(rel, sizeof rel, "%s%s", g_dir, folder);
		snprintf(folder, sizeof folder, "%s", rel);
	}
	CreateDirectoryA(folder, NULL);
	GetLocalTime(&t);
	snprintf(path, sizeof path, "%s\\%04d-%02d-%02d %02d-%02d-%02d battle report.%s", folder,
		 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, pdf ? "pdf" : "xps");
	mine = *di;
	mine.lpszOutput = path;
	say("print: \"%s\" to %s, as %s", di->lpszDocName ? di->lpszDocName : "", printer, path);
	return o_StartDocA(dc, &mine);
}

static void redirect_ctcl(void)
{
	if (g_launcher) iat_hook("GDI32.dll", "StartDocA", i_StartDocA, (void **)&o_StartDocA);
	iat_hook("USER32.dll", "GetForegroundWindow", i_GetForegroundWindow, (void **)&o_GetForegroundWindow);
	iat_hook("USER32.dll", "GetCursorPos", i_GetCursorPos, (void **)&o_GetCursorPos);
	iat_hook("USER32.dll", "SetCursorPos", i_SetCursorPos, (void **)&o_SetCursorPos);
	iat_hook("USER32.dll", "ClipCursor", i_ClipCursor, (void **)&o_ClipCursor);
	iat_hook("WS2_32.dll", "WSAConnect", i_WSAConnect, (void **)&o_WSAConnect);
	iat_hook("WS2_32.dll", "WSAAccept", i_WSAAccept, (void **)&o_WSAAccept);
	iat_hook("WS2_32.dll", "WSASend", i_WSASend, (void **)&o_WSASend);
	iat_hook("WSOCK32.dll", "#16", i_recv, (void **)&o_recv);
	iat_hook("WSOCK32.dll", "#2", i_bind, (void **)&o_bind);
	private_instance();
	iat_hook("USER32.dll", "FindWindowA", i_FindWindowA, (void **)&o_FindWindowA);
	iat_hook("KERNEL32.dll", "CreateFileMappingA", i_CreateFileMappingA, (void **)&o_CreateFileMappingA);
	iat_hook("KERNEL32.dll", "ExitProcess", i_ExitProcess, (void **)&o_ExitProcess);
	iat_hook("MSVCRT.dll", "exit", i_exit, (void **)&o_exit);
	iat_hook("MSVCRT.dll", "_exit", i__exit, (void **)&o__exit);
	snprintf(g_ctcl, sizeof g_ctcl, "%sctcl.ini", g_dir);
	iat_hook("KERNEL32.dll", "GetPrivateProfileStringA", i_GPPS, (void **)&o_GPPS);
	iat_hook("KERNEL32.dll", "GetPrivateProfileIntA", i_GPPI, (void **)&o_GPPI);
	iat_hook("KERNEL32.dll", "WritePrivateProfileStringA", i_WPPS, (void **)&o_WPPS);
	iat_hook("KERNEL32.dll", "GetFileAttributesA", i_GFA, (void **)&o_GFA);
	iat_hook("KERNEL32.dll", "CreateFileA", i_CFA, (void **)&o_CFA);
	iat_hook("MSVCRT.dll", "fopen", i_fopen, (void **)&o_fopen);
	iat_hook("MSVCRT.dll", "_open", i_open, (void **)&o_open);
	iat_hook("KERNEL32.dll", "DeleteFileA", i_DeleteFileA, (void **)&o_DeleteFileA);
	iat_hook("KERNEL32.dll", "SetFileAttributesA", i_SetFileAttributesA, (void **)&o_SetFileAttributesA);
	iat_hook("KERNEL32.dll", "MoveFileA", i_MoveFileA, (void **)&o_MoveFileA);
	iat_hook("KERNEL32.dll", "CreateDirectoryA", i_CreateDirectoryA, (void **)&o_CreateDirectoryA);
	iat_hook("KERNEL32.dll", "FindFirstFileA", i_FindFirstFileA, (void **)&o_FindFirstFileA);
	iat_hook("KERNEL32.dll", "CopyFileA", i_CopyFileA, (void **)&o_CopyFileA);
	iat_hook("MSVCRT.dll", "_access", i_access, (void **)&o_access);
	iat_hook("MSVCRT.dll", "remove", i_remove, (void **)&o_remove);
	iat_hook("KERNEL32.dll", "GetFullPathNameA", i_GetFullPathNameA, (void **)&o_GetFullPathNameA);
	iat_hook("KERNEL32.dll", "RemoveDirectoryA", i_RemoveDirectoryA, (void **)&o_RemoveDirectoryA);
	/* One folder per role: on a site every machine had its own c:\mw4files, and a
	 * console and a pod sharing one write the same report file and trip over
	 * each other's temporary copy. */
	snprintf(g_files, sizeof g_files, "%smw4files", g_dir);
	CreateDirectoryA(g_files, NULL);
	snprintf(g_files, sizeof g_files, "%smw4files\\%s", g_dir, g_role);
	CreateDirectoryA(g_files, NULL);
	say("c:\\mw4files is %s", g_files);
	say("c:\\ctcl.ini is read from %s%s", g_ctcl,
	    GetFileAttributesA(g_ctcl) == INVALID_FILE_ATTRIBUTES ? " - which does not exist" : "");
}

/* ------------------------------------------------ the keyboard, as the game reads it */

/* MW4 reads the keyboard through DirectInput, which it finds with
 * GetProcAddress. The shim hands it a DirectInputCreateEx of its own and
 * watches the keyboard device it makes, which lets it
 *   - hide keys the cockpit owns (the RIO keys and the panel F-keys) from the
 *     game, so they never fire a game binding as well, and
 *   - type into the game from the control port ("key", "type"), for a test
 *     harness: Windows' synthetic input does not reach GameOS's text fields.
 * Slots: IDirectInput7 CreateDevice 3, CreateDeviceEx 9; IDirectInputDevice7
 * GetDeviceState 9, GetDeviceData 10. */

typedef struct { DWORD ofs, data, time, seq; } di_event;	/* DIDEVICEOBJECTDATA */
static const GUID GUID_SysKeyboard_ = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

static struct vtpatch g_divt[MAX_VTBLS], g_kbvt[MAX_VTBLS];
static int g_ndivt, g_nkbvt;
static void *g_kbdev[8];
static int g_nkbdev;
static uint8_t g_kb_mask[256];			/* scan codes the game must not see */
static uint8_t g_kb_held[256];			/* scan codes we hold down */
static CRITICAL_SECTION g_kb_lock;
static di_event g_kb_q[256];			/* key events we are typing */
static int g_kb_qn;
static DWORD g_kb_seq = 0x40000000;
static int g_kb_logged;

static int is_kbdev(void *d)
{
	int i;
	for (i = 0; i < g_nkbdev; i++) if (g_kbdev[i] == d) return 1;
	return 0;
}

/* Queue a key event for the game, and keep the immediate state in step. */
static void kb_event(int sc, int down)
{
	EnterCriticalSection(&g_kb_lock);
	if (g_kb_qn < 256) {
		g_kb_q[g_kb_qn].ofs = sc;
		g_kb_q[g_kb_qn].data = down ? 0x80 : 0;
		g_kb_q[g_kb_qn].time = GetTickCount();
		g_kb_q[g_kb_qn].seq = g_kb_seq++;
		g_kb_qn++;
	}
	g_kb_held[sc] = down ? 0x80 : 0;
	LeaveCriticalSection(&g_kb_lock);
}

typedef HRESULT (WINAPI *GetDeviceState_t)(void *, DWORD, LPVOID);
typedef HRESULT (WINAPI *GetDeviceData_t)(void *, DWORD, di_event *, LPDWORD, DWORD);

static HRESULT WINAPI h_GetDeviceState(void *dev, DWORD n, LPVOID out)
{
	HRESULT hr = ((GetDeviceState_t)orig_in(g_kbvt, g_nkbvt, dev, 9))(dev, n, out);
	if (SUCCEEDED(hr) && is_kbdev(dev) && n >= 256) {
		uint8_t *k = out;
		int i;
		EnterCriticalSection(&g_kb_lock);
		for (i = 0; i < 256; i++) {
			if (g_kb_mask[i]) k[i] = 0;
			k[i] |= g_kb_held[i];
		}
		LeaveCriticalSection(&g_kb_lock);
		if (g_kb_logged++ < 3) say("keyboard: the game polls its state (GetDeviceState)");
	}
	return hr;
}

static HRESULT WINAPI h_GetDeviceData(void *dev, DWORD cb, di_event *ev, LPDWORD inout, DWORD flags)
{
	DWORD room = inout ? *inout : 0;
	HRESULT hr = ((GetDeviceData_t)orig_in(g_kbvt, g_nkbvt, dev, 10))(dev, cb, ev, inout, flags);

	if (FAILED(hr) || !is_kbdev(dev) || !inout || cb < sizeof(di_event)) return hr;
	if (g_kb_logged++ < 3) say("keyboard: the game reads buffered events (GetDeviceData, %lu bytes each)", cb);
	if (ev && !(flags & 1 /* DIGDD_PEEK */)) {
		DWORD i, kept = 0;
		/* drop the cockpit's keys */
		for (i = 0; i < *inout; i++) {
			di_event *e = (di_event *)((uint8_t *)ev + i * cb);
			if (g_kb_mask[e->ofs & 0xFF]) continue;
			if (kept != i) memmove((uint8_t *)ev + kept * cb, e, cb);
			kept++;
		}
		/* add ours */
		EnterCriticalSection(&g_kb_lock);
		for (i = 0; i < (DWORD)g_kb_qn && kept < room; i++) {
			di_event *e = (di_event *)((uint8_t *)ev + kept * cb);
			memset(e, 0, cb);
			*e = g_kb_q[i];
			kept++;
		}
		if (i) { memmove(g_kb_q, g_kb_q + i, (g_kb_qn - i) * sizeof *g_kb_q); g_kb_qn -= i; }
		LeaveCriticalSection(&g_kb_lock);
		*inout = kept;
	}
	return hr;
}

static void hook_kbdev(void *dev)
{
	static const int slots[] = { 9, 10 };
	static void *const fns[] = { h_GetDeviceState, h_GetDeviceData };
	if (g_nkbdev < 8) g_kbdev[g_nkbdev++] = dev;
	hook_vtable(g_kbvt, &g_nkbvt, dev, 2, slots, fns, "IDirectInputDevice7 (keyboard)");
	say("keyboard device %p", dev);
}

typedef HRESULT (WINAPI *DICreateDevice_t)(void *, REFGUID, void **, IUnknown *);
typedef HRESULT (WINAPI *DICreateDeviceEx_t)(void *, REFGUID, REFIID, void **, IUnknown *);

static HRESULT WINAPI h_DICreateDevice(void *di, REFGUID g, void **out, IUnknown *u)
{
	HRESULT hr = ((DICreateDevice_t)orig_in(g_divt, g_ndivt, di, 3))(di, g, out, u);
	if (SUCCEEDED(hr) && IsEqualGUID(g, &GUID_SysKeyboard_)) hook_kbdev(*out);
	return hr;
}

static HRESULT WINAPI h_DICreateDeviceEx(void *di, REFGUID g, REFIID iid, void **out, IUnknown *u)
{
	HRESULT hr = ((DICreateDeviceEx_t)orig_in(g_divt, g_ndivt, di, 9))(di, g, iid, out, u);
	if (SUCCEEDED(hr) && IsEqualGUID(g, &GUID_SysKeyboard_)) hook_kbdev(*out);
	return hr;
}

typedef HRESULT (WINAPI *DICreateEx_t)(HINSTANCE, DWORD, REFIID, void **, IUnknown *);
typedef HRESULT (WINAPI *DICreateA_t)(HINSTANCE, DWORD, void **, IUnknown *);
static DICreateEx_t r_DirectInputCreateEx;
static DICreateA_t r_DirectInputCreateA;

static void hook_di(void *di, int has_ex)
{
	static const int slots[] = { 3, 9 };
	static void *const fns[] = { h_DICreateDevice, h_DICreateDeviceEx };
	hook_vtable(g_divt, &g_ndivt, di, has_ex ? 2 : 1, slots, fns, "IDirectInput");
}

static HRESULT WINAPI my_DirectInputCreateEx(HINSTANCE h, DWORD ver, REFIID iid, void **out, IUnknown *u)
{
	HRESULT hr = r_DirectInputCreateEx(h, ver, iid, out, u);
	say("DirectInputCreateEx version %lx -> %08lX", ver, (unsigned long)hr);
	if (SUCCEEDED(hr)) hook_di(*out, 1);
	return hr;
}

static HRESULT WINAPI my_DirectInputCreateA(HINSTANCE h, DWORD ver, void **out, IUnknown *u)
{
	HRESULT hr = r_DirectInputCreateA(h, ver, out, u);
	say("DirectInputCreateA version %lx -> %08lX", ver, (unsigned long)hr);
	if (SUCCEEDED(hr)) hook_di(*out, 0);
	return hr;
}

typedef FARPROC (WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
static GetProcAddress_t o_GetProcAddress;

static FARPROC WINAPI i_GetProcAddress(HMODULE m, LPCSTR name)
{
	FARPROC f = o_GetProcAddress(m, name);
	if (!f || (ULONG_PTR)name < 0x10000) return f;
	if (!strcmp(name, "DirectInputCreateEx")) { r_DirectInputCreateEx = (DICreateEx_t)f; return (FARPROC)my_DirectInputCreateEx; }
	if (!strcmp(name, "DirectInputCreateA")) { r_DirectInputCreateA = (DICreateA_t)f; return (FARPROC)my_DirectInputCreateA; }
	return f;
}

/* Scan codes for typing; enough for names and numbers. */
static int scan_of(int ch, int *shift)
{
	static const char *row1 = "1234567890", *qrow = "qwertyuiop", *arow = "asdfghjkl", *zrow = "zxcvbnm";
	const char *p;
	*shift = isupper(ch) || ch == '_';
	ch = tolower(ch);
	if ((p = strchr(row1, ch)) && ch) return 0x02 + (int)(p - row1);
	if ((p = strchr(qrow, ch)) && ch) return 0x10 + (int)(p - qrow);
	if ((p = strchr(arow, ch)) && ch) return 0x1E + (int)(p - arow);
	if ((p = strchr(zrow, ch)) && ch) return 0x2C + (int)(p - zrow);
	if (ch == ' ') return 0x39;
	if (ch == '-' || ch == '_') return 0x0C;
	if (ch == '.') return 0x34;
	return 0;
}

static void kb_type(const char *t)
{
	for (; *t; t++) {
		int sh, sc = scan_of((unsigned char)*t, &sh);
		if (!sc) continue;
		if (sh) kb_event(0x2A, 1);
		kb_event(sc, 1);
		Sleep(40);
		kb_event(sc, 0);
		if (sh) kb_event(0x2A, 0);
		Sleep(40);
	}
}

/* ---- what each key and pad button does
 *
 * Every cockpit action has keys ([keys] <action> = up to four, by name: a
 * letter or digit, F1-F24, Space, Enter, Tab, Backslash, Shift, Ctrl, Alt,
 * Up, Down, Left, Right, Num0-Num9, PageUp, PageDown, Home, End, Insert,
 * Delete, ScrollLock, or 0xNN for any virtual-key code; "none" for none) and
 * a pad button ([pad] <action> = A, B, X, Y, LB, RB, LT, RT, Back, Start, LS,
 * RS, DUp, DDown, DLeft, DRight, or none). The MFD soft keys (F1-F8, the
 * selected MFD) and the radar's (Ctrl/Shift + F1-F6) are fixed: they are the
 * panel. */
enum {
	A_THROTTLE_UP, A_THROTTLE_DOWN, A_THROTTLE_FULL, A_THROTTLE_ZERO,
	A_PEDAL_LEFT, A_PEDAL_RIGHT, A_TWIST_LEFT, A_TWIST_RIGHT, A_AIM_UP, A_AIM_DOWN,
	A_FIRE1, A_FIRE2, A_FIRE3, A_FIRE_ALL, A_THROTTLE_BUTTON, A_PANIC,
	A_MFD_PREV, A_MFD_NEXT, A_MOUSE, A_COUNT
};
static struct bind {
	const char *name, *keys, *pad;
	int vk[4];
	WORD pad_bits;
	int trigger;			/* 1 left, 2 right */
} g_bind[A_COUNT] = {
	{ "throttle_up", "R", "none" },		{ "throttle_down", "F", "none" },
	{ "throttle_full", "none", "DUp" },	{ "throttle_zero", "none", "DDown" },
	{ "pedal_left", "A", "none" },		{ "pedal_right", "D", "none" },
	{ "twist_left", "J", "none" },		{ "twist_right", "L", "none" },
	{ "aim_up", "I", "none" },		{ "aim_down", "K", "none" },
	{ "fire1", "Space 1", "RT" },		{ "fire2", "2", "LT" },
	{ "fire3", "3", "RB" },			{ "fire_all", "4 Backslash", "A" },
	{ "throttle_button", "F11", "LB" },	{ "panic", "F12", "none" },
	{ "mfd_prev", "F9", "DLeft" },		{ "mfd_next", "F10", "DRight" },
	{ "mouse", "ScrollLock", "none" },
};

static int vk_named(const char *s)
{
	static const struct { const char *n; int vk; } names[] = {
		{ "Space", VK_SPACE }, { "Enter", VK_RETURN }, { "Tab", VK_TAB }, { "Backslash", VK_OEM_5 },
		{ "Shift", VK_SHIFT }, { "Ctrl", VK_CONTROL }, { "Alt", VK_MENU }, { "Up", VK_UP },
		{ "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT }, { "PageUp", VK_PRIOR },
		{ "PageDown", VK_NEXT }, { "Home", VK_HOME }, { "End", VK_END }, { "Insert", VK_INSERT },
		{ "Delete", VK_DELETE }, { "ScrollLock", VK_SCROLL }, { "Backspace", VK_BACK },
		{ "Minus", VK_OEM_MINUS }, { "Equals", VK_OEM_PLUS }, { "Comma", VK_OEM_COMMA },
		{ "Period", VK_OEM_PERIOD }, { "Slash", VK_OEM_2 }, { "Semicolon", VK_OEM_1 },
		{ "Quote", VK_OEM_7 }, { "LBracket", VK_OEM_4 }, { "RBracket", VK_OEM_6 },
	};
	size_t i;

	if (s[0] && !s[1] && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
	if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1]) && atoi(s + 1) >= 1 && atoi(s + 1) <= 24)
		return VK_F1 + atoi(s + 1) - 1;
	if (!_strnicmp(s, "Num", 3) && isdigit((unsigned char)s[3]) && !s[4]) return VK_NUMPAD0 + s[3] - '0';
	if (!_strnicmp(s, "0x", 2)) return (int)strtol(s, NULL, 16);
	for (i = 0; i < sizeof names / sizeof *names; i++)
		if (!_stricmp(s, names[i].n)) return names[i].vk;
	return 0;
}

static void pad_named(struct bind *b, const char *s)
{
	static const struct { const char *n; WORD bit; } names[] = {
		{ "DUp", 0x1 }, { "DDown", 0x2 }, { "DLeft", 0x4 }, { "DRight", 0x8 }, { "Start", 0x10 },
		{ "Back", 0x20 }, { "LS", 0x40 }, { "RS", 0x80 }, { "LB", 0x100 }, { "RB", 0x200 },
		{ "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 },
	};
	size_t i;

	if (!_stricmp(s, "LT")) { b->trigger = 1; return; }
	if (!_stricmp(s, "RT")) { b->trigger = 2; return; }
	for (i = 0; i < sizeof names / sizeof *names; i++)
		if (!_stricmp(s, names[i].n)) { b->pad_bits |= names[i].bit; return; }
	if (_stricmp(s, "none")) say("[pad] %s: no button \"%s\"", b->name, s);
}

/* Read the bindings, and hide every bound key from the game. */
static void binds_load(void)
{
	int a;

	for (a = 0; a < A_COUNT; a++) {
		struct bind *b = &g_bind[a];
		char buf[128], *t;
		int n = 0;

		cfg_str("keys", b->name, b->keys, buf, sizeof buf);
		for (t = strtok(buf, " ,+"); t; t = strtok(NULL, " ,+")) {
			int vk = vk_named(t);
			if (!vk) { if (_stricmp(t, "none")) say("[keys] %s: no key \"%s\"", b->name, t); continue; }
			if (n < 4) b->vk[n++] = vk;
		}
		cfg_str("pad", b->name, b->pad, buf, sizeof buf);
		for (t = strtok(buf, " ,+"); t; t = strtok(NULL, " ,+")) pad_named(b, t);
	}
}

static void keyboard_hooks(void)
{
	/* The cockpit's keys - every bound key and the panel's F1-F12 - are
	 * hidden from the game; [keys] mask = 0 lets it see them as well. */
	int a, i;

	InitializeCriticalSection(&g_kb_lock);
	binds_load();
	if (g_cockpit && cfg_int("keys", "mask", 1)) {
		for (i = 0; i < 12; i++) g_kb_mask[MapVirtualKeyA(VK_F1 + i, 0) & 0xFF] = 1;
		for (a = 0; a < A_COUNT; a++)
			for (i = 0; i < 4 && g_bind[a].vk[i]; i++) {
				UINT sc = MapVirtualKeyA(g_bind[a].vk[i], 0);
				if (sc) g_kb_mask[sc & 0xFF] = 1;
			}
	}
	iat_hook("KERNEL32.dll", "GetProcAddress", i_GetProcAddress, (void **)&o_GetProcAddress);
}

/* ------------------------------------------------ the RIO board */

/* A Tesla pod's controls - stick, throttle, pedals, and every button on the
 * MFDs, the radar and the stick - reach the game from VWE's Remote I/O board
 * on a serial port, read by MW4.exe's driver at 0x566EB0. This is that board.
 *
 * When the game opens COMn, it gets a handle of ours, and its serial calls on
 * that handle are answered here. The protocol, read from the driver:
 *
 *   a message is an opcode byte with the top bit set, then that opcode's
 *   payload in 7-bit bytes, then a checksum: the sum of every byte before it
 *   with the top bit masked, masked to 7 bits. Payload lengths, opcodes
 *   0x80-0x8C: 0 0 0 1 2 2 2 10 1 1 2 2 1 (the default, -trio 0; table
 *   0x76B630). -trio 1 selects a second table, 0x80-0x8E, for another board.
 *
 *   board to game:  0x87 + 10   five analog values, two bytes each, low seven
 *                               bits first, 14-bit two's complement: throttle,
 *                               left pedal, right pedal, stick Y, stick X.
 *                               The game resets the board if none arrives in
 *                               2.8 s, so it is also the heartbeat.
 *                   0x88 id     button id pressed; 0x89 released. 0x40, 0x46
 *                               and 0x47 are the stick's buttons; every other
 *                               id is posted to the game as message 0x464.
 *                   0xFC        acknowledge the game's last message; 0xFD
 *                               refuses it, and a refused lamp is sent again.
 *                               The game sends one message at a time.
 *
 *   game to board:  82          poll, whenever 50 ms pass with nothing heard
 *                   83 00       sent once, after the board is reset by DTR
 *                   84 ...      lamps (not yet seen)
 *
 * The game self-calibrates: the throttle counts down from the highest value
 * it has seen, the pedals up from the lowest, so everything starts at rest. */

#define RIO_Q 4096
static HANDLE g_rio_h;			/* the port the game holds: an event of ours */
static CRITICAL_SECTION g_rio_lock;
static uint8_t g_rio_q[RIO_Q];		/* bytes on their way to the game */
static int g_rio_head, g_rio_tail;
static LPOVERLAPPED g_rio_wait_ov;	/* a WaitCommEvent waiting for data */
static LPDWORD g_rio_wait_mask;
static int g_rio_logged;

static int rio_count(void) { return (g_rio_tail - g_rio_head + RIO_Q) % RIO_Q; }

static void rio_push(const uint8_t *b, int n)
{
	int i;

	EnterCriticalSection(&g_rio_lock);
	for (i = 0; i < n; i++) {
		int nt = (g_rio_tail + 1) % RIO_Q;
		if (nt == g_rio_head) break;			/* the game is not reading */
		g_rio_q[g_rio_tail] = b[i];
		g_rio_tail = nt;
	}
	if (g_rio_wait_ov && rio_count()) {
		if (g_rio_wait_mask) *g_rio_wait_mask = EV_RXCHAR;
		g_rio_wait_ov->Internal = 0;
		g_rio_wait_ov->InternalHigh = 0;
		SetEvent(g_rio_wait_ov->hEvent);
		g_rio_wait_ov = NULL;
		g_rio_wait_mask = NULL;
	}
	LeaveCriticalSection(&g_rio_lock);
}

static void rio_packet(uint8_t op, const uint8_t *d, int n)
{
	uint8_t b[16];
	int i, sum = op & 0x7F;

	b[0] = op;
	for (i = 0; i < n; i++) {
		b[1 + i] = d[i] & 0x7F;
		sum += b[1 + i];
	}
	b[1 + n] = (uint8_t)(sum & 0x7F);
	rio_push(b, n + 2);
}

static void rio_button(int id, int down)
{
	uint8_t d = (uint8_t)id;

	if (!g_rio_h) return;
	rio_packet(down ? 0x88 : 0x89, &d, 1);
	say("RIO button %02X %s", id, down ? "down" : "up");
}

static void rio_tell(const char *fmt, ...);

/* Every lamp the game has set, so a bridge that says hello late - after the
 * mission's start lit the panel - is told the panel as it is. */
static int g_lamp[128];			/* level + 1; 0: never set */

/* What the game sent the board, logged until we know what it all means. */
static void rio_from_host(const uint8_t *b, DWORD n)
{
	if (n == 1 && b[0] >= 0xFC) return;		/* its own ack/nak of ours */
	if (n >= 3 && b[0] == 0x84) g_lamp[b[1] & 0x7F] = b[2] + 1;
	if (!(n == 2 && b[0] == 0x82) && g_rio_logged++ < 5000) {
		char line[200];
		DWORD i;
		int k = 0;
		for (i = 0; i < n && k < (int)sizeof line - 4; i++) k += sprintf(line + k, " %02X", b[i]);
		say("RIO from game:%s", line);
		rio_tell("game%s", line);
	}
	if (b[0] >= 0x80) {
		uint8_t ack = 0xFC;
		rio_push(&ack, 1);
	}
}

/* ---- input: the keyboard, until there is a cockpit */

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static int our_window_has_focus(void)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
}

/* ---- the control port
 *
 * A local UDP port, one text command per datagram, for anything outside the
 * game that wants to be the cockpit: a test script today, the hardware
 * bridge tomorrow. [rio] control_port, default 9100; 0 turns it off.
 *
 *   press XX / release XX / tap XX   button XX (hex), as the board reports it
 *   axes T LP RP SY SX               hold the five analog values (counts, as
 *                                    in the 0x87 message); "axes off" returns
 *                                    them to the keyboard
 *   snap                             snapshots of every display
 *   key XX / keydown XX / keyup XX   a key, by DirectInput scan code (hex)
 *   type TEXT                        type into the game
 *   hello                            send me what the game tells the board,
 *                                    starting with every lamp as it stands
 *
 * Whoever spoke last is sent "game <hex bytes>" for every message the game
 * sends the board other than its polls - the lamps among them. */
static SOCKET g_ctl_sock = INVALID_SOCKET;
static struct sockaddr_in g_ctl_peer;
static int g_ctl_have_peer;
static int g_ctl_axes[5], g_ctl_axes_on;

static void rio_tell(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (g_ctl_sock == INVALID_SOCKET || !g_ctl_have_peer) return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	sendto(g_ctl_sock, buf, (int)strlen(buf), 0, (struct sockaddr *)&g_ctl_peer, sizeof g_ctl_peer);
}

static void ctl_open(void)
{
	WSADATA w;
	struct sockaddr_in a;
	u_long on = 1;
	/* 9100 for a cockpit, 9101 for anything else (the console). */
	int port = cfg_int("rio", "control_port", g_cockpit ? 9100 : 9101);

	if (!port) return;
	WSAStartup(MAKEWORD(2, 2), &w);
	g_ctl_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_ctl_sock == INVALID_SOCKET) return;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((u_short)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(g_ctl_sock, (struct sockaddr *)&a, sizeof a)) {
		say("RIO: control port %d is taken; no control port", port);
		closesocket(g_ctl_sock);
		g_ctl_sock = INVALID_SOCKET;
		return;
	}
	ioctlsocket(g_ctl_sock, FIONBIO, &on);
	say("RIO: control port udp://127.0.0.1:%d", port);
}

static void ctl_poll(void)
{
	char buf[256];
	struct sockaddr_in from;
	int fl = sizeof from, n;

	if (g_ctl_sock == INVALID_SOCKET) return;
	while ((n = recvfrom(g_ctl_sock, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl)) > 0) {
		unsigned id;
		int v[5];

		buf[n] = 0;
		g_ctl_peer = from;
		g_ctl_have_peer = 1;
		if (sscanf(buf, "press %x", &id) == 1) rio_button(id, 1);
		else if (sscanf(buf, "release %x", &id) == 1) rio_button(id, 0);
		else if (sscanf(buf, "tap %x", &id) == 1) { rio_button(id, 1); Sleep(80); rio_button(id, 0); }
		else if (sscanf(buf, "axes %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
			memcpy(g_ctl_axes, v, sizeof v);
			g_ctl_axes_on = 1;
		} else if (!strncmp(buf, "axes off", 8)) g_ctl_axes_on = 0;
		else if (!strncmp(buf, "snap", 4)) g_snapreq = GetTickCount();
		else if (sscanf(buf, "keydown %x", &id) == 1) kb_event(id & 0xFF, 1);
		else if (sscanf(buf, "keyup %x", &id) == 1) kb_event(id & 0xFF, 0);
		else if (sscanf(buf, "key %x", &id) == 1) { kb_event(id & 0xFF, 1); Sleep(60); kb_event(id & 0xFF, 0); }
		else if (sscanf(buf, "peek %x", &id) == 1) {
			if (IsBadReadPtr((void *)(uintptr_t)id, 4)) rio_tell("peek %08X bad", id);
			else rio_tell("peek %08X %08lX", id, (unsigned long)*(volatile DWORD *)(uintptr_t)id);
		}
		else if (!strncmp(buf, "type ", 5)) kb_type(buf + 5);
		else if (!strncmp(buf, "hello", 5)) {
			int i;
			rio_tell("rio ready");
			for (i = 0; i < 128; i++)		/* the lamps, as they stand */
				if (g_lamp[i])
					rio_tell("game 84 %02X %02X %02X", i, g_lamp[i] - 1,
						 (0x04 + i + g_lamp[i] - 1) & 0x7F);
		}
		else rio_tell("unknown: %s", buf);
		fl = sizeof from;
	}
}

/* ---- the panel keys
 *
 * The keyboard stands in for the panel. F1-F4 are the selected MFD's top
 * soft keys left to right, F5-F8 its bottom ones; F9 and F10 select the
 * previous and next MFD (its window title says so); Ctrl+F1-F6 are the
 * radar's top row, Shift+F1-F6 its bottom row. F9-F12 and Scroll Lock are
 * bindings (mfd_prev, mfd_next, throttle_button, panic, mouse). */
static int g_panel_mfd;
static int mfd_button_index(int tile, int top, int col);
static void mfd_titles(void);

static void mouse_toggle(void);
static int radar_button_index(int top, int col);

static void panel_keys(int focus)
{
	static int held[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	int k, ctrl = key(VK_CONTROL), shift = key(VK_SHIFT);

	for (k = 0; k < 8; k++) {
		int down = focus && key(VK_F1 + k);
		if (down && held[k] < 0) {
			int id = ctrl || shift ? (k < 6 ? radar_button_index(ctrl, k) : -1)
				 : mfd_button_index(g_panel_mfd, k < 4, k % 4);
			if (id >= 0) rio_button(id, 1);
			held[k] = id >= 0 ? id : 0x100;
		} else if (!down && held[k] >= 0) {
			if (held[k] < 0x100) rio_button(held[k], 0);
			held[k] = -1;
		}
	}
}

static DWORD WINAPI ctl_main(LPVOID p)
{
	ctl_open();
	for (;;) {
		Sleep(20);
		ctl_poll();
	}
	return 0;
}

/* ---- a gamepad
 *
 * Any XInput pad (an Xbox controller, or anything Steam Input dresses as
 * one) stands in for the stick, throttle and pedals: the left stick's X is
 * the pedals and its Y moves the throttle lever (push to open it, pull to
 * close it); the right stick is the stick. Its buttons are bindings (above):
 * by default the triggers and RB fire groups 1-3, A all of them, LB is the
 * throttle button, the D-pad opens and closes the throttle all the way and
 * selects the MFD the F-keys work. [pad]
 * enabled = 0 turns it off, index picks the pad (0-3), invert_y = 1 turns
 * the stick's Y round, deadzone (of 32767) sets the sticks' dead centre.
 * It works whichever window has focus. */
struct pad { int ok, lx, ly, rx, ry, lt, rt; WORD b; };

static void pad_read(struct pad *p)
{
	typedef struct { WORD b; BYTE lt, rt; SHORT lx, ly, rx, ry; } XGP;
	typedef struct { DWORD packet; XGP g; } XST;
	typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XST *);
	static XInputGetState_t get;
	static int tried, index, dead, was_ok = -1;
	XST st;

	memset(p, 0, sizeof *p);
	if (!tried) {
		static const char *dll[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
		int i;
		tried = 1;
		if (!cfg_int("pad", "enabled", 1)) return;
		for (i = 0; i < 3 && !get; i++) {
			HMODULE m = LoadLibraryA(dll[i]);
			if (m) get = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
		}
		index = cfg_int("pad", "index", 0);
		dead = cfg_int("pad", "deadzone", 7000);
		if (get) say("gamepad: XInput, pad %d", index);
		else say("gamepad: no XInput");
	}
	if (!get) return;
	memset(&st, 0, sizeof st);
	p->ok = get((DWORD)index, &st) == ERROR_SUCCESS;
	if (p->ok != was_ok) { say("gamepad %d %s", index, p->ok ? "connected" : "not connected"); was_ok = p->ok; }
	if (!p->ok) return;
#define DZ(v) ((v) > dead ? ((v) - dead) * 1000 / (32767 - dead) : (v) < -dead ? ((v) + dead) * 1000 / (32767 - dead) : 0)
	p->lx = DZ(st.g.lx); p->ly = DZ(st.g.ly);
	p->rx = DZ(st.g.rx); p->ry = DZ(st.g.ry);
#define CLAMP(v) ((v) > 1000 ? 1000 : (v) < -1000 ? -1000 : (v))
	p->lx = CLAMP(p->lx); p->ly = CLAMP(p->ly); p->rx = CLAMP(p->rx); p->ry = CLAMP(p->ry);
#undef DZ
#undef CLAMP
	if (cfg_int("pad", "invert_y", 0)) p->ry = -p->ry;
	p->lt = st.g.lt > 60; p->rt = st.g.rt > 60;
	p->b = st.g.b;
}

/* Whether an action is held: one of its keys (with a game window focused),
 * or its pad button. */
static int held(int a, int focus, const struct pad *pad)
{
	const struct bind *b = &g_bind[a];
	int i;

	if (focus)
		for (i = 0; i < 4 && b->vk[i]; i++)
			if (key(b->vk[i])) return 1;
	if (!pad->ok) return 0;
	return (pad->b & b->pad_bits) || (b->trigger == 1 && pad->lt) || (b->trigger == 2 && pad->rt);
}

/* A key on the stick: a tap nudges it, holding it pushes further - from a
 * quarter of the way to all the way in [keys] ramp_ms (default 600). *at is
 * when the key went down. */
static int stick_ramp(DWORD *at)
{
	static int ms = -1;
	DWORD held;

	if (ms < 0) ms = cfg_int("keys", "ramp_ms", 600);
	if (!*at) *at = GetTickCount() | 1;
	held = GetTickCount() - *at;
	if (ms <= 0 || (int)held >= ms) return 75;
	return 20 + (int)(55 * held / ms);
}

/* One of the stick's buttons, pressed by whichever input holds it. */
static void stick_button(int *state, int id, int down)
{
	if (down != *state) { *state = down; rio_button(id, down); }
}

static DWORD WINAPI rio_main(LPVOID p)
{
	int throttle = 0, trigger = 0, b46 = 0, b47 = 0, b3f = 0, b3e = 0, all;
	int was_prev = 0, was_next = 0, was_mouse = 0;
	DWORD last = 0, jl_at = 0, ik_at = 0;

	mfd_titles();
	for (;;) {
		int focus = our_window_has_focus();
		int lp = 0, rp = 0, sx = 0, sy = 0, h;
		struct pad pad;

		Sleep(20);
		if ((GetTickCount() / 20) % 100 == 0) mfd_titles();	/* the windows come and go */
		plasma_draw();
		panel_keys(focus);
		pad_read(&pad);

		if (held(A_THROTTLE_UP, focus, &pad)) throttle += 2;
		if (held(A_THROTTLE_DOWN, focus, &pad)) throttle -= 2;
		if (pad.ok) {
			static int part;
			part += pad.ly;			/* held full over: a second, end to end */
			throttle += part / 500;
			part %= 500;
		}
		if (held(A_THROTTLE_FULL, focus, &pad)) throttle = 100;
		if (held(A_THROTTLE_ZERO, focus, &pad)) throttle = 0;
		if (throttle > 100) throttle = 100;
		if (throttle < 0) throttle = 0;

		lp = held(A_PEDAL_LEFT, focus, &pad) ? 500 : 0;
		rp = held(A_PEDAL_RIGHT, focus, &pad) ? 500 : 0;
		sx = held(A_TWIST_LEFT, focus, &pad) ? -stick_ramp(&jl_at)
		   : held(A_TWIST_RIGHT, focus, &pad) ? stick_ramp(&jl_at) : (int)(jl_at = 0);
		sy = held(A_AIM_UP, focus, &pad) ? stick_ramp(&ik_at)
		   : held(A_AIM_DOWN, focus, &pad) ? -stick_ramp(&ik_at) : (int)(ik_at = 0);
		if (pad.ok) {
			if (pad.lx < 0 && -pad.lx / 2 > lp) lp = -pad.lx / 2;
			if (pad.lx > 0 && pad.lx / 2 > rp) rp = pad.lx / 2;
			if (pad.rx) sx = pad.rx * 75 / 1000;
			if (pad.ry) sy = pad.ry * 75 / 1000;
		}

		h = held(A_MFD_PREV, focus, &pad);
		if (h && !was_prev) { g_panel_mfd = (g_panel_mfd + 4) % 5; mfd_titles(); }
		was_prev = h;
		h = held(A_MFD_NEXT, focus, &pad);
		if (h && !was_next) { g_panel_mfd = (g_panel_mfd + 1) % 5; mfd_titles(); }
		was_next = h;
		h = held(A_MOUSE, focus, &pad);
		if (h && !was_mouse) mouse_toggle();
		was_mouse = h;

		/* The stick's buttons fire weapon groups 1, 2 and 3; all three at
		 * once fire everything (the stick's fourth button, which the board
		 * never reports, does nothing in this build). */
		all = held(A_FIRE_ALL, focus, &pad);
		stick_button(&trigger, 0x40, all || held(A_FIRE1, focus, &pad));
		stick_button(&b47, 0x47, all || held(A_FIRE2, focus, &pad));
		stick_button(&b46, 0x46, all || held(A_FIRE3, focus, &pad));
		stick_button(&b3f, 0x3F, held(A_THROTTLE_BUTTON, focus, &pad));
		stick_button(&b3e, 0x3E, held(A_PANIC, focus, &pad));

		if (GetTickCount() - last >= 50) {
			int v[5], i;
			uint8_t d[10];
			v[0] = -(throttle * 800 / 100);
			v[1] = lp;
			v[2] = rp;
			v[3] = sy;
			v[4] = -sx;
			if (g_ctl_axes_on) memcpy(v, g_ctl_axes, sizeof v);
			/* To the game a stick value of exactly 0 means "as before": it
			 * holds Y at its last value, and flips X's sign every message
			 * (0x569060) - a tap aimed all the way, and the view shook. A
			 * real stick's noise never sits on 0; 1 is inside its dead zone. */
			if (!v[3]) v[3] = 1;
			if (!v[4]) v[4] = 1;
			for (i = 0; i < 5; i++) {
				d[2 * i] = (uint8_t)(v[i] & 0x7F);
				d[2 * i + 1] = (uint8_t)((v[i] >> 7) & 0x7F);
			}
			rio_packet(0x87, d, 10);
			last = GetTickCount();
		}
	}
	return 0;
}

/* ------------------------------------------------ the plasma sign */

/* On top of each pod, facing the room, an orange dot-matrix plasma display
 * showed the pilot's name, and after a mission their rank and score in two
 * boxes beneath it. The game drives it on its own serial port - "\\.\com2",
 * 9600 8N1 (0x5680B0) - unless started with -noplasma; with no COM2 the open
 * fails and the sign stays dark. This is that sign: the port is answered here,
 * what the game sends is drawn in a window ([plasma] x, y, w, h), and passed
 * to the control port as "plasma <hex bytes>".
 *
 * The protocol, read from the driver (0x569BE0-0x56A3ED): text is sent as
 * itself; commands start with ESC (1B):
 *
 *   1B 40                  clear
 *   1B 52 x, 1B 51 y       the next text's left edge, its top
 *   1B 4B f                font: 4 and 5 are 10x14 dots, the rest 5x7
 *   1B 48 n, 1B 47 n       an attribute and a mode; the game sends 0
 *   1B 58 x0 y0 x1 y1      a box
 *   1B 78 0 x0 y0 x1 y1    clear a rectangle
 *
 * The game centres a name on (56, 15), so the sign is 112 dots wide; its
 * boxes reach row 30, so 32 high. [plasma] port picks the COM number
 * (default 2); enabled = 0 leaves the port alone. */
#define PL_W 112
#define PL_H 32
static HANDLE g_plasma_h;
static HWND g_plasma_win;
static uint8_t g_pl[PL_H][PL_W];
static volatile LONG g_pl_dirty;
static CRITICAL_SECTION g_pl_lock;

/* The classic 5x7 font, ' ' to '~': five columns, bit 0 at the top. */
static const uint8_t g_font5x7[95][5] = {
	{0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
	{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
	{0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
	{0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
	{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
	{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
	{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
	{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
	{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
	{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
	{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
	{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
	{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
	{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
	{0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
	{0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
	{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
	{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
	{0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
	{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
	{0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
	{0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
	{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
	{0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},
};

static void pl_dot(int x, int y, int on)
{
	if (x >= 0 && x < PL_W && y >= 0 && y < PL_H) g_pl[y][x] = (uint8_t)on;
}

static void pl_rect(int x0, int y0, int x1, int y1, int fill, int on);

/* One character at (x, y): 5x7, or for the large fonts the same at 10x14
 * less a column, so letters keep a gap. Its whole cell is drawn, unlit dots
 * too - the game rubs out a rank or score by writing spaces over it.
 * Returns how far the next one goes. */
static int pl_char(int x, int y, int ch, int big)
{
	static const int col_x[5] = { 0, 2, 4, 5, 7 }, col_w[5] = { 2, 2, 1, 2, 2 };
	const uint8_t *g;
	int c, r, i, j;

	pl_rect(x, y, x + (big ? 9 : 5), y + (big ? 13 : 6), 1, 0);
	if (ch < 0x20 || ch > 0x7E) ch = '?';
	g = g_font5x7[ch - 0x20];
	for (c = 0; c < 5; c++)
		for (r = 0; r < 7; r++) {
			if (!(g[c] >> r & 1)) continue;
			if (!big) pl_dot(x + c, y + r, 1);
			else
				for (i = 0; i < col_w[c]; i++)
					for (j = 0; j < 2; j++) pl_dot(x + col_x[c] + i, y + 2 * r + j, 1);
		}
	return big ? 10 : 6;
}

static void pl_rect(int x0, int y0, int x1, int y1, int fill, int on)
{
	int x, y;
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
			if (fill || y == y0 || y == y1 || x == x0 || x == x1) pl_dot(x, y, on);
}

/* The bytes the game writes, a command at a time. */
static void plasma_feed(const uint8_t *b, DWORD n)
{
	static uint8_t cmd[8];
	static int have, need, cx, cy, font;
	char hex[3 * 64 + 1];
	DWORD i;
	int k = 0;

	for (i = 0; i < n && k < (int)sizeof hex - 4; i++) k += sprintf(hex + k, " %02X", b[i]);
	rio_tell("plasma%s%s", hex, i < n ? " ..." : "");
	EnterCriticalSection(&g_pl_lock);
	for (i = 0; i < n; i++) {
		uint8_t c = b[i];
		if (have) {
			cmd[have++] = c;
			if (have == 2)
				switch (c) {
				case 0x40: need = 2; break;
				case 0x52: case 0x51: case 0x4B: case 0x48: case 0x47: need = 3; break;
				case 0x58: need = 6; break;
				case 0x78: need = 7; break;
				default: say("plasma: unknown command 1B %02X", c); need = 2;
				}
			if (have < need) continue;
			switch (cmd[1]) {
			case 0x40: memset(g_pl, 0, sizeof g_pl); cx = cy = 0; break;
			case 0x52: cx = cmd[2]; break;
			case 0x51: cy = cmd[2]; break;
			case 0x4B: font = cmd[2]; break;
			case 0x58: pl_rect(cmd[2], cmd[3], cmd[4], cmd[5], 0, 1); break;
			case 0x78: pl_rect(cmd[3], cmd[4], cmd[5], cmd[6], 1, cmd[2] != 0); break;
			}
			have = 0;
		} else if (c == 0x1B) {
			have = 1;
			need = 2;
		} else
			cx += pl_char(cx, cy, c, font == 4 || font == 5);
	}
	LeaveCriticalSection(&g_pl_lock);
	InterlockedExchange(&g_pl_dirty, 1);
}

/* Draw the sign: lit dots orange, unlit ones a faint glow, as on the glass. */
static void plasma_draw(void)
{
	enum { S = 6 };
	static uint32_t px[PL_H * S][PL_W * S];
	static DWORD last;
	int x, y, i, j;

	if (!g_plasma_win) return;
	/* now and then regardless, for a window uncovered */
	if (!InterlockedExchange(&g_pl_dirty, 0) && GetTickCount() - last < 1000) return;
	last = GetTickCount();
	EnterCriticalSection(&g_pl_lock);
	for (y = 0; y < PL_H; y++)
		for (x = 0; x < PL_W; x++) {
			uint32_t on = g_pl[y][x] ? 0xFF7A1A : 0x2A0E04;
			for (j = 0; j < S; j++)
				for (i = 0; i < S; i++) {
					int dx = 2 * i - (S - 1), dy = 2 * j - (S - 1);
					px[y * S + j][x * S + i] = dx * dx + dy * dy <= (S - 1) * (S - 1) ? on : 0;
				}
		}
	LeaveCriticalSection(&g_pl_lock);
	draw_rgb(g_plasma_win, &px[0][0], PL_W * S, PL_H * S, 0, 0, PL_W * S, PL_H * S);
}

static HANDLE plasma_open(const char *name)
{
	if (!g_plasma_h) {
		InitializeCriticalSection(&g_pl_lock);
		g_plasma_h = CreateEventA(NULL, TRUE, FALSE, NULL);
		if (cfg_int("shim", "cockpit_windows", g_cockpit))
			g_plasma_win = make_window("Plasma sign", "plasma", 1500, 0, PL_W * 6, PL_H * 6);
		InterlockedExchange(&g_pl_dirty, 1);
	}
	say("plasma: the game opened %s; the virtual sign answers", name);
	return g_plasma_h;
}

/* "\\.\comN", N the sign's port. */
static int is_plasma_port(const char *f)
{
	if (!f || (ULONG_PTR)f < 0x10000 || _strnicmp(f, "\\\\.\\com", 7) || !isdigit((unsigned char)f[7]))
		return 0;
	return cfg_int("plasma", "enabled", g_cockpit) && atoi(f + 7) == cfg_int("plasma", "port", 2);
}

static HANDLE rio_open(const char *name)
{
	if (!g_rio_h) {
		InitializeCriticalSection(&g_rio_lock);
		g_rio_h = CreateEventA(NULL, TRUE, FALSE, NULL);
		CreateThread(NULL, 0, rio_main, NULL, 0, NULL);
	}
	say("RIO: the game opened %s; the virtual board answers. Keys (with a game window"
	    " focused): A/D pedals, J/L stick X, I/K stick Y, R/F throttle, Space/1/2/3 fire"
	    " groups 1-3, 4 all;"
	    " click an MFD's soft-key labels for its buttons", name);
	return g_rio_h;
}

/* The MFD windows' soft keys: four labels above and four below, each the
 * button of the board behind that aux monitor. Which board is which window,
 * and which way each row counts, is [rio] mfd1..mfd5 = base address, until a
 * cockpit settles it; the default reads the Tesla manual's button diagram
 * (top row base+7..base+4, bottom row base+3..base+0). */
static int mfd_button_index(int tile, int top, int col)
{
	static const int def[5] = { 0x28, 0x20, 0x30, 0x08, 0x00 };
	char k[8];
	int base;

	if (tile < 0 || tile >= 5 || col < 0 || col > 3) return -1;
	sprintf(k, "mfd%d", tile + 1);
	base = cfg_int("rio", k, def[tile]);
	return top ? base + 7 - col : base + 3 - col;
}

/* The radar's twelve buttons, six above the screen and six below. The game
 * draws the radar landscape, the labels in rotated text: the radar monitor
 * stood on its side in the pod. Read off a running game: above, left to
 * right, 0x18-0x1D (RANGE, RADAR MODE, LIGHT AMP, SEARCHLIGHT, AUTO CENTER,
 * PILOT MODE); below, 0x10-0x15 (ZOOM, SHUTDOWN, AUXIL MODE, FLUSH, CROUCH,
 * PROG MODE). [rio] radar_top and radar_bottom move them. */
static int radar_button_index(int top, int col)
{
	if (col < 0 || col > 5) return -1;
	return (top ? cfg_int("rio", "radar_top", 0x18) : cfg_int("rio", "radar_bottom", 0x10)) + col;
}

static int radar_rotation(void);

static int radar_button_at(HWND w, int x, int y)
{
	RECT rc;
	int u, v;			/* where in the landscape picture, of 1000 */

	GetClientRect(w, &rc);
	if (rc.right <= 0 || rc.bottom <= 0) return -1;
	u = x * 1000 / rc.right;
	v = y * 1000 / rc.bottom;
	if (radar_rotation() == 90) { int t = u; u = v; v = 999 - t; }
	else if (radar_rotation() == 270) { int t = u; u = 999 - v; v = t; }
	if (v < 1000 * 80 / 480) return radar_button_index(1, u * 6 / 1000);
	if (v > 1000 * 405 / 480) return radar_button_index(0, u * 6 / 1000);
	return -1;
}

static void mfd_titles(void)
{
	int i;
	char t[64];

	if (g_wall_cols && g_ntiles > 0) {
		sprintf(t, "MFDs  [F1-F8: MFD %d]", g_panel_mfd + 1);
		SetWindowTextA(g_tile[0].hwnd, t);
		return;
	}
	for (i = 0; i < g_ntiles && i < 5; i++)
		if (g_tile[i].hwnd) {
			sprintf(t, i == g_panel_mfd ? "MFD %d  [F1-F8]" : "MFD %d", i + 1);
			SetWindowTextA(g_tile[i].hwnd, t);
		}
}

static int mfd_click_at(HWND w, int x, int y)
{
	int i;

	for (i = 0; i < g_ntiles && i < 5; i++) {
		RECT rc;
		int tw, th, col, top;
		POINT p = { x, y };

		if (g_tile[i].hwnd != w) continue;
		tile_rect(i, &rc);
		if (!PtInRect(&rc, p)) continue;
		tw = rc.right - rc.left; th = rc.bottom - rc.top;
		if (tw <= 0 || th <= 0) return -1;
		y -= rc.top; x -= rc.left;
		if (y < th * 48 / 480) top = 1;
		else if (y > th * 432 / 480) top = 0;
		else return -1;
		col = x * 4 / tw;
		return mfd_button_index(i, top, col);
	}
	return -1;
}

/* ---- the serial calls, on our handle */

typedef BOOL (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *WriteFile_t)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *GOR_t)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
typedef BOOL (WINAPI *WCE_t)(HANDLE, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CCE_t)(HANDLE, LPDWORD, LPCOMSTAT);
typedef BOOL (WINAPI *HDW_t)(HANDLE, DWORD);
typedef BOOL (WINAPI *HDCB_t)(HANDLE, LPDCB);
typedef BOOL (WINAPI *SetupComm_t)(HANDLE, DWORD, DWORD);
typedef BOOL (WINAPI *HCT_t)(HANDLE, LPCOMMTIMEOUTS);
typedef BOOL (WINAPI *HCP_t)(HANDLE, LPCOMMPROP);
typedef BOOL (WINAPI *TCC_t)(HANDLE, char);
static ReadFile_t o_ReadFile; static WriteFile_t o_WriteFile; static GOR_t o_GetOverlappedResult;
static WCE_t o_WaitCommEvent; static CCE_t o_ClearCommError;
static HDW_t o_SetCommMask, o_PurgeComm, o_EscapeCommFunction;
static HDCB_t o_SetCommState, o_GetCommState; static SetupComm_t o_SetupComm;
static HCT_t o_SetCommTimeouts, o_GetCommTimeouts; static HCP_t o_GetCommProperties;
static TCC_t o_TransmitCommChar;

/* The sign's port is answered by the same hooks, saying nothing back. */
#define PLASMA(h) ((h) && (h) == g_plasma_h)
#define RIO(h) ((h) && ((h) == g_rio_h || PLASMA(h)))

static BOOL WINAPI i_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
	DWORD k = 0;

	if (!RIO(h)) return o_ReadFile(h, buf, n, got, ov);
	if (PLASMA(h)) n = 0;
	EnterCriticalSection(&g_rio_lock);
	while (k < n && rio_count()) {
		((uint8_t *)buf)[k++] = g_rio_q[g_rio_head];
		g_rio_head = (g_rio_head + 1) % RIO_Q;
	}
	LeaveCriticalSection(&g_rio_lock);
	if (got) *got = k;
	if (ov) { ov->Internal = 0; ov->InternalHigh = k; SetEvent(ov->hEvent); }
	return TRUE;
}

static BOOL WINAPI i_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov)
{
	if (!RIO(h)) return o_WriteFile(h, buf, n, put, ov);
	if (PLASMA(h)) plasma_feed(buf, n);
	else rio_from_host(buf, n);
	if (put) *put = n;
	if (ov) { ov->Internal = 0; ov->InternalHigh = n; SetEvent(ov->hEvent); }
	return TRUE;
}

static BOOL WINAPI i_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD n, BOOL wait)
{
	if (!RIO(h)) return o_GetOverlappedResult(h, ov, n, wait);
	if (n) *n = ov ? (DWORD)ov->InternalHigh : 0;
	return TRUE;
}

static BOOL WINAPI i_WaitCommEvent(HANDLE h, LPDWORD mask, LPOVERLAPPED ov)
{
	if (!RIO(h)) return o_WaitCommEvent(h, mask, ov);
	if (PLASMA(h)) {
		if (mask) *mask = 0;
		if (ov) { ov->Internal = 0; ov->InternalHigh = 0; SetEvent(ov->hEvent); }
		return TRUE;
	}
	EnterCriticalSection(&g_rio_lock);
	if (ov) ResetEvent(ov->hEvent);
	if (rio_count() || !ov) {
		if (mask) *mask = EV_RXCHAR;
		if (ov) { ov->Internal = 0; ov->InternalHigh = 0; SetEvent(ov->hEvent); }
		LeaveCriticalSection(&g_rio_lock);
		return TRUE;
	}
	g_rio_wait_ov = ov;
	g_rio_wait_mask = mask;
	LeaveCriticalSection(&g_rio_lock);
	SetLastError(ERROR_IO_PENDING);
	return FALSE;
}

static BOOL WINAPI i_ClearCommError(HANDLE h, LPDWORD err, LPCOMSTAT st)
{
	if (!RIO(h)) return o_ClearCommError(h, err, st);
	if (err) *err = 0;
	if (st) { memset(st, 0, sizeof *st); st->cbInQue = PLASMA(h) ? 0 : rio_count(); }
	return TRUE;
}

static BOOL WINAPI i_SetCommMask(HANDLE h, DWORD m) { return RIO(h) ? TRUE : o_SetCommMask(h, m); }
static BOOL WINAPI i_PurgeComm(HANDLE h, DWORD f) { return RIO(h) ? TRUE : o_PurgeComm(h, f); }
static BOOL WINAPI i_EscapeCommFunction(HANDLE h, DWORD f)
{
	if (!RIO(h)) return o_EscapeCommFunction(h, f);
	if (f == CLRDTR && !PLASMA(h)) say("RIO: the game reset the board (DTR)");
	return TRUE;
}
static BOOL WINAPI i_SetCommState(HANDLE h, LPDCB d) { return RIO(h) ? TRUE : o_SetCommState(h, d); }
static BOOL WINAPI i_GetCommState(HANDLE h, LPDCB d)
{
	if (!RIO(h)) return o_GetCommState(h, d);
	memset(d, 0, sizeof *d);
	d->DCBlength = sizeof *d;
	d->BaudRate = CBR_19200;
	d->ByteSize = 8;
	d->fBinary = 1;
	return TRUE;
}
static BOOL WINAPI i_SetupComm(HANDLE h, DWORD a, DWORD b) { return RIO(h) ? TRUE : o_SetupComm(h, a, b); }
static BOOL WINAPI i_SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) { return RIO(h) ? TRUE : o_SetCommTimeouts(h, t); }
static BOOL WINAPI i_GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t)
{
	if (!RIO(h)) return o_GetCommTimeouts(h, t);
	memset(t, 0, sizeof *t);
	return TRUE;
}
static BOOL WINAPI i_GetCommProperties(HANDLE h, LPCOMMPROP p)
{
	if (!RIO(h)) return o_GetCommProperties(h, p);
	memset(p, 0, sizeof *p);
	p->wPacketLength = sizeof *p;
	p->dwMaxTxQueue = p->dwMaxRxQueue = RIO_Q;
	p->dwMaxBaud = BAUD_USER;
	p->dwProvSubType = PST_RS232;
	return TRUE;
}
static BOOL WINAPI i_TransmitCommChar(HANDLE h, char c) { return RIO(h) ? TRUE : o_TransmitCommChar(h, c); }

static void rio_hooks(void)
{
	iat_hook("KERNEL32.dll", "ReadFile", i_ReadFile, (void **)&o_ReadFile);
	iat_hook("KERNEL32.dll", "WriteFile", i_WriteFile, (void **)&o_WriteFile);
	iat_hook("KERNEL32.dll", "GetOverlappedResult", i_GetOverlappedResult, (void **)&o_GetOverlappedResult);
	iat_hook("KERNEL32.dll", "WaitCommEvent", i_WaitCommEvent, (void **)&o_WaitCommEvent);
	iat_hook("KERNEL32.dll", "ClearCommError", i_ClearCommError, (void **)&o_ClearCommError);
	iat_hook("KERNEL32.dll", "SetCommMask", i_SetCommMask, (void **)&o_SetCommMask);
	iat_hook("KERNEL32.dll", "PurgeComm", i_PurgeComm, (void **)&o_PurgeComm);
	iat_hook("KERNEL32.dll", "EscapeCommFunction", i_EscapeCommFunction, (void **)&o_EscapeCommFunction);
	iat_hook("KERNEL32.dll", "SetCommState", i_SetCommState, (void **)&o_SetCommState);
	iat_hook("KERNEL32.dll", "GetCommState", i_GetCommState, (void **)&o_GetCommState);
	iat_hook("KERNEL32.dll", "SetupComm", i_SetupComm, (void **)&o_SetupComm);
	iat_hook("KERNEL32.dll", "SetCommTimeouts", i_SetCommTimeouts, (void **)&o_SetCommTimeouts);
	iat_hook("KERNEL32.dll", "GetCommTimeouts", i_GetCommTimeouts, (void **)&o_GetCommTimeouts);
	iat_hook("KERNEL32.dll", "GetCommProperties", i_GetCommProperties, (void **)&o_GetCommProperties);
	iat_hook("KERNEL32.dll", "TransmitCommChar", i_TransmitCommChar, (void **)&o_TransmitCommChar);
}

/* ------------------------------------------------ faults */

/* The game's own handler reports a fault as an offset from the start of
 * .text and exits. A vectored handler runs before it, so the shim logs the
 * real address, the registers, and the stack words that point into MW4.exe's
 * code - enough of a call chain to find the caller. First fault only. */
static LONG CALLBACK on_fault(PEXCEPTION_POINTERS ep)
{
	static LONG seen;
	PEXCEPTION_RECORD er = ep->ExceptionRecord;
	PCONTEXT c = ep->ContextRecord;
	DWORD *sp;
	int i, n = 0;

	if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || InterlockedExchange(&seen, 1))
		return EXCEPTION_CONTINUE_SEARCH;
	say("FAULT %08lX at eip %08lX: %s %08lX", er->ExceptionCode, c->Eip,
	    er->ExceptionInformation[0] ? "write" : "read", er->ExceptionInformation[1]);
	say("  eax %08lX ebx %08lX ecx %08lX edx %08lX", c->Eax, c->Ebx, c->Ecx, c->Edx);
	say("  esi %08lX edi %08lX ebp %08lX esp %08lX", c->Esi, c->Edi, c->Ebp, c->Esp);
	/* Firestorm's display devices keep their IDirectDraw7 at this+8 (esi). */
	if (!IsBadReadPtr((void *)c->Esi, 0x20)) {
		DWORD *t = (DWORD *)c->Esi;
		say("  this %08lX: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX", c->Esi,
		    t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
		if (!IsBadReadPtr((void *)t[2], 4) && !IsBadReadPtr(*(void **)t[2], 0x70)) {
			void **vt = *(void ***)t[2];
			HMODULE m = NULL;
			char mod[MAX_PATH] = "?";
			struct dev *d = dev_of((void *)t[2]);
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[DD_SetCooperativeLevel], &m);
			if (m) GetModuleFileNameA(m, mod, sizeof mod);
			say("  dd %08lX (%s) vtbl %p, SetCooperativeLevel %p in %s, our hook %p", t[2],
			    d ? KIND[d->kind] : "not ours", (void *)vt, vt[DD_SetCooperativeLevel], mod,
			    (void *)h_SetCooperativeLevel);
		}
	}
	sp = (DWORD *)c->Esp;
	for (i = 0; i < 2048 && n < 16; i++) {
		DWORD v;
		if (IsBadReadPtr(&sp[i], 4)) break;
		v = sp[i];
		if (v >= 0x401000 && v < 0x6E2000) { say("  stack +%03X: %08lX", i * 4, v); n++; }
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
{
	if (why == DLL_PROCESS_ATTACH) {
		char path[MAX_PATH], *slash;

		InitializeCriticalSection(&g_loglock);
		InitializeCriticalSection(&g_lock);
		AddVectoredExceptionHandler(1, on_fault);
		GetModuleFileNameA(h, g_dir, sizeof g_dir);
		if ((slash = strrchr(g_dir, '\\'))) slash[1] = 0;
		sprintf(g_ini, "%sshim.ini", g_dir);
		{
			/* Each role gets its own log and snapshots: a console and a pod
			 * can run side by side from one directory. */
			const char *t = strstr(GetCommandLineA(), "-ctcltype ");
			if (t) {
				snprintf(g_tag, sizeof g_tag, "ctcl%d-", atoi(t + 10));
				snprintf(g_role, sizeof g_role, "ctcl%d", atoi(t + 10));
			}
			g_cockpit = strstr(GetCommandLineA(), "-tmfds ") != NULL;
			{
				char exe[MAX_PATH], *base;
				GetModuleFileNameA(NULL, exe, sizeof exe);
				base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
				if (!_stricmp(base, "Launcher.exe")) {
					g_launcher = 1;
					strcpy(g_role, "launcher");
					strcpy(g_tag, "launcher-");
				} else if (!_stricmp(base, "mw4print.exe")) {
					/* The battle-report printer ran on the console's
					 * PC, and shares the console's c:\mw4files. */
					g_launcher = 1;
					strcpy(g_role, "ctcl1");
					strcpy(g_tag, "mw4print-");
				}
			}
		}
		snprintf(path, sizeof path, "%sshim%s%s.log", g_dir, g_tag[0] ? "-" : "", g_tag);
		if (g_tag[0]) path[strlen(path) - 5] = 0, strcat(path, ".log");
		g_log = fopen(path, "w");
		GetModuleFileNameA(NULL, path, sizeof path);
		say("firestorm-shim phase 2 (windows), loaded into %s", path);
		say("command line: %s", GetCommandLineA());
		say("role: %s%s", g_role, g_cockpit ? ", a cockpit (MFD and radar windows)" : "");
		say("settings: %s%s", g_ini, GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES ? " (absent; defaults)" : "");
		load_real();
		redirect_ctcl();
		if (!g_launcher) {
			rio_hooks();
			keyboard_hooks();
			CreateThread(NULL, 0, ctl_main, NULL, 0, NULL);
		}
	}
	return TRUE;
}
