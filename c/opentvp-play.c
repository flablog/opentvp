/*
 * opentvp-play — minimal player for TVPaint 11 files (.tvpp), without TVPaint.
 *
 *   opentvp-play [file.tvpp file2.tvpp ...] [--clip N [--frames A-B]] [--canvas] [--scale S] [--blur-samples N] [--threads N]
 *
 * Plays every clip of every file in order (or only --clip N of a single file), rendered through
 * the camera with the same renderer as `opentvp render`. Frames are rendered in a background
 * thread and kept in memory; playback starts before loading is done. The timeline shows one
 * section per clip, the frames already in memory and an in/out selection. Clips hidden in
 * TVPaint are left out until H shows them. Files dropped on the window replace the timeline (top
 * half) or are appended to it (bottom half); started without a file, the window waits for some.
 * C shows the current clip's whole canvas, outside the camera darkened. The preview resolution
 * (half or full) is chosen in the bar and remembered. The keyframes (drawing changes) of each clip
 * are found when the file is opened: Shift shows them on the timeline, snaps the playhead to them
 * while dragging, and Shift+Left / Right jumps between them. F1 shows the keyboard shortcuts,
 * Ctrl+I the file information.
 *
 * Window layer: Win32 (GDI, OLE drag and drop) on Windows; X11 (XDND) on Linux, with libX11
 * loaded at run time (dlopen), so the binary has no build-time X11 dependency. macOS is not
 * supported yet.
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#define OPENTVP_NO_MAIN
#include "opentvp.c"
#include "third_party/spleen/spleen.h"
#include <stdarg.h>

#ifdef __APPLE__
#error "opentvp-play has no macOS window layer yet"
#endif

/* ------------------------------------------------------------------ events */

enum { EV_QUIT = 1, EV_KEY, EV_DOWN, EV_DOUBLE, EV_UP, EV_MOVE, EV_WHEEL, EV_REDRAW };
enum { K_OTHER, K_SPACE, K_LEFT, K_RIGHT, K_PREV_CLIP, K_NEXT_CLIP, K_HOME, K_END, K_ESC, K_F, K_Q,
       K_HELP, K_IN, K_OUT, K_RESET, K_INFO, K_HIDDEN, K_CANVAS, K_GROW_PREV, K_GROW_NEXT, K_PREV_KEY, K_NEXT_KEY, K_LUMA, K_COPY, K_MUTE };
typedef struct { int type, key, x, y; } Event;

#define EVQ 256
static Event g_evq[EVQ];
static int g_evh, g_evt;
static void ev_push(int type, int key, int x, int y) {
    int n = (g_evt + 1) % EVQ;
    if (n == g_evh) return;                 /* queue full: drop */
    Event e = {type, key, x, y};
    g_evq[g_evt] = e; g_evt = n;
}
static int ev_pop(Event *e) {
    if (g_evh == g_evt) return 0;
    *e = g_evq[g_evh]; g_evh = (g_evh + 1) % EVQ;
    return 1;
}

/* keys typed as characters, whatever the keyboard layout ([ ] \ need AltGr on AZERTY) */
static int char_key(unsigned c) {
    switch (c) {
    case ' ': case 'k': case 'K': return K_SPACE;
    case 'f': case 'F': return K_F;
    case 'q': case 'Q': return K_Q;
    case 'i': case 'I': case '[': return K_IN;
    case 'o': case 'O': case ']': return K_OUT;
    case '\\': return K_RESET;
    case '?': return K_HELP;
    case 'h': case 'H': return K_HIDDEN;
    case 'c': case 'C': return K_CANVAS;
    case 'l': case 'L': return K_LUMA;
    case 'm': case 'M': return K_MUTE;
    }
    return K_OTHER;
}

/* window framebuffer, 0x00RRGGBB, g_ww x g_wh */
static uint32_t *g_fb;
static int g_ww, g_wh, g_fbcap;
static int g_fullscreen;
static void (*g_repaint)(void);           /* redraw now (Windows calls it while the user resizes) */

/* files dragged over the window: top half replaces the timeline, bottom half appends to it */
enum { DROP_REPLACE = 1, DROP_APPEND };
static int g_dragfile, g_drag_y;          /* a drag is over the window, and the cursor height */
static char **g_drop;                     /* dropped paths (UTF-8), handled by the main loop */
static int g_ndrop, g_drop_mode;
static void drop_add(const char *path) {
    g_drop = xrealloc(g_drop, sizeof(char *) * (g_ndrop + 1));
    g_drop[g_ndrop++] = strdup(path);
}

static void fb_resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w * h > g_fbcap) { g_fbcap = w * h; g_fb = xrealloc(g_fb, sizeof(uint32_t) * g_fbcap); }
    g_ww = w; g_wh = h;
}

/* ------------------------------------------------------------------ Windows */
#ifdef _WIN32

static HWND g_hwnd;
static int g_cursor_on = 1;
static WINDOWPLACEMENT g_place = {sizeof(WINDOWPLACEMENT)};
static LONG g_style;

static void blit(HDC dc) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_ww; bi.bmiHeader.biHeight = -g_wh;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, g_ww, g_wh, 0, 0, 0, g_wh, g_fb, &bi, DIB_RGB_COLORS);
}

/* keys that do not type a character */
static int vk_key(WPARAM vk) {
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    switch (vk) {
    case VK_LEFT: return ctrl ? (shift ? K_GROW_PREV : K_PREV_CLIP) : (shift ? K_PREV_KEY : K_LEFT);
    case VK_RIGHT: return ctrl ? (shift ? K_GROW_NEXT : K_NEXT_CLIP) : (shift ? K_NEXT_KEY : K_RIGHT);
    case VK_HOME: return K_HOME;
    case VK_END: return K_END;
    case VK_ESCAPE: return K_ESC;
    case VK_F1: return K_HELP;
    case VK_F11: return K_F;
    case 'I': return ctrl ? K_INFO : K_OTHER;   /* plain I arrives as a character */
    case 'C': return ctrl ? K_COPY : K_OTHER;
    }
    return K_OTHER;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (m) {
    case WM_CLOSE: ev_push(EV_QUIT, 0, 0, 0); return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { fb_resize(LOWORD(lp), HIWORD(lp)); if (g_repaint) g_repaint(); }
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); if (g_fb) blit(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SYSCOMMAND:                          /* Alt alone would open the system menu and keep the next keys */
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && !g_cursor_on) { SetCursor(NULL); return TRUE; }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        int k = vk_key(wp);
        if (k != K_OTHER) { ev_push(EV_KEY, k, 0, 0); return 0; }
        break;                                   /* TranslateMessage turns it into WM_CHAR */
    }
    case WM_CHAR: {
        int k = char_key((unsigned)wp);
        if (k != K_OTHER) ev_push(EV_KEY, k, 0, 0);
        return 0;
    }
    case WM_LBUTTONDOWN: SetCapture(h); ev_push(EV_DOWN, 0, x, y); return 0;
    case WM_LBUTTONDBLCLK: SetCapture(h); ev_push(EV_DOUBLE, 0, x, y); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); ev_push(EV_UP, 0, x, y); return 0;
    case WM_MOUSEMOVE: ev_push(EV_MOVE, 0, x, y); return 0;
    case WM_MOUSEWHEEL: {   /* position in screen coordinates */
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)}; ScreenToClient(h, &pt);
        ev_push(EV_WHEEL, (short)HIWORD(wp) > 0 ? -1 : 1, pt.x, pt.y); return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* OLE drop target: unlike WM_DROPFILES, it reports the drag while it is over the window */
static const IID my_IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const IID my_IID_IDropTarget = {0x00000122, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static int g_drag_ok;

static int has_files(IDataObject *d) {
    FORMATETC f = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return d->lpVtbl->QueryGetData(d, &f) == S_OK;
}
static void drag_at(POINTL pt) {
    POINT p = {pt.x, pt.y};
    ScreenToClient(g_hwnd, &p);
    g_drag_y = p.y;
}
static HRESULT STDMETHODCALLTYPE dt_query(IDropTarget *t, REFIID riid, void **pv) {
    if (IsEqualIID(riid, &my_IID_IUnknown) || IsEqualIID(riid, &my_IID_IDropTarget)) { *pv = t; return S_OK; }
    *pv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dt_addref(IDropTarget *t) { (void)t; return 2; }    /* static object */
static ULONG STDMETHODCALLTYPE dt_release(IDropTarget *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE dt_enter(IDropTarget *t, IDataObject *d, DWORD keys, POINTL pt, DWORD *effect) {
    (void)t; (void)keys;
    g_drag_ok = has_files(d);
    g_dragfile = g_drag_ok;
    if (g_drag_ok) drag_at(pt);
    *effect = g_drag_ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    ev_push(EV_REDRAW, 0, 0, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dt_over(IDropTarget *t, DWORD keys, POINTL pt, DWORD *effect) {
    (void)t; (void)keys;
    if (g_drag_ok) drag_at(pt);
    *effect = g_drag_ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    ev_push(EV_REDRAW, 0, 0, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dt_leave(IDropTarget *t) { (void)t; g_dragfile = 0; ev_push(EV_REDRAW, 0, 0, 0); return S_OK; }
static HRESULT STDMETHODCALLTYPE dt_drop(IDropTarget *t, IDataObject *d, DWORD keys, POINTL pt, DWORD *effect) {
    (void)t; (void)keys;
    g_dragfile = 0;
    *effect = DROPEFFECT_NONE;
    FORMATETC f = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m;
    if (d->lpVtbl->GetData(d, &f, &m) != S_OK) return S_OK;
    HDROP h = (HDROP)GlobalLock(m.hGlobal);
    if (h) {
        drag_at(pt);
        UINT n = DragQueryFileW(h, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t w[1024]; char u[4096];
            DragQueryFileW(h, i, w, 1024);
            WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
            drop_add(u);
        }
        g_drop_mode = g_drag_y < g_wh / 2 ? DROP_REPLACE : DROP_APPEND;
        GlobalUnlock(m.hGlobal);
        *effect = DROPEFFECT_COPY;
    }
    ReleaseStgMedium(&m);
    ev_push(EV_REDRAW, 0, 0, 0);
    return S_OK;
}
static IDropTargetVtbl g_dt_vtbl = {dt_query, dt_addref, dt_release, dt_enter, dt_over, dt_leave, dt_drop};
static IDropTarget g_dt = {&g_dt_vtbl};

/* Shift held down while the window has the focus */
static int plat_shift(void) { return GetForegroundWindow() == g_hwnd && GetKeyState(VK_SHIFT) < 0; }
static int plat_alt(void) { return GetKeyState(VK_MENU) < 0; }

static void plat_screen(int *w, int *h) { *w = GetSystemMetrics(SM_CXSCREEN); *h = GetSystemMetrics(SM_CYSCREEN); }
static int plat_ui_scale(void) {
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return dpi >= 144 ? (dpi + 48) / 96 : 1;
}

static void plat_open(const char *title, int w, int h) {
    SetProcessDPIAware();
    timeBeginPeriod(1);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"opentvp-play";
    RegisterClassW(&wc);
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    fb_resize(w, h);
    wchar_t *wt = widen(title);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, wt, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    free(wt);
    if (!g_hwnd) die("cannot create the window");
    OleInitialize(NULL);
    RegisterDragDrop(g_hwnd, &g_dt);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
}

static void plat_title(const char *title) { wchar_t *wt = widen(title); SetWindowTextW(g_hwnd, wt); free(wt); }

/* client area size, window centred on its screen */
static void plat_resize(int w, int h) {
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int ow = r.right - r.left, oh = r.bottom - r.top;
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ow) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - oh) / 2;
    SetWindowPos(g_hwnd, NULL, x, y, ow, oh, SWP_NOZORDER);
}

static void plat_pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

static void plat_present(void) { HDC dc = GetDC(g_hwnd); blit(dc); ReleaseDC(g_hwnd, dc); }

static void plat_fullscreen(int on) {
    if (on == g_fullscreen) return;
    g_fullscreen = on;
    if (on) {
        MONITORINFO mi = {sizeof mi};
        g_style = GetWindowLongW(g_hwnd, GWL_STYLE);
        GetWindowPlacement(g_hwnd, &g_place);
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g_hwnd, GWL_STYLE, g_style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, g_style);
        SetWindowPlacement(g_hwnd, &g_place);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static void plat_cursor(int on) {
    if (on == g_cursor_on) return;
    g_cursor_on = on;
    POINT p; GetCursorPos(&p); SetCursorPos(p.x, p.y);   /* triggers WM_SETCURSOR */
}

/* Ctrl+C: rgb (w x h, 3 bytes per pixel) to the clipboard, as a DIB and as PNG (kept by Windows after we quit) */
static HGLOBAL global_copy(const void *p, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g) { memcpy(GlobalLock(g), p, n); GlobalUnlock(g); }
    return g;
}
static int plat_copy_image(const uint8_t *rgb, int w, int h) {
    size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3, n = sizeof(BITMAPINFOHEADER) + stride * h;
    uint8_t *dib = xcalloc(n);
    BITMAPINFOHEADER *bh = (BITMAPINFOHEADER *)dib;
    bh->biSize = sizeof *bh; bh->biWidth = w; bh->biHeight = h;          /* bottom-up */
    bh->biPlanes = 1; bh->biBitCount = 24; bh->biCompression = BI_RGB; bh->biSizeImage = (DWORD)(stride * h);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = rgb + (size_t)y * w * 3;
        uint8_t *d = dib + sizeof *bh + (size_t)(h - 1 - y) * stride;
        for (int x = 0; x < w; x++) { d[x * 3] = s[x * 3 + 2]; d[x * 3 + 1] = s[x * 3 + 1]; d[x * 3 + 2] = s[x * 3]; }
    }
    Bytes png = {0, 0, 0};
    png_encode(&png, rgb, w, h, 3);
    HGLOBAL gd = global_copy(dib, n), gp = global_copy(png.d, png.n);
    free(dib); free(png.d);
    int ok = 0;
    if (gd && OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        ok = SetClipboardData(CF_DIB, gd) != NULL;
        if (ok) gd = NULL;
        if (gp && SetClipboardData(RegisterClipboardFormatW(L"PNG"), gp)) gp = NULL;
        CloseClipboard();
    }
    if (gd) GlobalFree(gd);
    if (gp) GlobalFree(gp);
    return ok;
}

static double now(void) {
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}
static void sleep_ms(int ms) { Sleep(ms); }

static void thread_start(DWORD (WINAPI *fn)(LPVOID)) { CreateThread(NULL, 0, fn, NULL, 0, NULL); }
#define THREAD_FN(name) static DWORD WINAPI name(LPVOID arg_)
#define THREAD_RET return 0

/* launched by double-click or drag-and-drop: the console is ours alone, hide it and show errors in a box */
static void msgbox_error(const char *msg) {
    wchar_t *w = widen(msg);
    MessageBoxW(NULL, w, L"opentvp-play", MB_OK | MB_ICONERROR);
    free(w);
}
static int own_console(void) {
    DWORD ids[2];
    return GetConsoleProcessList(ids, 2) <= 1;
}

/* ------------------------------------------------------------------ Linux (X11, loaded at run time) */
#else

#include <dlfcn.h>
#include <X11/Xlib.h>
int XLookupString(XKeyEvent *event, char *buffer, int bytes, KeySym *keysym, void *status);   /* from Xutil.h */

#define X11_FUNCS(F) F(XOpenDisplay) F(XCreateSimpleWindow) F(XSelectInput) F(XMapWindow) F(XStoreName) \
    F(XInternAtom) F(XSetWMProtocols) F(XPending) F(XNextEvent) F(XCreateGC) F(XCreateImage) F(XPutImage) \
    F(XFlush) F(XLookupString) F(XSendEvent) F(XChangeProperty) F(XCreateBitmapFromData) \
    F(XCreatePixmapCursor) F(XDefineCursor) F(XUndefineCursor) F(XFreePixmap) F(XResizeWindow) \
    F(XTranslateCoordinates) F(XConvertSelection) F(XGetWindowProperty) F(XFree) F(XQueryPointer) \
    F(XSetSelectionOwner) F(XMaxRequestSize) F(XExtendedMaxRequestSize)
#define X11_DECL(name) static __typeof__(name) *p_##name;
X11_FUNCS(X11_DECL)

static int x11_load(void) {
    void *h = dlopen("libX11.so.6", RTLD_NOW);
    if (!h) h = dlopen("libX11.so", RTLD_NOW);
    if (!h) return 0;
#define X11_LOAD(name) if (!(p_##name = (__typeof__(p_##name))dlsym(h, #name))) return 0;
    X11_FUNCS(X11_LOAD)
    return 1;
}

static Display *g_dpy;
static Window g_win;
static GC g_gc;
static int g_scr;
static Atom a_delete, a_state, a_fullscreen, a_name, a_utf8;
static Atom a_dnd_aware, a_dnd_enter, a_dnd_position, a_dnd_status, a_dnd_leave, a_dnd_drop, a_dnd_finished,
            a_dnd_selection, a_dnd_copy, a_uri_list, a_drop_prop;
static Window g_dnd_source;            /* XDND: window of the application that drags files over us */
static Atom a_clipboard, a_targets, a_png;
static Bytes g_clip;                   /* PNG we offer on CLIPBOARD, while we own it */
static Cursor g_blank;
static Time g_last_click;
static int g_last_cx, g_last_cy;

static void plat_screen(int *w, int *h) { *w = DisplayWidth(g_dpy, g_scr); *h = DisplayHeight(g_dpy, g_scr); }

/* Shift / Alt held down (the X server's modifier state) */
static unsigned x_mods(void) {
    Window r, c; int rx, ry, wx, wy; unsigned mask = 0;
    p_XQueryPointer(g_dpy, g_win, &r, &c, &rx, &ry, &wx, &wy, &mask);
    return mask;
}
static int plat_shift(void) { return (x_mods() & ShiftMask) != 0; }
static int plat_alt(void) { return (x_mods() & Mod1Mask) != 0; }
/* from the physical screen size; when the server does not know it, 2 for screens taller than 1600 px */
static int plat_ui_scale(void) {
    int mm = DisplayHeightMM(g_dpy, g_scr), px = DisplayHeight(g_dpy, g_scr);
    double dpi = mm > 0 ? px * 25.4 / mm : 96;
    if (mm <= 0 || dpi < 50) return px > 1600 ? 2 : 1;
    return dpi >= 144 ? (int)((dpi + 48) / 96) : 1;
}

static void plat_title(const char *title) {
    p_XStoreName(g_dpy, g_win, title);
    p_XChangeProperty(g_dpy, g_win, a_name, a_utf8, 8, PropModeReplace, (const unsigned char *)title, (int)strlen(title));
    p_XFlush(g_dpy);
}

static void plat_open(const char *title, int w, int h) {
    if (!x11_load()) die("libX11 not found: opentvp-play needs an X11 (or XWayland) desktop");
    g_dpy = p_XOpenDisplay(NULL);
    if (!g_dpy) die("cannot open the X display (is DISPLAY set?)");
    g_scr = DefaultScreen(g_dpy);
    if (DefaultDepth(g_dpy, g_scr) < 24) die("the X display needs a 24-bit colour depth");
    g_win = p_XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, g_scr), 0, 0, (unsigned)w, (unsigned)h, 0,
                                  BlackPixel(g_dpy, g_scr), BlackPixel(g_dpy, g_scr));
    p_XSelectInput(g_dpy, g_win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                                 PointerMotionMask | StructureNotifyMask);
    a_delete = p_XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    a_state = p_XInternAtom(g_dpy, "_NET_WM_STATE", False);
    a_fullscreen = p_XInternAtom(g_dpy, "_NET_WM_STATE_FULLSCREEN", False);
    a_name = p_XInternAtom(g_dpy, "_NET_WM_NAME", False);
    a_utf8 = p_XInternAtom(g_dpy, "UTF8_STRING", False);
    p_XSetWMProtocols(g_dpy, g_win, &a_delete, 1);
    /* XDND (drag and drop of files), protocol version 5 */
    a_dnd_aware = p_XInternAtom(g_dpy, "XdndAware", False);
    a_dnd_enter = p_XInternAtom(g_dpy, "XdndEnter", False);
    a_dnd_position = p_XInternAtom(g_dpy, "XdndPosition", False);
    a_dnd_status = p_XInternAtom(g_dpy, "XdndStatus", False);
    a_dnd_leave = p_XInternAtom(g_dpy, "XdndLeave", False);
    a_dnd_drop = p_XInternAtom(g_dpy, "XdndDrop", False);
    a_dnd_finished = p_XInternAtom(g_dpy, "XdndFinished", False);
    a_dnd_selection = p_XInternAtom(g_dpy, "XdndSelection", False);
    a_dnd_copy = p_XInternAtom(g_dpy, "XdndActionCopy", False);
    a_uri_list = p_XInternAtom(g_dpy, "text/uri-list", False);
    a_drop_prop = p_XInternAtom(g_dpy, "OPENTVP_DROP", False);
    a_clipboard = p_XInternAtom(g_dpy, "CLIPBOARD", False);
    a_targets = p_XInternAtom(g_dpy, "TARGETS", False);
    a_png = p_XInternAtom(g_dpy, "image/png", False);
    long version = 5;
    p_XChangeProperty(g_dpy, g_win, a_dnd_aware, 4 /* XA_ATOM */, 32, PropModeReplace, (unsigned char *)&version, 1);
    g_gc = p_XCreateGC(g_dpy, g_win, 0, NULL);
    static char zero[8];
    Pixmap pm = p_XCreateBitmapFromData(g_dpy, g_win, zero, 1, 1);
    XColor black; memset(&black, 0, sizeof black);
    g_blank = p_XCreatePixmapCursor(g_dpy, pm, pm, &black, &black, 0, 0);
    p_XFreePixmap(g_dpy, pm);
    fb_resize(w, h);
    plat_title(title);
    p_XMapWindow(g_dpy, g_win);
    p_XFlush(g_dpy);
}

/* the window manager answers with a ConfigureNotify, which resizes the framebuffer */
static void plat_resize(int w, int h) { p_XResizeWindow(g_dpy, g_win, (unsigned)w, (unsigned)h); p_XFlush(g_dpy); }

static int x_key(XKeyEvent *k) {
    char buf[8]; KeySym ks = 0;
    int n = p_XLookupString(k, buf, sizeof buf, &ks, NULL);
    int ctrl = (k->state & ControlMask) != 0, shift = (k->state & ShiftMask) != 0;
    switch (ks) {
    case 0xff51: return ctrl ? (shift ? K_GROW_PREV : K_PREV_CLIP) : (shift ? K_PREV_KEY : K_LEFT);
    case 0xff53: return ctrl ? (shift ? K_GROW_NEXT : K_NEXT_CLIP) : (shift ? K_NEXT_KEY : K_RIGHT);
    case 0xff50: return K_HOME;
    case 0xff57: return K_END;
    case 0xff1b: return K_ESC;
    case 0xffbe: return K_HELP;                  /* F1 */
    case 0xffc8: return K_F;                     /* F11 */
    case 'i': case 'I': if (ctrl) return K_INFO; break;
    case 'c': case 'C': if (ctrl) return K_COPY; break;
    }
    return n == 1 ? char_key((unsigned char)buf[0]) : K_OTHER;
}

/* Ctrl+C: we own CLIPBOARD and hand out the PNG when an application pastes. X11 keeps nothing:
 * the image can be pasted while the player is open (or after, with a clipboard manager) */
static int plat_copy_image(const uint8_t *rgb, int w, int h) {
    free(g_clip.d); memset(&g_clip, 0, sizeof g_clip);
    png_encode(&g_clip, rgb, w, h, 3);
    long maxreq = p_XExtendedMaxRequestSize(g_dpy);
    if (!maxreq) maxreq = p_XMaxRequestSize(g_dpy);
    if (g_clip.n + 1024 > (size_t)maxreq * 4) { free(g_clip.d); memset(&g_clip, 0, sizeof g_clip); return 0; }   /* no INCR */
    p_XSetSelectionOwner(g_dpy, a_clipboard, g_win, CurrentTime);
    p_XFlush(g_dpy);
    return 1;
}

static void clip_request(XSelectionRequestEvent *q) {
    XEvent r;
    memset(&r, 0, sizeof r);
    r.xselection.type = SelectionNotify; r.xselection.requestor = q->requestor; r.xselection.selection = q->selection;
    r.xselection.target = q->target; r.xselection.time = q->time;
    Atom prop = q->property ? q->property : q->target;    /* obsolete clients give no property */
    r.xselection.property = None;
    if (g_clip.n && q->target == a_targets) {
        Atom t[2] = {a_targets, a_png};
        p_XChangeProperty(g_dpy, q->requestor, prop, 4 /* XA_ATOM */, 32, PropModeReplace, (unsigned char *)t, 2);
        r.xselection.property = prop;
    } else if (g_clip.n && q->target == a_png) {
        p_XChangeProperty(g_dpy, q->requestor, prop, a_png, 8, PropModeReplace, g_clip.d, (int)g_clip.n);
        r.xselection.property = prop;
    }
    p_XSendEvent(g_dpy, q->requestor, False, NoEventMask, &r);
    p_XFlush(g_dpy);
}

/* XDND: message to the drag source */
static void dnd_reply(Atom type, long l1, long l2, long l3, long l4) {
    XEvent r;
    memset(&r, 0, sizeof r);
    r.xclient.type = ClientMessage; r.xclient.window = g_dnd_source; r.xclient.message_type = type; r.xclient.format = 32;
    r.xclient.data.l[0] = (long)g_win; r.xclient.data.l[1] = l1; r.xclient.data.l[2] = l2;
    r.xclient.data.l[3] = l3; r.xclient.data.l[4] = l4;
    p_XSendEvent(g_dpy, g_dnd_source, False, NoEventMask, &r);
    p_XFlush(g_dpy);
}

/* text/uri-list: one file:// URI per line, percent-encoded */
static void dnd_uris(const char *s) {
    while (*s) {
        const char *e = s;
        while (*e && *e != '\r' && *e != '\n') e++;
        if (!strncmp(s, "file://", 7)) {
            const char *p = s + 7;
            while (p < e && *p != '/') p++;                  /* skip the host part */
            char path[4096]; size_t n = 0;
            for (; p < e && n + 1 < sizeof path; p++) {
                if (*p == '%' && p + 2 < e) { unsigned v; if (sscanf(p + 1, "%2x", &v) == 1) { path[n++] = (char)v; p += 2; continue; } }
                path[n++] = *p;
            }
            path[n] = 0;
            if (n) drop_add(path);
        }
        s = e;
        while (*s == '\r' || *s == '\n') s++;
    }
}

static void plat_pump(void) {
    while (p_XPending(g_dpy)) {
        XEvent e;
        p_XNextEvent(g_dpy, &e);
        switch (e.type) {
        case ClientMessage:
            if (e.xclient.message_type == a_dnd_enter) {
                g_dnd_source = (Window)e.xclient.data.l[0]; g_dragfile = 1; ev_push(EV_REDRAW, 0, 0, 0);
            } else if (e.xclient.message_type == a_dnd_position) {
                int x = (int)((unsigned long)e.xclient.data.l[2] >> 16), y = (int)(e.xclient.data.l[2] & 0xffff), wx, wy;
                Window child;
                g_dnd_source = (Window)e.xclient.data.l[0];
                p_XTranslateCoordinates(g_dpy, RootWindow(g_dpy, g_scr), g_win, x, y, &wx, &wy, &child);
                g_dragfile = 1; g_drag_y = wy;
                dnd_reply(a_dnd_status, 3, 0, 0, (long)a_dnd_copy);          /* accepted, keep sending positions */
                ev_push(EV_REDRAW, 0, 0, 0);
            } else if (e.xclient.message_type == a_dnd_leave) {
                g_dragfile = 0; ev_push(EV_REDRAW, 0, 0, 0);
            } else if (e.xclient.message_type == a_dnd_drop) {
                g_dnd_source = (Window)e.xclient.data.l[0];
                g_drop_mode = g_drag_y < g_wh / 2 ? DROP_REPLACE : DROP_APPEND;
                p_XConvertSelection(g_dpy, a_dnd_selection, a_uri_list, a_drop_prop, g_win, (Time)e.xclient.data.l[2]);
                g_dragfile = 0; ev_push(EV_REDRAW, 0, 0, 0);
            } else if ((Atom)e.xclient.data.l[0] == a_delete) ev_push(EV_QUIT, 0, 0, 0);
            break;
        case SelectionRequest:                       /* an application pastes what Ctrl+C copied */
            if (e.xselectionrequest.selection == a_clipboard) clip_request(&e.xselectionrequest);
            break;
        case SelectionClear:                         /* another application copied something */
            if (e.xselectionclear.selection == a_clipboard) { free(g_clip.d); memset(&g_clip, 0, sizeof g_clip); }
            break;
        case SelectionNotify:                        /* XDND: the dropped file list */
            if (e.xselection.property == a_drop_prop) {
                Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
                if (p_XGetWindowProperty(g_dpy, g_win, a_drop_prop, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &n, &after, &data) == Success && data) {
                    dnd_uris((const char *)data);
                    p_XFree(data);
                }
                dnd_reply(a_dnd_finished, 1, (long)a_dnd_copy, 0, 0);
                ev_push(EV_REDRAW, 0, 0, 0);
            }
            break;
        case ConfigureNotify:
            if (e.xconfigure.width != g_ww || e.xconfigure.height != g_wh) {
                fb_resize(e.xconfigure.width, e.xconfigure.height);
                ev_push(EV_REDRAW, 0, 0, 0);
            }
            break;
        case Expose:
            if (e.xexpose.count == 0) ev_push(EV_REDRAW, 0, 0, 0);
            break;
        case KeyPress: {
            int k = x_key(&e.xkey);
            if (k != K_OTHER) ev_push(EV_KEY, k, 0, 0);
            break;
        }
        case ButtonPress:
            if (e.xbutton.button == 1) {
                int dbl = e.xbutton.time - g_last_click < 400 && abs(e.xbutton.x - g_last_cx) < 5 && abs(e.xbutton.y - g_last_cy) < 5;
                ev_push(dbl ? EV_DOUBLE : EV_DOWN, 0, e.xbutton.x, e.xbutton.y);
                g_last_click = dbl ? 0 : e.xbutton.time; g_last_cx = e.xbutton.x; g_last_cy = e.xbutton.y;
            } else if (e.xbutton.button == 4 || e.xbutton.button == 5) {
                ev_push(EV_WHEEL, e.xbutton.button == 4 ? -1 : 1, e.xbutton.x, e.xbutton.y);
            }
            break;
        case ButtonRelease:
            if (e.xbutton.button == 1) ev_push(EV_UP, 0, e.xbutton.x, e.xbutton.y);
            break;
        case MotionNotify:
            ev_push(EV_MOVE, 0, e.xmotion.x, e.xmotion.y);
            break;
        }
    }
}

static void plat_present(void) {
    XImage *img = p_XCreateImage(g_dpy, DefaultVisual(g_dpy, g_scr), (unsigned)DefaultDepth(g_dpy, g_scr), ZPixmap, 0,
                                 (char *)g_fb, (unsigned)g_ww, (unsigned)g_wh, 32, g_ww * 4);
    if (!img) return;
    p_XPutImage(g_dpy, g_win, g_gc, img, 0, 0, 0, 0, (unsigned)g_ww, (unsigned)g_wh);
    img->data = NULL;                        /* the framebuffer is ours */
    img->f.destroy_image(img);
    p_XFlush(g_dpy);
}

static void plat_fullscreen(int on) {
    if (on == g_fullscreen) return;
    g_fullscreen = on;
    XEvent e;
    memset(&e, 0, sizeof e);
    e.type = ClientMessage;
    e.xclient.window = g_win;
    e.xclient.message_type = a_state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = on ? 1 : 0;           /* _NET_WM_STATE_ADD / _REMOVE */
    e.xclient.data.l[1] = (long)a_fullscreen;
    e.xclient.data.l[3] = 1;
    p_XSendEvent(g_dpy, RootWindow(g_dpy, g_scr), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    p_XFlush(g_dpy);
}

static void plat_cursor(int on) {
    if (on) p_XUndefineCursor(g_dpy, g_win); else p_XDefineCursor(g_dpy, g_win, g_blank);
    p_XFlush(g_dpy);
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&t, NULL); }

static void thread_start(void *(*fn)(void *)) { pthread_t t; pthread_create(&t, NULL, fn, NULL); pthread_detach(t); }
#define THREAD_FN(name) static void *name(void *arg_)
#define THREAD_RET return NULL

#endif

/* ------------------------------------------------------------------ files and their clips, played one after the other */

/* an open .tvpp file */
typedef struct {
    Project P;
    char *path;
    const char *base;         /* file name without the folder */
} File;

/* a playable clip; hidden clips take part only while they are shown (H) */
typedef struct {
    File *F;
    Clip *C;
    int number;               /* clip number in its file, from 1 */
    Renderer R;
    float bg[3];
    int f0, f1, n;            /* project frames of the clip that are played */
    int w, h;                 /* size of its frames in memory */
    double fps;
    uint32_t **frames;        /* n entries, NULL until loaded; kept when the clip is hidden again */
    int nloaded;
    uint8_t *keys;            /* n entries: KEY_CHANGE / KEY_NEW flags, found when the file is opened */
    int active, start;        /* part of the sequence, and its position there */
    float *mix;               /* sound of the clip over its frames (same mix as render --sound), NULL if none */
    long long mixn;           /* frames of 2 samples at SND_RATE */
} Section;

/* keyframes: a visible layer shows another drawing than on the previous frame (KEY_CHANGE), or a
 * drawing not shown before in the clip (KEY_NEW); the first frame of a clip is both */
enum { KEY_CHANGE = 1, KEY_NEW = 2 };
static int g_key_mode = KEY_CHANGE;     /* which ones Shift shows and jumps to */

static void section_keys(Section *s) {
    s->keys = xcalloc(s->n);
    const Clip *C = s->C;
    int *seen = xmalloc(sizeof(int) * s->n);
    for (int l = 0; l < C->nlayers; l++) {
        const Layer *L = &C->layers[l];
        if (!L->visible) continue;
        LayerState S; layer_init(&S, L, 0, 0);       /* reads the entry kinds only: no pixels */
        int *ids = drawing_ids(&S, (int)L->count), nids = L->count > 0 ? (int)L->count : 1;
        int prev = -2, nseen = 0;
        for (int i = 0; i < s->n; i++) {
            int k = frame_source(L, s->f0 + i), d = (k < 0 || k >= nids) ? -1 : ids[k];
            if (d >= 0 && d != prev) {
                s->keys[i] |= KEY_CHANGE;
                int j = 0; while (j < nseen && seen[j] != d) j++;
                if (j == nseen) { seen[nseen++] = d; s->keys[i] |= KEY_NEW; }
            }
            prev = d;
        }
        free(ids); free(S.keep); free(S.cache);
    }
    free(seen);
    if (s->n) s->keys[0] |= KEY_CHANGE | KEY_NEW;
}

static File **FILES;
static Section **SECS;                  /* individually allocated: the loader keeps pointers to them */
static int NFILES, NALL, NHIDDEN;
static int *ACT;                        /* active sections, in timeline order */
static int NSEC, NFR;
static double g_scale = 0.5;            /* preview resolution, relative to the camera */
static int g_scale_from_cli, g_blur_samples = 8;
static int g_show_hidden;
static int g_canvas_all;                /* --canvas: every clip opened shows the whole canvas */

/* shared with the loader thread, guarded by lock(): the sequence, frames, playhead and selection */
static int g_cur, g_in, g_out;
static int g_loader_running, g_oom, g_stop;
static double g_load_start, g_load_time;

static Section *sec(int k) { return SECS[ACT[k]]; }

static int section_of(int i) {
    int k = 0;
    while (k + 1 < NSEC && i >= sec(k + 1)->start) k++;
    return k;
}

static uint32_t **slot(int i) { Section *s = sec(section_of(i)); return &s->frames[i - s->start]; }

static int key_at(int i) { Section *s = sec(section_of(i)); return s->keys[i - s->start] & g_key_mode; }

/* next keyframe after i (dir 1) or before it (dir -1), -1 if none */
static int next_key(int i, int dir) {
    for (int j = i + dir; j >= 0 && j < NFR; j += dir) if (key_at(j)) return j;
    return -1;
}

/* keyframe closest to i (the earlier one on a tie); frame 0 always is one */
static int nearest_key(int i) {
    if (key_at(i)) return i;
    int a = next_key(i, -1), b = next_key(i, 1);
    if (a < 0) return b < 0 ? i : b;
    return b >= 0 && b - i < i - a ? b : a;
}

/* size of the frames in memory: the camera, or the whole canvas (C), at the preview resolution */
static void section_size(Section *s) {
    const Project *P = &s->F->P;
    s->R.scale = g_scale;
    s->R.ow = (int)((s->R.canvas ? P->width : P->out_w) * g_scale + 0.5);
    s->R.oh = (int)((s->R.canvas ? P->height : P->out_h) * g_scale + 0.5);
    if (s->R.ow < 1) s->R.ow = 1;
    if (s->R.oh < 1) s->R.oh = 1;
    s->w = s->R.ow; s->h = s->R.oh;
}

/* canvas view: darkens by half what lies outside the camera frame at frame f (first motion blur sample) */
static void passe_partout(const Section *s, int f, uint32_t *fr) {
    const Project *P = &s->F->P;
    const Clip *C = s->C;
    int N = C->visible_frames > 0 ? C->visible_frames : 1;
    CamPt cp = camera_at_t(P, C, f / (double)(N > 1 ? N - 1 : 1));
    Affine A = camera_affine(&cp, P->out_w, P->out_h, 1);    /* camera pixels -> canvas */
    double det = A.a * A.e - A.b * A.d;
    if (fabs(det) < 1e-12) return;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            double dx = (x + 0.5) / g_scale - A.c, dy = (y + 0.5) / g_scale - A.f;
            double u = (A.e * dx - A.b * dy) / det, v = (-A.d * dx + A.a * dy) / det;
            uint32_t *p = &fr[(size_t)y * s->w + x];
            if (u < 0 || v < 0 || u >= P->out_w || v >= P->out_h) *p = (*p >> 1) & 0x7f7f7f;
        }
}

/* rebuilds the sequence from the shown clips (caller holds the lock) */
static void rebuild(void) {
    NSEC = NFR = 0;
    for (int k = 0; k < NALL; k++) {
        Section *s = SECS[k];
        s->active = !s->C->hidden || g_show_hidden;
        if (!s->active) continue;
        s->start = NFR; NFR += s->n;
        ACT[NSEC++] = k;
    }
}

/* next frame to load: from the playhead to the out point, then the rest of the selection, then the rest */
static int next_to_load(void) {
    int c = g_cur, a = g_in, b = g_out;
    for (int i = c; i <= b; i++) if (!*slot(i)) return i;
    for (int i = a; i < c && i <= b; i++) if (!*slot(i)) return i;
    for (int i = b + 1; i < NFR; i++) if (!*slot(i)) return i;
    for (int i = 0; i < a; i++) if (!*slot(i)) return i;
    return -1;
}

static int loaded_total(void) { int n = 0; for (int k = 0; k < NSEC; k++) n += sec(k)->nloaded; return n; }

THREAD_FN(loader) {
    (void)arg_;
    float *acc = NULL, *cover = NULL;
    uint8_t *rgb = NULL;
    size_t cap = 0;
    for (;;) {
        lock();
        int i = g_stop ? -1 : next_to_load();
        Section *s = i < 0 ? NULL : sec(section_of(i));
        int local = s ? i - s->start : 0;
        if (!s) { g_loader_running = 0; g_load_time = now() - g_load_start; }
        unlock();
        if (!s) break;
        size_t npx = (size_t)s->w * s->h;
        if (npx > cap) {
            cap = npx;
            acc = xrealloc(acc, sizeof(float) * 4 * cap); cover = xrealloc(cover, sizeof(float) * cap); rgb = xrealloc(rgb, cap * 3);
        }
        render_frame(&s->R, s->f0 + local, acc, cover);
        compose_pixels(acc, cover, s->w, s->h, 0, s->bg, rgb);
        uint32_t *fr = malloc(sizeof(uint32_t) * npx);
        if (!fr) {
            lock(); g_oom = 1; g_loader_running = 0; int n = loaded_total(); unlock();
            fprintf(stderr, "  ⚠ out of memory after %d frame(s): playing those only (try half resolution)\n", n);
            break;
        }
        for (size_t p = 0; p < npx; p++)
            fr[p] = ((uint32_t)rgb[p * 3] << 16) | ((uint32_t)rgb[p * 3 + 1] << 8) | rgb[p * 3 + 2];
        if (s->R.canvas) passe_partout(s, s->f0 + local, fr);
        lock(); s->frames[local] = fr; s->nloaded++; unlock();
    }
    free(acc); free(cover); free(rgb);
    THREAD_RET;
}

/* starts the loader if frames are missing and it is not running */
static void loader_wake(void) {
    lock();
    int go = !g_loader_running && !g_oom && NFR && next_to_load() >= 0;
    if (go) { g_loader_running = 1; g_load_start = now(); }
    unlock();
    if (go) thread_start(loader);
}

/* stops the loader after the frame it is rendering */
static void loader_stop(void) {
    lock(); g_stop = 1; unlock();
    for (;;) { lock(); int r = g_loader_running; unlock(); if (!r) break; sleep_ms(5); }
    lock(); g_stop = 0; g_oom = 0; unlock();
}

static const uint32_t *frame_at(int i, int *w, int *h) {
    lock();
    Section *s = sec(section_of(i));
    const uint32_t *f = s->frames[i - s->start];
    unlock();
    if (w) { *w = s->w; *h = s->h; }
    return f;
}
static void set_cur(int i) { lock(); g_cur = i; unlock(); }
static void set_inout(int a, int b) { lock(); g_in = a; g_out = b; unlock(); }

/* ------------------------------------------------------------------ sound
 * Each clip's mix is computed when its file is opened (section_sound). An audio thread plays the
 * timeline from a cursor (clip, sample), at SND_RATE, 16-bit stereo, by blocks of 10 ms. The video
 * clock drives playback: the main loop sets the cursor on the displayed frame whenever playback
 * (re)starts or jumps, and stops the sound when playback pauses or waits for frames. Between two
 * cues the thread follows the timeline by itself: next clip, and back to the in point after the
 * out point, like advance(). Each cue empties the device, so the first sample is heard at once. */

#define SND_BLOCK 480                   /* 10 ms */
static int g_snd_on = 1;                /* M, or the speaker button */
static float g_snd_vol = 1;             /* level 0..1: slider in the bar, saved in play.ini */
static int g_snd_scrub;                 /* scrub sound: each frame shown outside playback plays its sound (saved) */
static int g_snd_dev;                   /* 1: device open, -1: no device */
/* guarded by lock(): */
static int g_a_play, g_a_gen, g_a_k;    /* playing, cue counter (each cue empties the device), clip in ACT */
static long long g_a_pos;               /* sample in that clip's mix */
static long long g_a_from, g_a_end = -1; /* scrub: the one frame being played (g_a_end < 0: normal playback) */
#define SND_RAMP 96                     /* 2 ms fades at both ends of a scrubbed frame: no clicks */

/* first sample of timeline frame i (in section s) */
static long long snd_sample(const Section *s, int i) { return (long long)rint((double)(i - s->start) * SND_RATE / s->fps); }

/* next block, under lock(); returns 0 (block not filled) if a cue came in after `gen` */
static int snd_fill(int16_t *out, int frames, int gen) {
    lock();
    if (g_a_gen != gen) { unlock(); return 0; }
    for (int j = 0; j < frames; j++) {
        float v[2] = {0, 0};
        if (g_a_play && g_a_end >= 0 && g_a_k < NSEC) {
            Section *s = sec(g_a_k);
            if (g_a_pos >= g_a_end) g_a_play = 0;
            else {
                long long a = g_a_pos - g_a_from, b = g_a_end - g_a_pos;
                float r = a < SND_RAMP ? (float)a / SND_RAMP : 1.f;
                if (b < SND_RAMP && (float)b / SND_RAMP < r) r = (float)b / SND_RAMP;
                if (s->mix && g_a_pos >= 0 && g_a_pos < s->mixn) { v[0] = s->mix[2 * g_a_pos] * g_snd_vol * r; v[1] = s->mix[2 * g_a_pos + 1] * g_snd_vol * r; }
                g_a_pos++;
            }
        } else if (g_a_play && NSEC && g_out >= g_in) {
            int ko = section_of(g_out);
            if (g_a_k >= NSEC || g_a_k > ko) { g_a_k = section_of(g_in); g_a_pos = snd_sample(sec(g_a_k), g_in); }
            Section *s = sec(g_a_k);
            long long end = g_a_k == ko ? snd_sample(s, g_out + 1) : snd_sample(s, s->start + s->n);
            if (g_a_pos >= end) {
                if (g_a_k == ko) { g_a_k = section_of(g_in); g_a_pos = snd_sample(sec(g_a_k), g_in); }
                else { g_a_k++; g_a_pos = 0; }
                s = sec(g_a_k);
            }
            if (s->mix && g_a_pos >= 0 && g_a_pos < s->mixn) { v[0] = s->mix[2 * g_a_pos] * g_snd_vol; v[1] = s->mix[2 * g_a_pos + 1] * g_snd_vol; }
            g_a_pos++;
        }
        for (int c = 0; c < 2; c++) {
            float x = v[c] < -1.f ? -1.f : (v[c] > 1.f ? 1.f : v[c]);
            out[2 * j + c] = (int16_t)lrintf(x * 32767.f);
        }
    }
    unlock();
    return 1;
}

/* main thread: sound from frame i (just displayed), or silence */
static void snd_cue(int i) {
    lock();
    g_a_k = section_of(i); g_a_pos = snd_sample(sec(g_a_k), i); g_a_end = -1; g_a_play = 1; g_a_gen++;
    unlock();
}
/* main thread: scrub, the sound of frame i only (one frame long) */
static void snd_scrub(int i) {
    lock();
    g_a_k = section_of(i);
    Section *s = sec(g_a_k);
    g_a_from = g_a_pos = snd_sample(s, i); g_a_end = snd_sample(s, i + 1); g_a_play = 1; g_a_gen++;
    unlock();
}
static void snd_stop(void) { lock(); g_a_play = 0; g_a_gen++; unlock(); }

#ifdef _WIN32
#include <mmsystem.h>
#define SND_NBUF 4
THREAD_FN(audio_thread) {
    (void)arg_;
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, SND_RATE, SND_RATE * 4, 4, 16, 0};
    HANDLE ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    HWAVEOUT h;
    if (waveOutOpen(&h, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        g_snd_dev = -1;
        fprintf(stderr, "  ⚠ no sound output device: playing without sound\n");
        THREAD_RET;
    }
    g_snd_dev = 1;
    static int16_t buf[SND_NBUF][SND_BLOCK * 2];
    WAVEHDR hd[SND_NBUF];
    memset(hd, 0, sizeof hd);
    for (int i = 0; i < SND_NBUF; i++) {
        hd[i].lpData = (LPSTR)buf[i]; hd[i].dwBufferLength = sizeof buf[i];
        waveOutPrepareHeader(h, &hd[i], sizeof hd[i]);
        hd[i].dwFlags |= WHDR_DONE;          /* free */
    }
    int gen = -1;
    for (;;) {
        lock(); int g = g_a_gen, play = g_a_play; unlock();
        if (g != gen) { gen = g; waveOutReset(h); }   /* every queued buffer comes back as done */
        if (play)
            for (int i = 0; i < SND_NBUF; i++)
                if ((hd[i].dwFlags & WHDR_DONE) && snd_fill(buf[i], SND_BLOCK, gen)) {
                    hd[i].dwFlags &= ~WHDR_DONE;
                    waveOutWrite(h, &hd[i], sizeof hd[i]);
                }
        WaitForSingleObject(ev, play ? 20 : 10);
    }
}
#else
/* ALSA, loaded at run time like X11: no build-time dependency, no sound without libasound */
typedef struct snd_pcm_ snd_pcm_t;
static int (*p_snd_pcm_open)(snd_pcm_t **, const char *, int, int);
static int (*p_snd_pcm_set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
static long (*p_snd_pcm_writei)(snd_pcm_t *, const void *, unsigned long);
static int (*p_snd_pcm_recover)(snd_pcm_t *, int, int);
static int (*p_snd_pcm_drop)(snd_pcm_t *);
static int (*p_snd_pcm_prepare)(snd_pcm_t *);
THREAD_FN(audio_thread) {
    (void)arg_;
    void *lib = dlopen("libasound.so.2", RTLD_NOW);
    snd_pcm_t *pcm = NULL;
    if (lib) {
        p_snd_pcm_open = (__typeof__(p_snd_pcm_open))dlsym(lib, "snd_pcm_open");
        p_snd_pcm_set_params = (__typeof__(p_snd_pcm_set_params))dlsym(lib, "snd_pcm_set_params");
        p_snd_pcm_writei = (__typeof__(p_snd_pcm_writei))dlsym(lib, "snd_pcm_writei");
        p_snd_pcm_recover = (__typeof__(p_snd_pcm_recover))dlsym(lib, "snd_pcm_recover");
        p_snd_pcm_drop = (__typeof__(p_snd_pcm_drop))dlsym(lib, "snd_pcm_drop");
        p_snd_pcm_prepare = (__typeof__(p_snd_pcm_prepare))dlsym(lib, "snd_pcm_prepare");
    }
    /* stream 0 = playback; format 2 = S16_LE; access 3 = RW interleaved; 40 ms of buffer */
    if (!lib || !p_snd_pcm_open || !p_snd_pcm_set_params || !p_snd_pcm_writei || !p_snd_pcm_recover ||
        !p_snd_pcm_drop || !p_snd_pcm_prepare || p_snd_pcm_open(&pcm, "default", 0, 0) < 0 ||
        p_snd_pcm_set_params(pcm, 2, 3, 2, SND_RATE, 1, 40000) < 0) {
        g_snd_dev = -1;
        fprintf(stderr, "  ⚠ no sound output (ALSA, libasound.so.2): playing without sound\n");
        THREAD_RET;
    }
    g_snd_dev = 1;
    static int16_t buf[SND_BLOCK * 2];
    int gen = -1;
    for (;;) {
        lock(); int g = g_a_gen, play = g_a_play; unlock();
        if (g != gen) { gen = g; p_snd_pcm_drop(pcm); p_snd_pcm_prepare(pcm); }
        if (!play || !snd_fill(buf, SND_BLOCK, gen)) { sleep_ms(5); continue; }
        long r = p_snd_pcm_writei(pcm, buf, SND_BLOCK);       /* blocks while the buffer is full */
        if (r < 0) p_snd_pcm_recover(pcm, (int)r, 1);
    }
}
#endif

/* ------------------------------------------------------------------ opening and closing files */

/* errors while opening a dropped file come back here instead of ending the program */
#include <setjmp.h>
static _Thread_local jmp_buf *g_try;
static char g_try_msg[600];
static int g_own_console;

static void player_die(const char *msg) {
    if (g_try) { snprintf(g_try_msg, sizeof g_try_msg, "%s", msg); longjmp(*g_try, 1); }
#ifdef _WIN32
    if (g_own_console) msgbox_error(msg);
#endif
}

static void free_frames(Section *s) {
    for (int i = 0; i < s->n; i++) { free(s->frames[i]); s->frames[i] = NULL; }
    s->nloaded = 0;
}

static void free_section(Section *s) {
    free_frames(s);
    free(s->frames);
    free(s->keys);
    free(s->mix);
    for (int i = 0; i < s->R.nls; i++) {
        LayerState *L = &s->R.ls[i];
        free(L->img);
        for (int k = 0; k < L->nkeep; k++) free(L->cache[k]);
        free(L->cache); free(L->keep);
    }
    free(s->R.ls);
    free(s);
}

static void free_file(File *F) {
    Project *P = &F->P;
    free(P->file.d);
    for (int c = 0; c < P->nclips; c++) {
        Clip *C = &P->clips[c];
        for (int l = 0; l < C->nlayers; l++) { free(C->layers[l].name); free(C->layers[l].sraw.a); free(C->layers[l].rsraw.a); }
        free(C->layers); free(C->pts); free(C->name);
    }
    free(P->clips); free(P->author); free(P->modified);
    free(F->path);
    free(F);
}

static const char *base_name(const char *p) {
    const char *b = p + strlen(p);
    while (b > p && b[-1] != '/' && b[-1] != '\\') b--;
    return b;
}

/* the clip's sound, mixed once when the file is opened (frames f0..f1, as `render --sound
 * --project-sound`): its own tracks and the project's, as TVPaint plays the project */
static void section_sound(Section *s) {
    const Sound *S = &s->C->sound, *PS = &s->F->P.sound;
    int any = 0;
    if (!S->mute) for (int k = 0; k < S->n; k++) any |= !S->t[k].mute;
    if (!PS->mute) for (int k = 0; k < PS->n; k++) any |= !PS->t[k].mute;
    if (!any) return;
    long long n; int mixed;
    double *d = sound_mix(&s->F->P, s->C, s->f0, s->f1, 1, &n, &mixed);
    int silent = 1;
    for (long long i = 0; i < 2 * n && silent; i++) silent = d[i] == 0;
    if (!silent) {                       /* all silent (files not found, unreadable): no sound kept */
        s->mix = xmalloc(sizeof(float) * 2 * (size_t)(n ? n : 1));
        for (long long i = 0; i < 2 * n; i++) s->mix[i] = (float)d[i];
        s->mixn = n;
    }
    free(d);
}

/* opens a file and prepares its playable clips (all of them, or only clip `clip`); NULL and a message on error */
static File *load_file(const char *path, int clip, int f0, int f1, Section ***out, int *nout, char *err, size_t errn) {
    jmp_buf jb;
    File *F = xcalloc(sizeof *F);
    Section **list = xcalloc(sizeof(Section *));
    int n = 0;
    g_try = &jb;
    if (setjmp(jb)) {                       /* partial allocations are left behind: rare, and small next to the frames */
        g_try = NULL;
        snprintf(err, errn, "%s: %s", base_name(path), g_try_msg);
        return NULL;
    }
    F->P = load_project(path);
    F->path = strdup(path);
    F->base = base_name(F->path);
    if (clip && (clip < 1 || clip > F->P.nclips)) die("invalid clip number");
    list = xrealloc(list, sizeof(Section *) * (F->P.nclips + 1));
    for (int k = clip ? clip - 1 : 0; k < (clip ? clip : F->P.nclips); k++) {
        Section *s = xcalloc(sizeof *s);
        s->F = F;
        s->number = k + 1;
        s->f0 = clip ? f0 : -1; s->f1 = clip ? f1 : -1;
        s->C = renderer_setup(&s->R, &F->P, k + 1, g_scale, g_blur_samples);
        render_range(s->C, &s->f0, &s->f1);
        if (s->f1 < s->f0) { free(s->R.ls); free(s); continue; }
        render_background(s->C, "project", s->bg);
        s->n = s->f1 - s->f0 + 1;
        s->R.canvas = g_canvas_all;
        section_size(s);
        s->fps = F->P.fps > 0 ? F->P.fps : 25;
        s->frames = xcalloc(sizeof(uint32_t *) * s->n);
        section_keys(s);
        section_sound(s);
        list[n++] = s;
    }
    g_try = NULL;
    if (!n) {
        snprintf(err, errn, "%s: no clip with visible content", F->base);
        free(list); free_file(F);
        return NULL;
    }
    *out = list; *nout = n;
    return F;
}

/* adds an opened file at the end of the timeline */
static void commit(File *F, Section **list, int n) {
    lock();
    FILES = xrealloc(FILES, sizeof(File *) * (NFILES + 1));
    FILES[NFILES++] = F;
    SECS = xrealloc(SECS, sizeof(Section *) * (NALL + n));
    ACT = xrealloc(ACT, sizeof(int) * (NALL + n));
    for (int k = 0; k < n; k++) { SECS[NALL++] = list[k]; NHIDDEN += list[k]->C->hidden; }
    rebuild();
    if (!NFR) { g_show_hidden = 1; rebuild(); }    /* only hidden clips: play them anyway */
    unlock();
    free(list);
}

/* closes every file (the loader is stopped first) */
static void close_all(void) {
    loader_stop();
    lock();
    for (int k = 0; k < NALL; k++) free_section(SECS[k]);
    for (int k = 0; k < NFILES; k++) free_file(FILES[k]);
    NALL = NFILES = NHIDDEN = NSEC = NFR = 0;
    g_show_hidden = 0;
    unlock();
}

/* preview resolution: frames are rendered again (files and layer caches are kept) */
static void apply_scale(double sc) {
    loader_stop();
    lock();
    g_scale = sc;
    for (int k = 0; k < NALL; k++) { free_frames(SECS[k]); section_size(SECS[k]); }
    unlock();
    loader_wake();
}

/* C: the clip switches between the camera view and the whole canvas; its frames are rendered again */
static void set_canvas(Section *s, int on) {
    loader_stop();
    lock();
    free_frames(s);
    s->R.canvas = on;
    section_size(s);
    unlock();
    loader_wake();
}

/* ------------------------------------------------------------------ settings (the preview resolution is remembered) */

static void settings_file(char *b, size_t n, int dir_only) {
    char d[2048];
#ifdef _WIN32
    const wchar_t *a = _wgetenv(L"APPDATA");
    char u[1600] = ".";
    if (a) WideCharToMultiByte(CP_UTF8, 0, a, -1, u, sizeof u, NULL, NULL);
    snprintf(d, sizeof d, "%s/opentvp", u);
#else
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(d, sizeof d, "%s/opentvp", x); else snprintf(d, sizeof d, "%s/.config/opentvp", h ? h : ".");
#endif
    snprintf(b, n, dir_only ? "%s" : "%s/play.ini", d);
}

static double load_saved_scale(void) {
    char p[2100]; settings_file(p, sizeof p, 0);
    FILE *f = fopen_u8(p, "rb");
    double v = 0;
    double vol = -1;
    if (f) {
        char line[200];
        while (fgets(line, sizeof line, f)) { sscanf(line, "scale=%lf", &v); sscanf(line, "volume=%lf", &vol); sscanf(line, "scrub=%d", &g_snd_scrub); }
        fclose(f);
    }
    if (vol >= 0 && vol <= 1) g_snd_vol = (float)vol;
    return v > 0 && v <= 4 ? v : 0.5;
}

static void save_scale(void) {
    char d[2100], p[2100];
    settings_file(d, sizeof d, 1); settings_file(p, sizeof p, 0);
    mkdir_p(d);
    FILE *f = fopen_u8(p, "wb");
    if (f) { fprintf(f, "# opentvp-play settings\nscale=%g\nvolume=%g\nscrub=%d\n", g_scale, g_snd_vol, g_snd_scrub); fclose(f); }
}

/* ------------------------------------------------------------------ drawing */

/* interface scale: 1 at 96 dpi, 2 at 192 dpi (a 4K screen at 200 %); g_tmul enlarges text (drop overlay) */
static int g_ui = 1, g_tmul = 1;
#define BAR_H (62 * g_ui)
#define COL_BAR 0x1c1c1c
#define COL_ICON 0xe8e8e8
#define COL_DIM 0x9a9a9a
#define COL_TRACK 0x343434
#define COL_LOADED 0x6a6a6a
#define COL_PLAYED 0x4fa3e0
#define COL_MARK 0xf0a030
#define COL_WAIT 0xe04848
#define COL_SEP 0x5a5a5a
#define COL_HIDDEN 0xb48ce0
#define COL_MENU 0x2c2c2c
#define COL_KEY 0xf0d040

static void fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_ww ? g_ww : x + w, y1 = y + h > g_wh ? g_wh : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = g_fb + (size_t)j * g_ww;
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}

/* darkens a rectangle to 1/4 (panels) */
static void shade(int x, int y, int w, int h) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_ww ? g_ww : x + w, y1 = y + h > g_wh ? g_wh : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = g_fb + (size_t)j * g_ww;
        for (int i = x0; i < x1; i++) row[i] = (row[i] >> 2) & 0x3f3f3f;
    }
}

/* darkens a rectangle, keeping keep/256 of each channel */
static void darken(int x, int y, int w, int h, uint32_t keep) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_ww ? g_ww : x + w, y1 = y + h > g_wh ? g_wh : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = g_fb + (size_t)j * g_ww;
        for (int i = x0; i < x1; i++)
            row[i] = (((row[i] & 0xff00ff) * keep >> 8) & 0xff00ff) | (((row[i] & 0x00ff00) * keep >> 8) & 0x00ff00);
    }
}

/* L: a rectangle in grey levels, Rec. 709 luma (54 + 183 + 19 = 256) */
static int g_luma;
static void luminance(int x, int y, int w, int h) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_ww ? g_ww : x + w, y1 = y + h > g_wh ? g_wh : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = g_fb + (size_t)j * g_ww;
        for (int i = x0; i < x1; i++) {
            uint32_t p = row[i], l = (54 * (p >> 16 & 0xff) + 183 * (p >> 8 & 0xff) + 19 * (p & 0xff)) >> 8;
            row[i] = l << 16 | l << 8 | l;
        }
    }
}

static void frame_rect(int x, int y, int w, int h, int t, uint32_t c) {
    fill(x, y, w, t, c); fill(x, y + h - t, w, t, c); fill(x, y, t, h, c); fill(x + w - t, y, t, h, c);
}

/* text: Spleen 6x12, or 12x24 from 2x interface scale */
static int font_w(void) { return (g_ui >= 2 ? SPLEEN_12X24_W * (g_ui / 2) : SPLEEN_6X12_W) * g_tmul; }
static int font_h(void) { return (g_ui >= 2 ? SPLEEN_12X24_H * (g_ui / 2) : SPLEEN_6X12_H) * g_tmul; }

static unsigned utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
    else if (c >= 0xE0) { while ((*p & 0xC0) == 0x80) p++; c = '?'; }
    *s = (const char *)p;
    return c;
}

static int text_width(const char *s) { int n = 0; while (*s) { utf8_next(&s); n++; } return n * font_w(); }

/* draws text clipped to maxw pixels (0 = no limit); returns the width drawn */
static int text(int x, int y, const char *s, uint32_t col, int maxw) {
    int big = g_ui >= 2, sc = (big ? g_ui / 2 : 1) * g_tmul;
    int gw = big ? SPLEEN_12X24_W : SPLEEN_6X12_W, gh = big ? SPLEEN_12X24_H : SPLEEN_6X12_H;
    int x0 = x;
    while (*s) {
        unsigned c = utf8_next(&s);
        if (c < 32 || c > 255) c = '?';
        if (maxw && x + gw * sc > x0 + maxw) break;
        const uint16_t *g = big ? SPLEEN_12X24[c - 32] : SPLEEN_6X12[c - 32];
        for (int r = 0; r < gh; r++)
            for (int b = 0; b < gw; b++)
                if (g[r] & (0x8000 >> b)) fill(x + b * sc, y + r * sc, sc, sc, col);
        x += gw * sc;
    }
    return x - x0;
}

static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t w) {   /* w in 0..256 */
    uint32_t rb = (((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8) & 0xff00ff;
    uint32_t g = (((a & 0x00ff00) * (256 - w) + (b & 0x00ff00) * w) >> 8) & 0x00ff00;
    return rb | g;
}

/* bilinear scaling of src (sw x sh) into the framebuffer rectangle (dx, dy, dw, dh) */
static void blit_scaled(const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh) {
    static int *xi, *xf, cap;
    if (dw > cap) { cap = dw; xi = xrealloc(xi, sizeof(int) * cap); xf = xrealloc(xf, sizeof(int) * cap); }
    for (int x = 0; x < dw; x++) {
        double s = (x + 0.5) * sw / dw - 0.5;
        if (s < 0) s = 0;
        int i = (int)s;
        if (i > sw - 1) i = sw - 1;
        xi[x] = i; xf[x] = (int)((s - i) * 256);
    }
    for (int y = 0; y < dh; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= g_wh) continue;
        double s = (y + 0.5) * sh / dh - 0.5;
        if (s < 0) s = 0;
        int j = (int)s;
        if (j > sh - 1) j = sh - 1;
        int j1 = j + 1 < sh ? j + 1 : j, fy = (int)((s - j) * 256);
        const uint32_t *r0 = src + (size_t)j * sw, *r1 = src + (size_t)j1 * sw;
        uint32_t *o = g_fb + (size_t)ty * g_ww;
        if (dw == sw && dh == sh) { memcpy(o + dx, r0, sizeof(uint32_t) * sw); continue; }
        for (int x = 0; x < dw; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= g_ww) continue;
            int i = xi[x], i1 = i + 1 < sw ? i + 1 : i;
            o[tx] = lerp_px(lerp_px(r0[i], r0[i1], xf[x]), lerp_px(r1[i], r1[i1], xf[x]), fy);
        }
    }
}

/* ------------------------------------------------------------------ panels (F1 help, Ctrl+I information) */

/* a panel line: cells at given character columns; the first line is the title */
#define MAXCELLS 7
typedef struct { const char *cell[MAXCELLS]; int col[MAXCELLS]; uint32_t colour; } Line;

static void draw_panel(const Line *L, int n) {
    int cw = font_w(), lh = font_h() + 4 * g_ui, lw = 0;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < MAXCELLS && L[i].cell[c]; c++) {
            int w = L[i].col[c] * cw + text_width(L[i].cell[c]);
            if (w > lw) lw = w;
        }
    int pad = 16 * g_ui, pw = lw + 2 * pad, ph = n * lh + 2 * pad;
    int avail = g_wh - (NFR ? BAR_H : 0);           /* no bar while nothing is open */
    int px = (g_ww - pw) / 2, py = (avail - ph) / 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    shade(px, py, pw, ph);
    for (int i = 0; i < n; i++)
        for (int c = 0; c < MAXCELLS && L[i].cell[c]; c++)
            text(px + pad + L[i].col[c] * cw, py + pad + i * lh, L[i].cell[c],
                 i == 0 ? COL_MARK : (L[i].colour ? L[i].colour : COL_ICON), g_ww - px - pad - L[i].col[c] * cw);
}

static const char *HELP[][2] = {
    {"Keyboard shortcuts", 0},
    {"", 0},
    {"Space or K", "play / pause"},
    {"Left / Right", "previous / next frame (also the mouse wheel)"},
    {"Drag on the picture", "move through the frames as on the timeline (pauses)"},
    {"Alt + drag on the picture", "frame ruler: frames, keyframes, frame and offset"},
    {"Home / End", "go to the in / out point"},
    {"Ctrl + Left / Right", "play only the previous / next clip"},
    {"Ctrl + Shift + Left / Right", "extend the selection by one clip to the left / right"},
    {"Shift + Left / Right", "previous / next keyframe (a drawing changes)"},
    {"Shift (held)", "show the keyframes; dragging on the timeline snaps to them"},
    {"I or [", "set the in point at the current frame"},
    {"O or ]", "set the out point at the current frame"},
    {"\\", "reset in / out: play everything"},
    {"H", "show / hide the hidden clips"},
    {"C", "current clip: whole canvas, outside the camera darkened / camera view"},
    {"L", "luminance: show the pictures in grey levels / in colour"},
    {"M", "sound on / off (the clips' sound tracks, as TVPaint exports them)"},
    {"F or F11", "fullscreen (or double-click the picture)"},
    {"Ctrl + I", "file information (resolution, frame rate, clips...)"},
    {"Ctrl + C", "copy the current frame to the clipboard (preview resolution)"},
    {"Esc", "close this panel / leave fullscreen"},
    {"Q", "quit"},
    {"F1 or ?", "show / hide this help"},
    {"", 0},
    {"Drop .tvpp files on the window: top half replaces the timeline,", 0},
    {"  bottom half appends them to it.", 0},
    {"Bottom left: preview resolution (half or full). Bottom right, before fullscreen:", 0},
    {"  the sound button: volume, scrub sound (each frame shown while paused or", 0},
    {"  scrubbing plays its own sound) and mute; the mouse wheel over it sets the", 0},
    {"  volume. All are remembered.", 0},
    {"Timeline: one section per clip (click a clip name to play only that clip),", 0},
    {"  light grey = frames in memory, orange brackets = in / out points,", 0},
    {"  purple names = clips hidden in TVPaint, yellow dots (Shift) = keyframes.", 0},
    {"Counter: frame in the current clip (from 0, as in the project settings:", 0},
    {"  the TVPaint timeline shows +1), then position / total frames.", 0},
    {"Red play / pause button: playback has caught up with loading and is waiting.", 0},
};

static void draw_help(void) {
    int n = (int)(sizeof HELP / sizeof HELP[0]);
    Line L[40];
    memset(L, 0, sizeof L);
    for (int i = 0; i < n; i++) { L[i].cell[0] = HELP[i][0]; L[i].cell[1] = HELP[i][1]; L[i].col[1] = 29; }
    draw_panel(L, n);
}

static char *fmt(const char *f, ...) {   /* small strings for one panel, freed on the next draw */
    static char *pool[400]; static int np;
    if (!f) { for (int i = 0; i < np; i++) free(pool[i]); np = 0; return NULL; }
    va_list ap; va_start(ap, f);
    char b[1100]; vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    static char empty[1];
    if (np == 400) return empty;
    return pool[np++] = strdup(b);
}

static const char *seconds(int frames, double fps) { return fmt("%.2f s", frames / fps); }

static void draw_info(void) {
    Line L[90];
    int n = 0;
    memset(L, 0, sizeof L);
    fmt(NULL);
#define ROW(k, v) (L[n].cell[0] = (k), L[n].cell[1] = (v), L[n].col[1] = 14, n++)
    Section *cs = sec(section_of(g_cur));
    const Project *P = &cs->F->P;
    L[n++].cell[0] = "File information";
    L[n++].cell[0] = "";
    if (NFILES > 1) ROW("Files", fmt("%d open; below, the file of the current clip", NFILES));
    ROW("File", cs->F->base);
    ROW("Canvas", fmt("%d x %d", P->width, P->height));
    ROW("Camera", fmt("%d x %d, %g fps", P->out_w, P->out_h, P->out_fps));
    ROW("Frame rate", fmt("%g fps", P->fps));
    if (fabs(P->out_fps - P->fps) > 1e-3) {
        L[n].colour = COL_WAIT;
        ROW("", fmt("camera at %g fps: TVPaint resamples when rendering, played here at %g fps", P->out_fps, P->fps));
    }
    ROW("Author", fmt("%s, modified %s", *P->author ? P->author : "?", *P->modified ? P->modified : "?"));
    lock(); int nl = loaded_total(); size_t bytes = 0;
    for (int k = 0; k < NSEC; k++) bytes += (size_t)sec(k)->nloaded * sec(k)->w * sec(k)->h * 4;
    unlock();
    double dur = 0; for (int k = 0; k < NSEC; k++) dur += sec(k)->n / sec(k)->fps;
    ROW("Playback", fmt("%d clip(s), %d frames, %.2f s", NSEC, NFR, dur));
    ROW("Preview", fmt("%d x %d (%g %%%s), %d / %d frames in memory, %.0f MB", cs->w, cs->h, g_scale * 100,
                       cs->R.canvas ? ", whole canvas" : "", nl, NFR, (double)bytes / (1 << 20)));
    L[n++].cell[0] = "";
    static const int cols[MAXCELLS] = {0, 5, 27, 42, 53, 64, 75};
    const char *head[] = {"#", "Clip", "Frames", "Count", "Duration", "Marks", "Layers"};
    for (int c = 0; c < MAXCELLS; c++) { L[n].cell[c] = head[c]; L[n].col[c] = cols[c]; }
    L[n++].colour = COL_DIM;
    for (int k = 0; k < NALL && n < 80; k++) {
        Section *s = SECS[k];
        Clip *C = s->C;
        const char *marks = C->mark_in >= 0 || C->mark_out >= 0
            ? fmt("%d-%d", C->mark_in >= 0 ? C->mark_in : 0, C->mark_out >= 0 ? C->mark_out : C->frames - 1) : "-";
        int vis = 0; for (int l = 0; l < C->nlayers; l++) vis += C->layers[l].visible;
        int fi = 0; while (fi < NFILES && FILES[fi] != s->F) fi++;
        const char *v[MAXCELLS] = {NFILES > 1 ? fmt("%d.%d", fi + 1, s->number) : fmt("%d", s->number),
                                   fmt("%s%s", C->name, C->hidden ? " (hidden)" : ""),
                                   fmt("%d-%d", s->f0, s->f1), fmt("%d", s->n), seconds(s->n, s->fps), marks,
                                   fmt("%d (%d visible)", C->nlayers, vis)};
        for (int c = 0; c < MAXCELLS; c++) { L[n].cell[c] = v[c]; L[n].col[c] = cols[c]; }
        L[n++].colour = s == cs ? COL_PLAYED : (!s->active ? COL_DIM : (C->hidden ? COL_HIDDEN : 0));
    }
    Clip *C = cs->C;
    L[n++].cell[0] = "";
    L[n].colour = COL_DIM; L[n++].cell[0] = fmt("Current clip: %s", C->name);
    if (C->npts)
        ROW("Camera", fmt("%d point(s), %s profile%s", C->npts,
                          C->prof_type == 1 ? "linear" : (C->prof_type == 2 ? "polynomial" : "curve"),
                          C->blur > 0 ? fmt(", motion blur %g", C->blur) : ""));
    else
        ROW("Camera", "none (centred, zoom 1)");
    ROW("Background", fmt("#%02X%02X%02X%s", (int)(cs->bg[0] * 255 + 0.5), (int)(cs->bg[1] * 255 + 0.5),
                          (int)(cs->bg[2] * 255 + 0.5), C->has_bg && memcmp(C->bg1, C->bg2, 3) ? " (checkerboard: first colour)" : ""));
    int unsup = 0;
    for (int l = 0; l < C->nlayers; l++) {
        int b = C->layers[l].blend;
        if (C->layers[l].visible && !(b == 0 || b == 1 || b == 2 || b == 7 || b == 9 || b == 10 || b == 11 || b == 12 || b == 15 || b == 17 || b == 25 || b == 26)) unsup++;
    }
    if (unsup) { L[n].colour = COL_WAIT; ROW("Warning", fmt("%d layer(s) with a blending mode played as Color", unsup)); }
    {
        const Sound *S = &C->sound, *PS = &P->sound;   /* the clip's tracks, then the project's */
        int missing = 0, muted = 0;
        for (int k = 0; k < S->n; k++) { muted += S->t[k].mute; missing += !S->t[k].mute && !S->t[k].found; }
        for (int k = 0; k < PS->n; k++) { muted += PS->t[k].mute; missing += !PS->t[k].mute && !PS->t[k].found; }
        const char *state = cs->mix ? (g_snd_on ? (g_snd_dev < 0 ? ", no sound output" : ", played") : ", off (M)") : ", silent";
        if (!S->n && !PS->n) ROW("Sound", "no track");
        else ROW("Sound", fmt("%s%s%s%s%s%s", S->n ? fmt("%d track(s)", S->n) : "", S->n && PS->n ? " + " : "",
                              PS->n ? fmt("%d project track(s)", PS->n) : "", muted ? fmt(", %d muted", muted) : "",
                              missing ? fmt(", %d file(s) not found", missing) : "", state));
        if (PS->n) { L[n].colour = COL_DIM; ROW("", fmt("project tracks: the clip starts at frame %d of the project", clip_start(P, C))); }
    }
    L[n++].cell[0] = "";
    L[n].colour = COL_DIM; L[n++].cell[0] = "Frames counted from 0, as in the project settings (the TVPaint timeline shows +1).";
#undef ROW
    draw_panel(L, n);
}

/* ------------------------------------------------------------------ player */

static int g_playing = 1, g_waiting, g_dragging, g_resume, g_bar_on = 1, g_dirty = 1;
static int g_shift;                     /* Shift held: keyframes shown, the playhead snaps to them */
static int g_vol_drag;                  /* dragging the volume slider */
static int g_pic_drag, g_pic_x, g_pic_cur, g_pic_moved;   /* drag on the picture: start x and frame */
static int g_ruler, g_ruler_x, g_ruler_y, g_ruler_cur;    /* Alt + drag on the picture: frame ruler at the click */
#define RULER_STEP (12 * g_ui)                             /* pixels per frame on the ruler */
static int g_panel;                     /* 0 none, 1 help (F1), 2 information (Ctrl+I) */
static int g_menu;                      /* resolution menu open */
static int g_vmenu;                     /* sound panel open (volume slider, mute) */
static double g_next_t, g_last_move;
static char g_toast[300];               /* short message above the bar, e.g. after H */
static double g_toast_until;
static char g_title[1200];

/* fullscreen: playback waiting for frames brings the bar back (with its red button and the loading),
 * until 2 seconds after it plays smoothly again; the cursor stays hidden */
static double g_last_wait = -1e9;
static int bar_visible(void) {
    return NFR && (!g_fullscreen || g_bar_on || g_panel || g_menu || g_vmenu || g_shift || now() - g_last_wait < 2.0);
}

static void toast(const char *m) { snprintf(g_toast, sizeof g_toast, "%s", m); g_toast_until = now() + 3.0; g_dirty = 1; }

/* the file of the current frame: with several files, the title follows the playback */
static void update_title(void) {
    if (!NFILES) snprintf(g_title, sizeof g_title, "opentvp-play");
    else if (NFILES == 1) snprintf(g_title, sizeof g_title, "%s — opentvp-play", FILES[0]->base);
    else snprintf(g_title, sizeof g_title, "%s + %d more — opentvp-play", NFR ? sec(section_of(g_cur))->F->base : FILES[0]->base, NFILES - 1);
}

/* window: the first clip at half size (at least 640 wide; 960 x 540 with no file), within 85 % of the screen */
static void fit_window(void) {
    int sw, sh;
    plat_screen(&sw, &sh);
    double ww = 960, wh = 540;
    if (NALL) {
        const Project *P = &SECS[0]->F->P;
        ww = (SECS[0]->R.canvas ? P->width : P->out_w) * 0.5; wh = (SECS[0]->R.canvas ? P->height : P->out_h) * 0.5;
        if (ww < 640) { wh *= 640 / ww; ww = 640; }
    }
    ww *= g_ui; wh *= g_ui;
    double fit = 1;
    if (ww > sw * 0.85) fit = sw * 0.85 / ww;
    if (wh * fit > sh * 0.85 - BAR_H) fit = (sh * 0.85 - BAR_H) / wh;
    plat_resize((int)(ww * fit), (int)(wh * fit) + (NALL ? BAR_H : 0));
}

/* counter: frame in the current clip (project numbering, from 0), then position / total (from 1) */
static void counter_text(char *b, size_t n, int i) {
    const Section *s = sec(section_of(i));
    snprintf(b, n, "%d   %d / %d", s->f0 + i - s->start, i + 1, NFR);
}
static int counter_w(void) {
    int fmax = 0, total = 0;
    for (int k = 0; k < NALL; k++) { if (SECS[k]->f1 > fmax) fmax = SECS[k]->f1; total += SECS[k]->n; }
    char b[64]; snprintf(b, sizeof b, "%d   %d / %d", fmax, total, total);
    return text_width(b);
}

/* bar geometry: play button, resolution button, track, counter, fullscreen button */
static int bar_top(void) { return g_wh - BAR_H; }
static int track_cy(void) { return bar_top() + 42 * g_ui; }
static void res_button(int *x, int *w) { *x = 46 * g_ui; *w = text_width("100%") + 20 * g_ui; }
/* sound button (speaker and arrow), on the right: between the frame counters and the fullscreen button */
static void snd_button(int *x, int *w) { *w = 40 * g_ui; *x = g_ww - 52 * g_ui - *w; }
/* sound panel above it: "Volume", a vertical slider from sy0 (100 %) to sy1 (0 %), then the
 * "Scrub sound" row at sr and the "Mute (M)" row at my (both mh high) */
static void vmenu_rect(int *x, int *y, int *w, int *h, int *sy0, int *sy1, int *sr, int *my) {
    int u = g_ui, bx, bw; snd_button(&bx, &bw);
    int th = font_h() + 12 * u, sh = 110 * u, mh = font_h() + 14 * u;
    *w = text_width("Scrub sound") + 48 * u;
    *h = th + sh + 2 * mh;
    *x = bx + bw / 2 - *w / 2; if (*x + *w > g_ww - 4 * u) *x = g_ww - 4 * u - *w; if (*x < 4 * u) *x = 4 * u;
    *y = bar_top() - *h - 4 * u;
    *sy0 = *y + th + 8 * u; *sy1 = *y + th + sh - 8 * u; *sr = *y + th + sh; *my = *sr + mh;
}
static void track_x(int *x0, int *x1) {
    int rx, rw, bx, bw; res_button(&rx, &rw); snd_button(&bx, &bw);
    *x0 = rx + rw + 14 * g_ui; *x1 = bx - 20 * g_ui - counter_w();
    if (*x1 < *x0 + 10) *x1 = *x0 + 10;
}
static int pos_x(int i) { int x0, x1; track_x(&x0, &x1); return x0 + (int)((double)(x1 - x0) * i / NFR); }

/* resolution menu, opened above its button */
static const double MENU_SCALE[2] = {1.0, 0.5};
static const char *MENU_TEXT[2] = {"Full resolution (100%)", "Half resolution (50%)"};
static void menu_rect(int *x, int *y, int *w, int *ih) {
    int rx, rw; res_button(&rx, &rw);
    *ih = font_h() + 12 * g_ui;
    *w = text_width(MENU_TEXT[0]) + 40 * g_ui;
    *x = rx; *y = bar_top() - 2 * *ih - 4 * g_ui;
}

/* speaker icon from x, centred on cy: waves for the level, a red cross when the sound is off */
static void draw_speaker(int x, int cy, uint32_t c) {
    int u = g_ui;
    fill(x, cy - 3 * u, 4 * u, 6 * u, c);
    for (int i = 0; i < 6 * u; i++) fill(x + 4 * u + i, cy - 3 * u - i, 1, 6 * u + 2 * i, c);
    if (!g_snd_on || g_snd_vol <= 0)
        for (int i = 0; i < 7 * u; i++) { fill(x + 12 * u + i, cy - 4 * u + i, 2 * u, u, COL_WAIT); fill(x + 12 * u + i, cy + 3 * u - i, 2 * u, u, COL_WAIT); }
    else
        for (int k = 0; k < 2; k++) if (g_snd_vol > k * 0.5f) fill(x + 12 * u + 4 * k * u, cy - (4 + 2 * k) * u, 2 * u, (8 + 4 * k) * u, c);
}

static void draw_bar(void) {
    int u = g_ui, y = bar_top(), cy = track_cy();
    fill(0, y, g_ww, BAR_H, COL_BAR);
    /* play / pause; red while waiting for frames that are still loading */
    uint32_t pc = g_playing && g_waiting ? COL_WAIT : COL_ICON;
    if (g_playing && !g_dragging) { fill(16 * u, cy - 9 * u, 6 * u, 18 * u, pc); fill(27 * u, cy - 9 * u, 6 * u, 18 * u, pc); }
    else for (int i = 0; i < 17 * u; i++) { int h = (17 * u - 1 - i) / 2; fill(17 * u + i, cy - h, 1, 2 * h + 1, pc); }
    /* resolution button: percentage and a small arrow */
    int rx, rw; res_button(&rx, &rw);
    char pct[16]; snprintf(pct, sizeof pct, "%g%%", g_scale * 100);
    fill(rx, cy - 11 * u, rw, 22 * u, g_menu ? 0x3a3a3a : COL_MENU);
    text(rx + 6 * u, cy - font_h() / 2, pct, COL_ICON, 0);
    for (int i = 0; i < 4 * u; i++) fill(rx + rw - 12 * u + i, cy - 2 * u + i, 8 * u - 2 * i, 1, COL_DIM);
    /* sound button: speaker (crossed out when off, dim without an output device) and a small arrow */
    {
        int bx, bw; snd_button(&bx, &bw);
        fill(bx, cy - 11 * u, bw, 22 * u, g_vmenu ? 0x3a3a3a : COL_MENU);
        draw_speaker(bx + 5 * u, cy, g_snd_dev < 0 ? COL_DIM : COL_ICON);
        for (int i = 0; i < 4 * u; i++) fill(bx + bw - 10 * u + i, cy - 2 * u + i, 8 * u - 2 * i, 1, COL_DIM);
    }

    int x0, x1; track_x(&x0, &x1);
    int th = 8 * u, ty = cy - th / 2, cur_sec = section_of(g_cur);
    /* track: frames in memory, played part of the selection, outside the selection darker */
    lock();
    for (int x = x0; x < x1; x++) {
        int i = (int)((double)(x - x0) * NFR / (x1 - x0));
        uint32_t c = *slot(i) ? COL_LOADED : COL_TRACK;
        if (i >= g_in && i <= g_cur && g_cur <= g_out) c = COL_PLAYED;
        if (i < g_in || i > g_out) c = (c >> 1) & 0x7f7f7f;
        fill(x, ty, 1, th, c);
    }
    unlock();
    /* Shift: one dot under the track per keyframe (one per pixel column when they are dense) */
    if (g_shift) {
        int last = -1;
        for (int i = 0; i < NFR; i++) {
            if (!key_at(i)) continue;
            int kx = pos_x(i) + (pos_x(i + 1) - pos_x(i)) / 2;
            if (kx == last) continue;
            fill(kx - u, ty + th + 2 * u, 3 * u, 3 * u, COL_KEY);
            last = kx;
        }
    }
    /* clip sections: separators (stronger between files) and names with their frame count */
    int ly = y + 8 * u;
    for (int k = 0; k < NSEC; k++) {
        Section *s = sec(k);
        int sx = pos_x(s->start), ex = pos_x(s->start + s->n);
        int new_file = k == 0 || sec(k - 1)->F != s->F;
        if (k > 0) fill(sx, ly - 2 * u, new_file && NFILES > 1 ? 2 * u : u, cy + th - ly + 4 * u, new_file && NFILES > 1 ? COL_DIM : COL_SEP);
        char lab[1200];
        const char *pre = NFILES > 1 && new_file ? s->F->base : "";
        snprintf(lab, sizeof lab, "%s%s%s (%s%s%d frames)", pre, *pre ? ": " : "", s->C->name, s->C->hidden ? "hidden, " : "",
                 s->R.canvas ? "canvas, " : "", s->n);
        if (text_width(lab) > ex - sx - 6 * u) snprintf(lab, sizeof lab, "%s%s%s (%d)", pre, *pre ? ": " : "", s->C->name, s->n);
        uint32_t c = s->C->hidden ? COL_HIDDEN : COL_DIM;
        if (k == cur_sec) c = s->C->hidden ? 0xd8c4f0 : COL_ICON;
        text(sx + 4 * u, ly, lab, c, ex - sx - 6 * u);
    }
    /* in / out brackets when the selection is not everything */
    if (g_in > 0 || g_out < NFR - 1) {
        int ix = pos_x(g_in), ox = pos_x(g_out + 1);
        fill(ix - u, cy - 10 * u, 2 * u, 20 * u, COL_MARK); fill(ix - u, cy - 10 * u, 6 * u, 2 * u, COL_MARK); fill(ix - u, cy + 8 * u, 6 * u, 2 * u, COL_MARK);
        fill(ox - u, cy - 10 * u, 2 * u, 20 * u, COL_MARK); fill(ox - 5 * u, cy - 10 * u, 6 * u, 2 * u, COL_MARK); fill(ox - 5 * u, cy + 8 * u, 6 * u, 2 * u, COL_MARK);
    }
    /* playhead */
    int px = pos_x(g_cur) + (pos_x(g_cur + 1) - pos_x(g_cur)) / 2;
    fill(px - u, cy - 8 * u, 3 * u, 16 * u, COL_ICON);
    /* counter */
    char b[64]; counter_text(b, sizeof b, g_cur);
    { int bx, bw; snd_button(&bx, &bw); text(bx - 12 * u - counter_w(), cy - font_h() / 2, b, COL_ICON, 0); }
    /* fullscreen button: four corners */
    int fx = g_ww - 36 * u, fy = cy - 9 * u, a = 6 * u, t = 2 * u, e = 18 * u;
    uint32_t c = COL_ICON;
    fill(fx, fy, a, t, c); fill(fx, fy, t, a, c);
    fill(fx + e - a, fy, a, t, c); fill(fx + e - t, fy, t, a, c);
    fill(fx, fy + e - t, a, t, c); fill(fx, fy + e - a, t, a, c);
    fill(fx + e - a, fy + e - t, a, t, c); fill(fx + e - t, fy + e - a, t, a, c);
}

static void draw_menu(void) {
    int x, y, w, ih; menu_rect(&x, &y, &w, &ih);
    fill(x, y, w, 2 * ih, COL_MENU);
    frame_rect(x, y, w, 2 * ih, g_ui, COL_SEP);
    for (int i = 0; i < 2; i++) {
        int on = fabs(g_scale - MENU_SCALE[i]) < 1e-6;
        if (on) fill(x + 8 * g_ui, y + i * ih + ih / 2 - 3 * g_ui, 6 * g_ui, 6 * g_ui, COL_PLAYED);
        text(x + 22 * g_ui, y + i * ih + 6 * g_ui, MENU_TEXT[i], on ? COL_ICON : COL_DIM, 0);
    }
}

static void draw_vmenu(void) {
    int u = g_ui, x, y, w, h, sy0, sy1, sr, my; vmenu_rect(&x, &y, &w, &h, &sy0, &sy1, &sr, &my);
    int off = !g_snd_on || g_snd_vol <= 0, cx = x + w / 2;
    fill(x, y, w, h, COL_MENU);
    frame_rect(x, y, w, h, u, COL_SEP);
    text(cx - text_width("Volume") / 2, y + 6 * u, "Volume", COL_DIM, 0);
    int ky = sy1 - (int)(g_snd_vol * (sy1 - sy0) + 0.5f);
    fill(cx - u, sy0, 2 * u, sy1 - sy0, COL_TRACK);
    fill(cx - u, ky, 2 * u, sy1 - ky, off ? COL_DIM : COL_PLAYED);
    fill(cx - 6 * u, ky - 2 * u, 12 * u, 4 * u, off ? COL_DIM : COL_ICON);
    int mh = my - sr;
    fill(x + u, sr, w - 2 * u, u, COL_SEP);
    int qy = sr + mh / 2;                       /* scrub: a checkbox */
    frame_rect(x + 10 * u, qy - 5 * u, 10 * u, 10 * u, u, g_snd_scrub ? COL_ICON : COL_DIM);
    if (g_snd_scrub) fill(x + 12 * u, qy - 3 * u, 6 * u, 6 * u, COL_PLAYED);
    text(x + 34 * u, qy - font_h() / 2, "Scrub sound", g_snd_scrub ? COL_ICON : COL_DIM, 0);
    fill(x + u, my, w - 2 * u, u, COL_SEP);
    int ry = my + mh / 2;
    draw_speaker(x + 8 * u, ry, !g_snd_on ? COL_ICON : COL_DIM);
    text(x + 34 * u, ry - font_h() / 2, "Mute (M)", !g_snd_on ? COL_ICON : COL_DIM, 0);
}

/* drag over the window: top half replaces the timeline, bottom half appends */
static void draw_drop(void) {
    if (!NFR) {                                  /* nothing open: the whole window opens the files */
        shade(0, 0, g_ww, g_wh);
        frame_rect(4 * g_ui, 4 * g_ui, g_ww - 8 * g_ui, g_wh - 8 * g_ui, 3 * g_ui, COL_PLAYED);
        g_tmul = 2;
        text((g_ww - text_width("Open")) / 2, g_wh / 2 - font_h(), "Open", COL_ICON, 0);
        g_tmul = 1;
        const char *sub = "plays the dropped file(s), one after the other";
        text((g_ww - text_width(sub)) / 2, g_wh / 2 + 6 * g_ui, sub, COL_ICON, 0);
        return;
    }
    int half = g_wh / 2, top = g_drag_y < half;
    for (int i = 0; i < 2; i++) {
        int y = i ? half : 0, h = i ? g_wh - half : half, on = (i == 0) == top;
        shade(0, y, g_ww, h);
        if (on) frame_rect(4 * g_ui, y + 4 * g_ui, g_ww - 8 * g_ui, h - 8 * g_ui, 3 * g_ui, COL_PLAYED);
        const char *t = i ? "Append to timeline" : "Replace current timeline";
        const char *sub = i ? "adds the file(s) after the current timeline"
                            : "closes the open file(s) and clears the memory";
        g_tmul = 2;
        int tw = text_width(t), th = font_h();
        text((g_ww - tw) / 2, y + h / 2 - th, t, on ? COL_ICON : COL_DIM, 0);
        g_tmul = 1;
        text((g_ww - text_width(sub)) / 2, y + h / 2 + 6 * g_ui, sub, on ? COL_ICON : COL_DIM, 0);
    }
}

/* started without a file: waits for one to be dropped */
static void draw_empty(void) {
    int m = 24 * g_ui;
    frame_rect(m, m, g_ww - 2 * m, g_wh - 2 * m, g_ui, COL_SEP);
    const char *t = "Drop a .tvpp file here";
    const char *sub = "several files play one after the other.   F1: keyboard shortcuts";
    g_tmul = 2;
    text((g_ww - text_width(t)) / 2, g_wh / 2 - font_h(), t, COL_ICON, 0);
    g_tmul = 1;
    text((g_ww - text_width(sub)) / 2, g_wh / 2 + 6 * g_ui, sub, COL_DIM, 0);
}

/* Alt + drag on the picture: one tick per frame around the click, keyframes in yellow, clip starts
 * long, the starting frame taller, the current frame in blue with its offset and its frame in the clip */
static void draw_ruler(void) {
    int u = g_ui, by = g_ruler_y;
    g_tmul = 4; int nh = font_h(); g_tmul = 2; int oh = font_h(); g_tmul = 1;
    int top = by - 16 * u - nh - oh, bot = by + 22 * u;      /* room for the offset and the frame number */
    darken(0, top, g_ww, bot - top, 230);                    /* black at 10 % */
#define RX(i) (g_ruler_x + ((i) - g_ruler_cur) * RULER_STEP)
    int a = g_ruler_cur - g_ruler_x / RULER_STEP - 1, b = g_ruler_cur + (g_ww - g_ruler_x) / RULER_STEP + 1;
    if (a < 0) a = 0;
    if (b > NFR - 1) b = NFR - 1;
    if (a > b) return;
    fill(RX(a), by, RX(b) - RX(a) + 1, u, COL_DIM);
    for (int i = a; i <= b; i++) {
        int x = RX(i), k = section_of(i);
        if (i > 0 && sec(k)->start == i) fill(x, by - 28 * u, u, bot - by + 28 * u, COL_ICON);
        else fill(x, by - 10 * u, u, 20 * u, COL_DIM);
        if (key_at(i)) fill(x - u, by - 6 * u, 3 * u, 12 * u, COL_KEY);
    }
    fill(g_ruler_x, by - 16 * u, u, 32 * u, COL_ICON);
    int cx = RX(g_cur);
#undef RX
    fill(cx, top, u, bot - top, COL_PLAYED);
    const Section *s = sec(section_of(g_cur));
    char off[32], num[32];
    snprintf(off, sizeof off, g_cur == g_ruler_cur ? "0" : "%+d", g_cur - g_ruler_cur);
    snprintf(num, sizeof num, "%d", s->f0 + g_cur - s->start);
    g_tmul = 4; int nw = text_width(num); g_tmul = 2;
    int ow = text_width(off), w = nw > ow ? nw : ow;
    int tx = cx + 6 * u + w > g_ww ? cx - 6 * u - w : cx + 6 * u;
    int ny = by - 12 * u - nh, oy = ny - oh;
    text(tx, oy, off, COL_PLAYED, 0);
    g_tmul = 4; text(tx, ny, num, COL_PLAYED, 0);
    g_tmul = 1;
}

static void draw(void) {
    int vh = g_fullscreen || !NFR ? g_wh : g_wh - BAR_H;
    if (vh < 1) vh = 1;
    fill(0, 0, g_ww, g_wh, 0);
    int fw, fh;
    const uint32_t *fr = NFR ? frame_at(g_cur, &fw, &fh) : NULL;
    if (!NFR && !g_dragfile && !g_panel) draw_empty();
    else if (fr) {
        double s = (double)g_ww / fw < (double)vh / fh ? (double)g_ww / fw : (double)vh / fh;
        int dw = (int)(fw * s + 0.5), dh = (int)(fh * s + 0.5);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        blit_scaled(fr, fw, fh, (g_ww - dw) / 2, (vh - dh) / 2, dw, dh);
        if (g_luma) luminance((g_ww - dw) / 2, (vh - dh) / 2, dw, dh);
    } else {
        const char *m = "loading this frame...";
        text((g_ww - text_width(m)) / 2, (vh - font_h()) / 2, m, COL_DIM, 0);
    }
    if (g_ruler && NFR) draw_ruler();
    if (bar_visible()) draw_bar();
    if (g_panel == 1) draw_help();
    if (g_panel == 2) draw_info();
    if (g_menu) draw_menu();
    if (g_vmenu) draw_vmenu();
    if (g_toast[0] && now() < g_toast_until) {
        int w = text_width(g_toast) + 24 * g_ui, h = font_h() + 12 * g_ui, x = (g_ww - w) / 2;
        int y = (bar_visible() ? bar_top() : g_wh) - h - 12 * g_ui;   /* just above the bar, clear of the panels */
        shade(x, y, w, h);
        text(x + 12 * g_ui, y + 6 * g_ui, g_toast, COL_ICON, 0);
    }
    if (g_dragfile) draw_drop();
    plat_present();
}

static int clamp(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

/* Shift is read from the keyboard state, not from key events: no autorepeat, no key lost with the focus */
static void update_shift(void) {
    int sh = NFR && plat_shift();
    if (sh != g_shift) { g_shift = sh; g_dirty = 1; }
}

static void seek(int i) {
    i = clamp(i, 0, NFR - 1);
    if (g_shift) i = nearest_key(i);
    if (i != g_cur) { set_cur(i); g_dirty = 1; }
}

static void seek_x(int x) { int x0, x1; track_x(&x0, &x1); seek((int)((double)(x - x0) * NFR / (x1 - x0))); }

/* drag on the picture: the horizontal movement moves the playhead as much as on the timeline;
 * past a few pixels it pauses playback, and the playhead stays where it is released */
static void picture_drag(int x) {
    int x0, x1; track_x(&x0, &x1);
    if (!g_pic_moved) {
        if (abs(x - g_pic_x) < 4 * g_ui) return;   /* a click, or the first click of a double-click */
        g_pic_moved = 1; g_playing = 0; g_dirty = 1;
    }
    seek(g_pic_cur + (int)floor((double)(x - g_pic_x) * NFR / (x1 - x0) + 0.5));
}

static void go(int i) { g_playing = 0; set_cur(clamp(i, 0, NFR - 1)); g_dirty = 1; }

static void play_from(int i) { set_cur(i); g_playing = 1; g_next_t = now() + 1.0 / sec(section_of(i))->fps; g_dirty = 1; }

static void select_section(int k) {
    k = clamp(k, 0, NSEC - 1);
    set_inout(sec(k)->start, sec(k)->start + sec(k)->n - 1);
    play_from(sec(k)->start);
}

/* H: hidden clips join or leave the sequence; the playhead and in / out stay on the same pictures if they can */
static void toggle_hidden(void) {
    if (!NHIDDEN) { toast("No hidden clip in the open file(s)"); return; }
    lock();
    int full = g_in == 0 && g_out == NFR - 1;
    Section *cs = sec(section_of(g_cur)), *is = sec(section_of(g_in)), *os = sec(section_of(g_out));
    int cl = g_cur - cs->start, il = g_in - is->start, ol = g_out - os->start;
    g_show_hidden = !g_show_hidden;
    rebuild();
    if (!NFR) { g_show_hidden = 1; rebuild(); unlock(); toast("Only hidden clips: nothing else to play"); return; }
    if (cs->active) g_cur = cs->start + cl;
    else {                                       /* its clip was hidden: next shown clip */
        int ci = 0; while (SECS[ci] != cs) ci++;
        g_cur = 0;
        for (int k = 0; k < NSEC; k++) if (ACT[k] > ci) { g_cur = sec(k)->start; break; }
    }
    if (!full && is->active && os->active) { g_in = is->start + il; g_out = os->start + ol; }
    else { g_in = 0; g_out = NFR - 1; }
    unlock();
    char m[100];
    snprintf(m, sizeof m, g_show_hidden ? "Hidden clips shown (%d)" : "Hidden clips not shown (%d)", NHIDDEN);
    toast(m);
    loader_wake();
    g_dirty = 1;
}

/* Ctrl+Shift+Left / Right: the selection grows to the next clip boundary on that side */
static void grow_selection(int dir) {
    int a = g_in, b = g_out;
    if (dir > 0) {
        if (b >= NFR - 1) { toast("The selection already ends with the last clip"); return; }
        int k = section_of(b);
        Section *s = b == sec(k)->start + sec(k)->n - 1 ? sec(k + 1) : sec(k);   /* or the end of a partly selected clip */
        b = s->start + s->n - 1;
    } else {
        if (a <= 0) { toast("The selection already starts with the first clip"); return; }
        int k = section_of(a);
        a = a == sec(k)->start ? sec(k - 1)->start : sec(k)->start;
    }
    set_inout(a, b);
    g_dirty = 1;
}

/* C: the current clip shows the whole canvas, outside the camera darkened, or the camera view again */
/* L: grey levels on display only; the frames in memory stay in colour */
static void toggle_scrub(void) {
    g_snd_scrub = !g_snd_scrub;
    save_scale();
    if (!g_vmenu) toast(g_snd_scrub ? "Scrub sound on" : "Scrub sound off");
    g_dirty = 1;
}

/* level 0..1, by steps of 1 %; setting a level turns the sound on */
static void set_volume(float v, int save) {
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    g_snd_vol = floorf(v * 100 + 0.5f) / 100;
    g_snd_on = 1;
    char m[64]; snprintf(m, sizeof m, "Volume %d %%", (int)(g_snd_vol * 100 + 0.5f));
    if (!g_vmenu) toast(m);
    if (save) save_scale();
    g_dirty = 1;
}

static void toggle_sound(void) {
    g_snd_on = !g_snd_on;
    int any = 0; for (int k = 0; k < NSEC; k++) any |= sec(k)->mix != NULL;
    toast(!g_snd_on ? "Sound off" : (g_snd_dev < 0 ? "Sound on, but there is no sound output"
                                     : (any ? "Sound on" : "Sound on (no clip has sound)")));
}

static void toggle_luma(void) {
    g_luma = !g_luma; g_dirty = 1;
    toast(g_luma ? "Luminance: grey levels" : "Colour");
}

/* Ctrl+C: the current frame as it is in memory (preview resolution, grey levels with L) to the clipboard */
static void copy_frame(void) {
    int w, h;
    const uint32_t *fr = frame_at(g_cur, &w, &h);
    if (!fr) { toast("This frame is not loaded yet: nothing copied"); return; }
    uint8_t *rgb = xmalloc((size_t)w * h * 3);
    for (size_t p = 0; p < (size_t)w * h; p++) {
        uint32_t r = fr[p] >> 16 & 0xff, g = fr[p] >> 8 & 0xff, b = fr[p] & 0xff;
        if (g_luma) r = g = b = (54 * r + 183 * g + 19 * b) >> 8;
        rgb[p * 3] = (uint8_t)r; rgb[p * 3 + 1] = (uint8_t)g; rgb[p * 3 + 2] = (uint8_t)b;
    }
    int ok = plat_copy_image(rgb, w, h);
    free(rgb);
    const Section *s = sec(section_of(g_cur));
    char m[400];
    if (ok) snprintf(m, sizeof m, "Frame %d of %s copied (%d x %d%s)", s->f0 + g_cur - s->start, s->C->name, w, h,
                     g_luma ? ", grey levels" : "");
    else snprintf(m, sizeof m, "Could not copy the frame to the clipboard");
    toast(m);
}

static void toggle_canvas(void) {
    Section *s = sec(section_of(g_cur));
    set_canvas(s, !s->R.canvas);
    char m[300];
    snprintf(m, sizeof m, s->R.canvas ? "%s: whole canvas, outside the camera darkened" : "%s: camera view", s->C->name);
    toast(m);
}

static void choose_scale(double sc) {
    g_menu = 0; g_dirty = 1;
    if (fabs(sc - g_scale) < 1e-6) return;
    apply_scale(sc);
    save_scale();
    const Section *s = sec(section_of(g_cur));
    char m[200]; snprintf(m, sizeof m, "Preview at %g %% (%d x %d): rendering again", sc * 100, s->w, s->h);
    toast(m);
}

/* files dropped on the window (or a failed open): replace or append, keeping what works */
static void handle_drop(void) {
    if (!g_ndrop) return;
    int mode = NFR ? g_drop_mode : DROP_REPLACE, np = g_ndrop, was_empty = !NFR;
    char **paths = g_drop;
    g_drop = NULL; g_ndrop = 0;
    File **nf = xcalloc(sizeof(File *) * np);
    Section ***ls = xcalloc(sizeof(Section **) * np);
    int *ln = xcalloc(sizeof(int) * np), ok = 0, nerr = 0;
    char err[700] = "", e[700];
    for (int i = 0; i < np; i++) {
        nf[i] = load_file(paths[i], 0, -1, -1, &ls[i], &ln[i], e, sizeof e);
        if (nf[i]) ok++;
        else { if (!nerr++) snprintf(err, sizeof err, "%s", e); fprintf(stderr, "  ⚠ %s\n", e); }
    }
    if (ok) {
        int full = g_in == 0 && g_out == NFR - 1;
        if (mode == DROP_REPLACE) close_all();
        for (int i = 0; i < np; i++) if (nf[i]) commit(nf[i], ls[i], ln[i]);
        lock();
        if (mode == DROP_REPLACE) { g_cur = 0; g_in = 0; g_out = NFR - 1; }
        else if (full) g_out = NFR - 1;
        unlock();
        if (mode == DROP_REPLACE) { g_playing = 1; g_next_t = now(); }
        if (was_empty && !g_fullscreen) fit_window();
        update_title();
        loader_wake();
        char m[900];
        snprintf(m, sizeof m, "%s %d file(s)%s%s", mode == DROP_REPLACE ? "Opened" : "Appended", ok,
                 nerr ? ". Not opened: " : "", nerr ? err : "");
        toast(m);
        printf("%s:", mode == DROP_REPLACE ? "replaced by" : "appended");
        for (int i = 0; i < np; i++) if (nf[i]) printf(" %s", nf[i]->base);
        printf("\n"); fflush(stdout);
    } else {
        toast(err);
    }
    for (int i = 0; i < np; i++) free(paths[i]);
    free(paths); free(nf); free(ls); free(ln);
    g_dirty = 1;
}

static void toggle_play(void) { g_playing = !g_playing; g_next_t = now() + 1.0 / sec(section_of(g_cur))->fps; g_dirty = 1; }
static void toggle_fullscreen(void) { plat_fullscreen(!g_fullscreen); g_bar_on = 1; plat_cursor(1); g_last_move = now(); g_dirty = 1; }
static void toggle_panel(int p) { g_panel = g_panel == p ? 0 : p; g_dirty = 1; }

static void key(int k) {
    if (!NFR && k != K_F && k != K_HELP && k != K_ESC && k != K_Q) return;   /* nothing open yet */
    switch (k) {
    case K_SPACE: toggle_play(); break;
    case K_LEFT: go(g_cur - 1); break;
    case K_RIGHT: go(g_cur + 1); break;
    case K_HOME: go(g_in); break;
    case K_END: go(g_out); break;
    case K_PREV_CLIP: select_section(section_of(g_cur) - 1); break;
    case K_NEXT_CLIP: select_section(section_of(g_cur) + 1); break;
    case K_IN: set_inout(g_cur, g_out < g_cur ? NFR - 1 : g_out); g_dirty = 1; break;
    case K_OUT: set_inout(g_in > g_cur ? 0 : g_in, g_cur); g_dirty = 1; break;
    case K_RESET: set_inout(0, NFR - 1); g_dirty = 1; break;
    case K_GROW_PREV: grow_selection(-1); break;
    case K_GROW_NEXT: grow_selection(1); break;
    case K_PREV_KEY: case K_NEXT_KEY: {
        int j = next_key(g_cur, k == K_NEXT_KEY ? 1 : -1);
        if (j >= 0) go(j); else toast(k == K_NEXT_KEY ? "No keyframe after this frame" : "No keyframe before this frame");
        break;
    }
    case K_HIDDEN: toggle_hidden(); break;
    case K_CANVAS: toggle_canvas(); break;
    case K_LUMA: toggle_luma(); break;
    case K_MUTE: toggle_sound(); break;
    case K_COPY: copy_frame(); break;
    case K_F: toggle_fullscreen(); break;
    case K_HELP: toggle_panel(1); break;
    case K_INFO: toggle_panel(2); break;
    case K_ESC:
        if (g_menu || g_vmenu) { g_menu = g_vmenu = 0; g_dirty = 1; }
        else if (g_panel) { g_panel = 0; g_dirty = 1; }
        else if (g_fullscreen) toggle_fullscreen();
        break;
    case K_Q: exit(0);
    }
}

static void handle(const Event *e) {
    int u = g_ui, in_bar = bar_visible() && e->y >= bar_top();
    switch (e->type) {
    case EV_QUIT: exit(0);
    case EV_REDRAW: g_dirty = 1; break;
    case EV_KEY: key(e->key); break;
    case EV_WHEEL: {        /* over the sound button or its panel: level; elsewhere: frames */
        int bx, bw, x, y, w, h, sy0, sy1, sr, my; snd_button(&bx, &bw); vmenu_rect(&x, &y, &w, &h, &sy0, &sy1, &sr, &my);
        if ((in_bar && e->x >= bx && e->x < bx + bw) || (g_vmenu && e->x >= x && e->x < x + w && e->y >= y && e->y < y + h))
            set_volume(g_snd_vol - 0.05f * e->key, 1);
        else if (NFR) go(g_cur + e->key);
        break;
    }
    case EV_DOUBLE:
        if (!in_bar && !g_menu && !g_vmenu && !plat_alt()) { toggle_fullscreen(); break; }
        /* fall through: a quick second click on the controls is a normal click */
    case EV_DOWN: {
        if (g_menu) {                            /* open menu: a choice, or a click elsewhere closes it */
            int x, y, w, ih; menu_rect(&x, &y, &w, &ih);
            if (e->x >= x && e->x < x + w && e->y >= y && e->y < y + 2 * ih) choose_scale(MENU_SCALE[(e->y - y) / ih]);
            else { g_menu = 0; g_dirty = 1; }
            break;
        }
        if (g_vmenu) {                           /* sound panel: slider, mute row; a click elsewhere closes it */
            int x, y, w, h, sy0, sy1, sr, my, bx, bw; vmenu_rect(&x, &y, &w, &h, &sy0, &sy1, &sr, &my); snd_button(&bx, &bw);
            int inside = e->x >= x && e->x < x + w && e->y >= y && e->y < y + h;
            if (inside && e->y >= my) toggle_sound();
            else if (inside && e->y >= sr) toggle_scrub();
            else if (inside && e->y >= sy0 - 8 * u && e->y <= sy1 + 8 * u) { g_vol_drag = 1; set_volume((float)(sy1 - e->y) / (sy1 - sy0), 0); }
            else if (!inside) { g_vmenu = 0; g_dirty = 1; if (in_bar && e->x >= bx && e->x < bx + bw) break; }
            if (inside) break;
        }
        if (!in_bar) {
            if (!NFR || g_dragfile) break;
            if (plat_alt()) { g_ruler = 1; g_ruler_x = e->x; g_ruler_y = e->y; g_ruler_cur = g_cur; g_playing = 0; g_dirty = 1; }
            else { g_pic_drag = 1; g_pic_moved = 0; g_pic_x = e->x; g_pic_cur = g_cur; }
            break;
        }
        int x0, x1, rx, rw, bx, bw; track_x(&x0, &x1); res_button(&rx, &rw); snd_button(&bx, &bw);
        if (e->x < 44 * u) toggle_play();
        else if (e->x >= rx && e->x < rx + rw) { g_menu = 1; g_dirty = 1; }
        else if (e->x >= bx && e->x < bx + bw) { g_vmenu = 1; g_dirty = 1; }
        else if (e->x >= g_ww - 44 * u) toggle_fullscreen();
        else if (e->x >= x0 && e->x < x1 && e->y < track_cy() - 12 * u) {
            /* clip name row: play only that clip */
            for (int k = NSEC - 1; k >= 0; k--) if (e->x >= pos_x(sec(k)->start)) { select_section(k); break; }
        } else if (e->x >= x0 - 8 * u && e->x <= x1 + 8 * u) {
            g_dragging = 1; g_resume = g_playing; g_playing = 0; update_shift(); seek_x(e->x); g_dirty = 1;
        }
        break;
    }
    case EV_MOVE: {
        /* Windows reports a "move" when the cursor is hidden or the window changes under it */
        static int lx = -1, ly = -1;
        if (e->x == lx && e->y == ly) break;
        lx = e->x; ly = e->y;
        g_last_move = now();
        if (!g_bar_on) { g_bar_on = 1; plat_cursor(1); g_dirty = 1; }
        if (g_vol_drag) { int x, y, w, h, sy0, sy1, sr, my; vmenu_rect(&x, &y, &w, &h, &sy0, &sy1, &sr, &my); set_volume((float)(sy1 - e->y) / (sy1 - sy0), 0); }
        else if (g_dragging) { update_shift(); seek_x(e->x); }
        else if (g_pic_drag) { update_shift(); picture_drag(e->x); }
        else if (g_ruler) {
            update_shift();
            seek(g_ruler_cur + (int)floor((double)(e->x - g_ruler_x) / RULER_STEP + 0.5));
            g_dirty = 1;
        }
        break;
    }
    case EV_UP:
        if (g_vol_drag) { g_vol_drag = 0; save_scale(); }
        g_pic_drag = 0;
        if (g_ruler) { g_ruler = 0; g_dirty = 1; }
        if (g_dragging) { g_dragging = 0; g_playing = g_resume; g_next_t = now() + 1.0 / sec(section_of(g_cur))->fps; g_dirty = 1; }
        break;
    }
}

/* one playback tick: next frame of the in/out selection, looping; waits for frames not loaded yet */
static void advance(double t) {
    if (!NFR || !g_playing || g_dragging || t < g_next_t) return;
    int nx = (g_cur >= g_out || g_cur < g_in) ? g_in : g_cur + 1;   /* outside the selection: back to the in point */
    double fps = sec(section_of(nx))->fps;
    if (frame_at(nx, NULL, NULL)) {
        set_cur(nx); g_dirty = 1;
        if (g_waiting) { g_waiting = 0; g_next_t = t; }
        g_next_t += 1.0 / fps;
        if (t - g_next_t > 0.25) g_next_t = t + 1.0 / fps;   /* too far behind: resync */
    } else {
        static double since;
        if (!g_waiting) { g_waiting = 1; g_dirty = 1; since = t; }
        if (t - since > 1.0 / fps) g_last_wait = t;          /* late by a frame or more: shows the bar */
        g_next_t = t;
    }
}

static void repaint_now(void) { draw(); g_dirty = 0; }

static void play_usage(void) {
    const char *u =
        "opentvp-play " OPENTVP_VERSION " — plays TVPaint 11 files (.tvpp) without TVPaint\n\n"
        "  opentvp-play [file.tvpp file2.tvpp ...] [options]\n\n"
        "  every clip of every file is played, in order; clips hidden in TVPaint: key H\n"
        "  without a file, the window waits for .tvpp files to be dropped on it\n"
        "  --clip N            one file only: play only its clip N\n"
        "  --frames A-B        with --clip: range, from 0 (default: in/out marks, otherwise all visible content)\n"
        "  --canvas            whole canvas without the camera framing, outside the camera darkened (key C, per clip)\n"
        "  --scale S           preview resolution for this session (the window menu saves 1 or 0.5; default 0.5)\n"
        "  --blur-samples N    motion blur samples (8)\n"
        "  --threads N         render threads (default: all cores)\n"
        "  --ffmpeg PATH       ffmpeg used for sound files other than WAV (default: the PATH, then next to the player)\n\n"
        "In the player, F1 shows the keyboard shortcuts and Ctrl+I the file information.\n"
        "Drop .tvpp files on the window to replace or extend the timeline.\n";
    fputs(u, stderr);
#ifdef _WIN32
    if (g_own_console) msgbox_error(u);
#endif
}

int main(int argc, char **argv) {
    utf8_args(&argc, &argv);
    g_argv0 = argv[0];
    g_die_hook = player_die;
#ifdef _WIN32
    if (own_console()) { g_own_console = 1; ShowWindow(GetConsoleWindow(), SW_HIDE); }
#endif
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version"))) { printf("opentvp-play %s\n", OPENTVP_VERSION); return 0; }
    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) { play_usage(); return 2; }
    const char **paths = xcalloc(sizeof(char *) * argc);
    int npaths = 0, clip = 0, f0 = -1, f1 = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--clip") && i + 1 < argc) clip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { if (sscanf(argv[++i], "%d-%d", &f0, &f1) == 1) f1 = f0; }
        else if (!strcmp(argv[i], "--canvas")) g_canvas_all = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) { g_scale = atof(argv[++i]); g_scale_from_cli = 1; }
        else if (!strcmp(argv[i], "--blur-samples") && i + 1 < argc) g_blur_samples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) g_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ffmpeg") && i + 1 < argc) { g_ffmpeg = argv[++i]; g_ffmpeg_known = 1; }
        else if (!strncmp(argv[i], "--", 2)) { fprintf(stderr, "unknown option: %s\n", argv[i]); play_usage(); return 2; }
        else paths[npaths++] = argv[i];
    }
    if (clip && npaths != 1) die("--clip works with a single file");
    if (f0 >= 0 && !clip) die("--frames needs --clip (each clip has its own frame numbers)");
    if (!g_scale_from_cli) g_scale = load_saved_scale();
    if (g_scale <= 0) g_scale = 0.5;

    /* files given on the command line, in order; any that cannot be opened stops here */
    for (int i = 0; i < npaths; i++) {
        Section **list; int n; char err[700];
        File *F = load_file(paths[i], clip, f0, f1, &list, &n, err, sizeof err);
        if (!F) die(err);
        commit(F, list, n);
    }
    lock(); if (clip) { g_show_hidden = 1; rebuild(); NHIDDEN = 0; } unlock();
    g_in = 0; g_out = NFR - 1;
    for (int f = 0; f < NFILES; f++) {
        printf("%s — %d x %d camera, %g fps\n", FILES[f]->path, FILES[f]->P.out_w, FILES[f]->P.out_h, FILES[f]->P.fps);
        for (int k = 0; k < NALL; k++)
            if (SECS[k]->F == FILES[f])
                printf("  %s: frames %d to %d%s\n", SECS[k]->C->name, SECS[k]->f0, SECS[k]->f1,
                       SECS[k]->active ? "" : " (hidden in TVPaint: key H to show it)");
    }
    if (NFILES) printf("%d clip(s), %d frames, preview at %g %%%s\n", NSEC, NFR, g_scale * 100, g_canvas_all ? ", whole canvas" : "");
    else printf("no file: waiting for .tvpp files dropped on the window\n");
    fflush(stdout);

    update_title();
    g_repaint = repaint_now;
    plat_open(g_title, 640, 360 + BAR_H);
    g_ui = plat_ui_scale();
    fit_window();

    loader_wake();
    thread_start(audio_thread);
    g_next_t = now();
    char shown_title[1400] = "";
    int last_loaded = -1, reported = 0;
    for (;;) {
        plat_pump();
        Event e;
        while (ev_pop(&e)) handle(&e);
        handle_drop();
        update_shift();
        double t = now();
        lock(); int nl = loaded_total(), running = g_loader_running; double lt = g_load_time; unlock();
        if (nl != last_loaded) { last_loaded = nl; g_dirty = 1; }
        advance(t);
        /* sound follows the picture: cue on the displayed frame when playback starts or jumps;
           outside playback, with scrub sound on, each new frame plays its own sound */
        {
            static int was, last = -1;
            int want = g_snd_on && NFR && g_playing && !g_waiting && !g_dragging;
            if (want) { if (!was || (g_cur != last && g_cur != last + 1 && g_cur != g_in)) snd_cue(g_cur); }
            else {
                if (was) snd_stop();
                if (g_snd_scrub && g_snd_on && NFR && g_cur != last && last >= 0 && !g_waiting) snd_scrub(g_cur);
            }
            was = want; last = NFR ? g_cur : -1;
        }
        /* title: file of the current frame, loading progress, and whether playback is waiting for it */
        update_title();
        char tt[1400];
        if (nl >= NFR || !running) snprintf(tt, sizeof tt, "%s", g_title);
        else snprintf(tt, sizeof tt, "%s — %s %d %%", g_title, g_playing && g_waiting ? "waiting for frames, loading" : "loading",
                      (int)(100.0 * nl / NFR));
        if (strcmp(tt, shown_title)) { strcpy(shown_title, tt); plat_title(tt); }
        if (!running && nl >= NFR && reported != nl) {
            reported = nl;
            printf("%d frame(s) loaded in %.1f s\n", nl, lt);
            fflush(stdout);
        }
        if (g_toast[0] && t >= g_toast_until) { g_toast[0] = 0; g_dirty = 1; }
        if (g_fullscreen && g_bar_on && g_playing && !g_dragging && !g_panel && !g_menu && !g_vmenu && t - g_last_move > 2.0) {
            g_bar_on = 0; plat_cursor(0); g_dirty = 1;
        }
        static int bar_shown;
        if (bar_visible() != bar_shown) { bar_shown = !bar_shown; g_dirty = 1; }
        if (g_dirty) { g_dirty = 0; draw(); }
        double wait = g_playing ? g_next_t - now() : 0.015;
        int ms = (int)(wait * 1000);
        sleep_ms(ms < 1 ? 1 : (ms > 15 ? 15 : ms));
    }
}
