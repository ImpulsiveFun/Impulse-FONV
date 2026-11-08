#ifdef __ANDROID__
#include <android/log.h>
#endif
// Headless (M2) user32 / gdi32 / advapi32 / shell32 / ole32 / winmm / steam_api shims.
// The window exists only as a handle and an empty message queue until the Metal layer (M3).
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "shims.h"
#include "input.h"
#include <time.h>

#define FAKE_HWND 0x00010010u
static uint32_t wndproc;
static uint32_t display_width = 1920, display_height = 1080, client_width = 1280, client_height = 720;
#ifdef __ANDROID__
static uint32_t desktop_width=1920,desktop_height=1080;
void host_window_device_mode(uint32_t width,uint32_t height,int windowed){
    if(!width||!height||width>16384||height>16384)return;
    if(!windowed){display_width=client_width=width;display_height=client_height=height;}
    else {display_width=desktop_width;display_height=desktop_height;}
    __android_log_print(ANDROID_LOG_INFO,"FNVWindow","device mode=%ux%u windowed=%d client=%ux%u display=%ux%u",width,height,windowed,client_width,client_height,display_width,display_height);
}
#endif
static uint32_t window_time(void) {
#ifdef __ANDROID__
    return (uint32_t)(rt_rdtsc() / 1000000ull);
#else
    return (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull);
#endif
}

// ---- a small Win32 message queue; DispatchMessage / SendMessage run the game's own WndProc ----------
#include <pthread.h>
typedef struct { uint32_t msg, wp, lp; } QMsg;
static QMsg mq[512]; static int mq_h, mq_t;
static pthread_mutex_t mq_lk = PTHREAD_MUTEX_INITIALIZER;
#ifdef __ANDROID__
void host_window_configure(uint32_t width, uint32_t height) {
    if (width && height && width <= 16384 && height <= 16384) { desktop_width=display_width=width;desktop_height=display_height=height; }
}
void host_win_reset(void) {
    pthread_mutex_lock(&mq_lk);
    wndproc = 0; mq_h = mq_t = 0;
    pthread_mutex_unlock(&mq_lk);
    display_width=desktop_width;display_height=desktop_height;
    client_width = display_width; client_height = display_height;
}
void host_pump_events(void) { /* Surface/input event bridge is a separate layer. */ }
#endif
void host_post_message(uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&mq_lk);
    int n = (mq_t + 1) % 512;
    if (n != mq_h) { mq[mq_t] = (QMsg){msg, wp, lp}; mq_t = n; }
    pthread_mutex_unlock(&mq_lk);
}
static uint32_t call_wndproc(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!wndproc) return 0;
    uint32_t esp = REG(c, R_ESP), saved_eax = REG(c, R_EAX);
    uint32_t sp = esp - 64;                       // below the shim's own frame
    P32(sp, HOST_EXIT_THUNK); P32(sp + 4, hwnd); P32(sp + 8, msg); P32(sp + 12, wp); P32(sp + 16, lp);
    SETREG(c, R_ESP, sp);
    rt_call(c, wndproc);                          // stdcall: callee pops the 4 args + return address
    uint32_t r = REG(c, R_EAX);
    SETREG(c, R_ESP, esp); (void)saved_eax;
    return r;
}

// ---- user32 ------------------------------------------------------------------------------------
SHIM(RegisterClassA) { wndproc = G32(ARG(c, 0) + 4); fprintf(stderr, "[host] RegisterClassA wndproc %#x\n", wndproc); RET(c, 0xC001, 1); }
SHIM(CreateWindowExA) {
    if (ARG(c, 6) && ARG(c, 6) <= 16384) client_width = ARG(c, 6);
    if (ARG(c, 7) && ARG(c, 7) <= 16384) client_height = ARG(c, 7);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"FNVWindow","client=%ux%u display=%ux%u",client_width,client_height,display_width,display_height);
#endif
    // the window starts active: tell the game the way Windows would
    host_post_message(0x001C /* WM_ACTIVATEAPP */, 1, 0);
    host_post_message(0x0006 /* WM_ACTIVATE */, 1 /* WA_ACTIVE */, 0);
    host_post_message(0x0007 /* WM_SETFOCUS */, 0, 0);
    fprintf(stderr, "[host] CreateWindowExA(\"%s\", %ux%u) -> headless window\n", GSTR(ARG(c, 2)) ? GSTR(ARG(c, 2)) : "", ARG(c, 6), ARG(c, 7));
    RET(c, FAKE_HWND, 12);
}
SHIM(DestroyWindow) { RET(c, 1, 1); }
SHIM(ShowWindow) { RET(c, 1, 2); }
SHIM(UpdateWindow) { RET(c, 1, 1); }
SHIM(SetWindowPos) {
    if(!(ARG(c,6)&1u)) { // SWP_NOSIZE preserves the existing client dimensions.
        if(ARG(c,4)&&ARG(c,4)<=16384)client_width=ARG(c,4);
        if(ARG(c,5)&&ARG(c,5)<=16384)client_height=ARG(c,5);
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"FNVWindow","SetWindowPos requested=%ux%u flags=%#x client=%ux%u",ARG(c,4),ARG(c,5),ARG(c,6),client_width,client_height);
#endif
    RET(c, 1, 7);
}
SHIM(SetWindowTextA) { RET(c, 1, 2); }
SHIM(GetWindowTextA) { if (ARG(c, 2)) MEM[ARG(c, 1)] = 0; RET(c, 0, 3); }
SHIM(GetClassNameA) { if (ARG(c, 2)) MEM[ARG(c, 1)] = 0; RET(c, 0, 3); }
SHIM(GetWindow) { RET(c, 0, 2); }
SHIM(GetWindowLongA) { RET(c, 0, 2); }
SHIM(GetClassLongA) { RET(c, 0, 2); }
SHIM(EnumChildWindows) { RET(c, 1, 3); }
SHIM(FindWindowA) { RET(c, 0, 2); }
SHIM(GetActiveWindow) { RET(c, FAKE_HWND, 0); }
SHIM(GetForegroundWindow) { RET(c, FAKE_HWND, 0); }
SHIM(GetFocus) { RET(c, FAKE_HWND, 0); }
SHIM(SetForegroundWindow) { RET(c, 1, 1); }
SHIM(GetClientRect) { uint32_t r = ARG(c, 1); P32(r, 0); P32(r + 4, 0); P32(r + 8, client_width); P32(r + 12, client_height); RET(c, 1, 2); }
SHIM(AdjustWindowRect) { RET(c, 1, 3); }
SHIM(AdjustWindowRectEx) { RET(c, 1, 4); }
SHIM(GetSystemMetrics) { uint32_t i = ARG(c, 0); RET(c, i == 0 || i == 78 ? display_width : i == 1 || i == 79 ? display_height : i == 80 ? 1 : 0, 1); }
SHIM(EnumDisplayDevicesA) { RET(c, 0, 4); }
SHIM(LoadIconA) { RET(c, 0x00020001, 2); }
SHIM(LoadCursorA) { RET(c, 0x00020002, 2); }
SHIM(ShowCursor) { RET(c, 0, 1); }
SHIM(GetDoubleClickTime) { RET(c, 500, 0); }
SHIM(SwapMouseButton) { RET(c, 0, 1); }
SHIM(GetAsyncKeyState) { RET(c, input_async_key((int)ARG(c,0)), 1); }
SHIM(SendInput) { RET(c, ARG(c, 0), 3); }
SHIM(SetWindowsHookExA) { RET(c, 0x00030001, 4); }
SHIM(UnhookWindowsHookEx) { RET(c, 1, 1); }
SHIM(CallNextHookEx) { RET(c, 0, 4); }
void host_pump_events(void);
SHIM(PeekMessageA) {
    host_pump_events();
    uint32_t m = ARG(c, 0), remove = ARG(c, 4) & 1;
    pthread_mutex_lock(&mq_lk);
    if (mq_h == mq_t) { pthread_mutex_unlock(&mq_lk); RET(c, 0, 5); return; }
    QMsg q = mq[mq_h];
    if (remove) mq_h = (mq_h + 1) % 512;
    pthread_mutex_unlock(&mq_lk);
    P32(m, FAKE_HWND); P32(m + 4, q.msg); P32(m + 8, q.wp); P32(m + 12, q.lp);
    P32(m + 16, window_time()); P32(m + 20, 0); P32(m + 24, 0);
    RET(c, 1, 5);
}
SHIM(TranslateMessage) { RET(c, 0, 1); }
SHIM(DispatchMessageA) {
    uint32_t m = ARG(c, 0);
    uint32_t r = call_wndproc(c, G32(m), G32(m + 4), G32(m + 8), G32(m + 12));
    RET(c, r, 1);
}
SHIM(SendMessageA) { uint32_t r = call_wndproc(c, ARG(c, 0), ARG(c, 1), ARG(c, 2), ARG(c, 3)); RET(c, r, 4); }
SHIM(DefWindowProcA) { RET(c, 0, 4); }
SHIM(MessageBoxA) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, "FNVHost", "MessageBox: %s: %s", GSTR(ARG(c, 2)) ? GSTR(ARG(c, 2)) : "", GSTR(ARG(c, 1)) ? GSTR(ARG(c, 1)) : "");
#endif
    fprintf(stderr, "[host] MessageBoxA: \"%s\" — %s\n", GSTR(ARG(c, 2)) ? GSTR(ARG(c, 2)) : "", GSTR(ARG(c, 1)) ? GSTR(ARG(c, 1)) : "");
    RET(c, 1, 4);    // IDOK
}
// ---- gdi32 / comctl32 ----------------------------------------------------------------------------
SHIM(GetStockObject) { RET(c, 0x00040001, 1); }
SHIM(InitCommonControlsEx) { RET(c, 1, 1); }
SHIM(ImageList_LoadImageA) { RET(c, 0x00050001, 7); }
SHIM(ImageList_Destroy) { RET(c, 1, 1); }

// ---- advapi32: an empty registry (the game falls back to defaults / its INIs) -------------------
SHIM(RegOpenKeyExA) { RET(c, 2, 5); }                // ERROR_FILE_NOT_FOUND
SHIM(RegCreateKeyExA) { if (ARG(c, 7)) P32(ARG(c, 7), 0x00060001); RET(c, 0, 9); }
SHIM(RegQueryValueExA) { RET(c, 2, 6); }
SHIM(RegSetValueExA) { RET(c, 0, 6); }
SHIM(RegCloseKey) { RET(c, 0, 1); }
SHIM(GetUserNameA) {
    uint32_t buffer = ARG(c, 0), size = ARG(c, 1);
    if (!buffer || !size) { SETERR(87); RET(c, 0, 2); return; }
    if (G32(size) < 7) { P32(size, 7); SETERR(122); RET(c, 0, 2); return; }
    memcpy(MEM + buffer, "Player", 7); P32(size, 7); RET(c, 1, 2);
}

// ---- shell32: special folders live under the run directory --------------------------------------
SHIM(SHGetFolderPathA) {
    const char *p = "C:\\Games\\FalloutNV\\UserDocuments";
#ifdef __ANDROID__
    if ((ARG(c, 1) & 0xff) == 0x1c) p = "C:\\Games\\FalloutNV\\LocalAppData"; // CSIDL_LOCAL_APPDATA
    char path[1024]; host_path(p, path, sizeof path);
    mkdir(path, 0755);
#else
    mkdir("UserDocuments", 0755);
#endif
    strcpy((char *)MEM + ARG(c, 4), p);
    RET(c, 0, 5);
}
// Nothing external is launched (e.g. FalloutNVLauncher.exe). FNV tests the result for != 0, not the
// documented > 32, so failure has to be 0 for it to carry on with default settings.
SHIM(ShellExecuteA) {
    fprintf(stderr, "[host] ShellExecuteA(%s) -> not launched\n", GSTR(ARG(c, 2)) ? GSTR(ARG(c, 2)) : "");
    RET(c, 0, 6);
}
SHIM(CommandLineToArgvW) { if (ARG(c, 1)) P32(ARG(c, 1), 0); RET(c, gmalloc(8), 2); }

// ---- ole32 -------------------------------------------------------------------------------------
SHIM(CoInitialize) { RET(c, 0, 1); }
SHIM(CoInitializeEx) { RET(c, 0, 2); }
SHIM(CoUninitialize) { RET(c, 0, 0); }
SHIM(CoFreeUnusedLibraries) { RET(c, 0, 0); }
SHIM(CoTaskMemAlloc) { RET(c, gmalloc(ARG(c, 0)), 1); }
SHIM(CoTaskMemFree) { gfree(ARG(c, 0)); RET(c, 0, 1); }
uint32_t dshow_create_filtergraph(uint32_t iid);
SHIM(CoCreateInstance) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"FNVMedia","CoCreateInstance clsid=%#x iid=%#x caller=%#x",G32(ARG(c,0)),G32(ARG(c,3)),G32(REG(c,R_ESP)));
#endif

    if (G32(ARG(c, 0)) == 0xe436ebb3u) {            // CLSID_FilterGraph: DirectShow music player
#ifdef __ANDROID__
        if(ARG(c,1)){P32(ARG(c,4),0);RET(c,0x80040110u,5);return;}
        uint32_t object=dshow_create_filtergraph(ARG(c,3));
        P32(ARG(c,4),object);RET(c,object?0:0x80004002u,5);return;
#else
        P32(ARG(c, 4), dshow_create_filtergraph(ARG(c, 3)));
        RET(c, 0, 5); return;
#endif
    }
    uint32_t g = ARG(c, 0), ig = ARG(c, 3);
    fprintf(stderr, "[host] CoCreateInstance clsid {%08x-%04x-%04x-...} iid {%08x-...} -> REGDB_E_CLASSNOTREG\n",
            G32(g), G32(g + 4) & 0xFFFF, G32(g + 4) >> 16, G32(ig));
    P32(ARG(c, 4), 0); RET(c, 0x80040154u, 5);
}

// ---- winmm -------------------------------------------------------------------------------------
SHIM(timeGetTime) { RET(c, window_time(), 0); }

#ifdef __ANDROID__
#define CREATE_INSTANCE_ENTRY E(CoCreateInstance)
int host_win_selftest(void) {
    uint32_t metric[] = {0};
    if (host_shim_test_call(GetSystemMetrics_, metric, 1) != display_width) return 1;
    metric[0] = 1;
    if (host_shim_test_call(GetSystemMetrics_, metric, 1) != display_height) return 2;
    uint32_t window[] = {0, 0, 0, 0, 0, 0, 320, 240, 0, 0, 0, 0};
    uint32_t handle = host_shim_test_call(CreateWindowExA_, window, 12), rectangle[] = {handle, 0x240000};
    if (!handle || !host_shim_test_call(GetClientRect_, rectangle, 2) || G32(0x240008) != 320 || G32(0x24000c) != 240) return 3;
    uint32_t resize[]={handle,0,0,0,640,480,0};
    if(!host_shim_test_call(SetWindowPos_,resize,7)||!host_shim_test_call(GetClientRect_,rectangle,2)||G32(0x240008)!=640||G32(0x24000c)!=480)return 9;
    resize[4]=999;resize[5]=999;resize[6]=1;
    if(!host_shim_test_call(SetWindowPos_,resize,7)||!host_shim_test_call(GetClientRect_,rectangle,2)||G32(0x240008)!=640||G32(0x24000c)!=480)return 10;
    host_window_device_mode(800,600,0);
    metric[0]=0;
    if(host_shim_test_call(GetSystemMetrics_,metric,1)!=800||!host_shim_test_call(GetClientRect_,rectangle,2)||G32(0x240008)!=800||G32(0x24000c)!=600)return 11;
    host_window_device_mode(640,480,1);
    if(host_shim_test_call(GetSystemMetrics_,metric,1)!=desktop_width||!host_shim_test_call(GetClientRect_,rectangle,2)||G32(0x240008)!=800)return 12;
    uint32_t peek[] = {0x240000, 0, 0, 0, 0};
    if (!host_shim_test_call(PeekMessageA_, peek, 5) || G32(0x240004) != 0x1c) return 4;
    peek[4] = 1;
    if (!host_shim_test_call(PeekMessageA_, peek, 5) || G32(0x240004) != 0x1c) return 5;
    if (!host_shim_test_call(PeekMessageA_, peek, 5) || G32(0x240004) != 6) return 6;
    uint32_t user[] = {0x240000, 0x240100};
    P32(user[1], 1); MEM[user[0]] = 0x5a;
    if (host_shim_test_call(GetUserNameA_, user, 2) || G32(user[1]) != 7 || MEM[user[0]] != 0x5a) return 7;
    if (!host_shim_test_call(GetUserNameA_, user, 2) || strcmp(GSTR(user[0]), "Player")) return 8;
    host_win_reset();
    return 0;
}
#else
#define CREATE_INSTANCE_ENTRY E(CoCreateInstance)
#endif

// ---- steam_api: report "Steam not running" so the game takes its offline path -------------------
SHIM(SteamAPI_Init) { fprintf(stderr, "[host] SteamAPI_Init -> false (offline)\n"); RET(c, 0, 0); }
SHIM(SteamAPI_IsSteamRunning) { RET(c, 0, 0); }
SHIM(SteamAPI_Shutdown) { RET(c, 0, 0); }
SHIM(SteamAPI_RunCallbacks) { RET(c, 0, 0); }
SHIM(SteamAPI_RegisterCallback) { RET(c, 0, 0); }      // cdecl: caller pops
SHIM(SteamAPI_UnregisterCallback) { RET(c, 0, 0); }
SHIM(SteamUser) { RET(c, 0, 0); }
SHIM(SteamFriends) { RET(c, 0, 0); }
SHIM(SteamUserStats) { RET(c, 0, 0); }
SHIM(SteamApps) { RET(c, 0, 0); }
SHIM(SteamUtils) { RET(c, 0, 0); }

SHIM_TABLE(win_shims,
    E(RegisterClassA) E(CreateWindowExA) E(DestroyWindow) E(ShowWindow) E(UpdateWindow) E(SetWindowPos)
    E(SetWindowTextA) E(GetWindowTextA) E(GetClassNameA) E(GetWindow) E(GetWindowLongA) E(GetClassLongA)
    E(EnumChildWindows) E(FindWindowA) E(GetActiveWindow) E(GetForegroundWindow) E(GetFocus) E(SetForegroundWindow) E(GetClientRect)
    E(AdjustWindowRect) E(AdjustWindowRectEx) E(GetSystemMetrics) E(EnumDisplayDevicesA) E(LoadIconA)
    E(LoadCursorA) E(ShowCursor) E(GetDoubleClickTime) E(SwapMouseButton) E(GetAsyncKeyState) E(SendInput)
    E(SetWindowsHookExA) E(UnhookWindowsHookEx) E(CallNextHookEx) E(PeekMessageA) E(TranslateMessage)
    E(DispatchMessageA) E(SendMessageA) E(DefWindowProcA) E(MessageBoxA) E(GetStockObject)
    E(InitCommonControlsEx) E(ImageList_LoadImageA) E(ImageList_Destroy) E(RegOpenKeyExA) E(RegCreateKeyExA)
    E(RegQueryValueExA) E(RegSetValueExA) E(RegCloseKey) E(GetUserNameA) E(SHGetFolderPathA) E(ShellExecuteA)
    E(CommandLineToArgvW) E(CoInitialize) E(CoInitializeEx) E(CoUninitialize) E(CoFreeUnusedLibraries)
    E(CoTaskMemAlloc) E(CoTaskMemFree) CREATE_INSTANCE_ENTRY E(timeGetTime) E(SteamAPI_Init)
    E(SteamAPI_IsSteamRunning) E(SteamAPI_Shutdown) E(SteamAPI_RunCallbacks) E(SteamAPI_RegisterCallback)
    E(SteamAPI_UnregisterCallback) E(SteamUser) E(SteamFriends) E(SteamUserStats) E(SteamApps) E(SteamUtils))
