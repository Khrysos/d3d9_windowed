// =============================================================================
// d3d9.dll proxy for legacy DirectX 9 games.
//
// Features (controlled by .\preferences.ini):
//     StartWindowed=1          -> 0 = borderless fullscreen, 1 = windowed
//     IgnoreDeactivate=1       -> don't pause game on focus lost (alt-tab)
//     DisableClipCursor=1      -> prevent cursor confinement/capture
// =============================================================================
#include <windows.h>
#include <windowsx.h>

#define Direct3DCreate9   Direct3DCreate9__sdk_decl
#define Direct3DCreate9Ex Direct3DCreate9Ex__sdk_decl
#include <d3d9.h>
#undef Direct3DCreate9
#undef Direct3DCreate9Ex

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif

#include <dinput.h>
#include <string>
#include "MinHook.h"

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

// =============================================================================
// Config
// =============================================================================

struct Config {
    bool startWindowed = true;
    bool ignoreDeactivate = true;
    bool disableClip = true;

    static bool ReadIniBool(const char* section, const char* key, bool def,
        const char* path = ".\\preferences.ini")
    {
        char buf[32]{};
        GetPrivateProfileStringA(section, key, def ? "1" : "0", buf, sizeof(buf), path);
        return buf[0] != '0';
    }

    void Load(const char* path = ".\\preferences.ini") {
        startWindowed = ReadIniBool("Preferences", "StartWindowed", true, path);
        ignoreDeactivate = ReadIniBool("Preferences", "IgnoreDeactivate", true, path);
        disableClip = ReadIniBool("Preferences", "DisableClipCursor", true, path);
    }
};

static Config g_cfg{};


// =============================================================================
// Globals / state
// =============================================================================

static HWND    g_hwnd = nullptr;         // best-known game window
static HWND    g_wndprocHwnd = nullptr;  // window actually subclassed
static WNDPROC g_origWndProc = nullptr;

static RECT g_windowedRect{ 100, 100, 1380, 880 };

static ULONGLONG g_processStartMs = 0;

// Virtual Win32 client size exposed to the game (usually the backbuffer size).
static volatile LONG g_virtualW = 0;
static volatile LONG g_virtualH = 0;

// Virtual Win32 sizing is only needed for titles that compute UI/input from Win32 client metrics
static volatile LONG g_win32VirtEnabled = 0;
static volatile LONG g_win32VirtHooksInstalled = 0;

// --- IgnoreDeactivate v2 (GFW spoof) ---
static volatile LONG g_deactivated = 0;   // set by WndProc
static volatile LONG g_seenPresent = 0;   // set by any Present hook
static unsigned long long g_presentTotal = 0;

using GetForegroundWindow_t = HWND(WINAPI*)();
static GetForegroundWindow_t Real_GetForegroundWindow = nullptr;
static void* g_pGetForegroundWindow = nullptr;
static volatile LONG g_gfwHookInstalled = 0;

static HWND GetRealForegroundWindow() {
    if (Real_GetForegroundWindow) return Real_GetForegroundWindow();
    return ::GetForegroundWindow(); // only safe before hook is installed
}

static bool IsGameForeground() {
    return g_hwnd && (GetRealForegroundWindow() == g_hwnd);
}

static BOOL GetClientRectRaw(HWND hwnd, RECT* rc);
static BOOL ScreenToClientRaw(HWND hwnd, POINT* pt);
static BOOL ClientToScreenRaw(HWND hwnd, POINT* pt);
static bool ShouldVirtualizeWin32(HWND hwnd);
static void GetVirtualSize(LONG& w, LONG& h);
static bool GetActualClientSize(HWND hwnd, LONG& w, LONG& h);

// Convert client rect -> screen-space rect. Useful for ClipCursor.
static RECT GetClientRectScreen(HWND hwnd) {
    RECT rc{ 0,0,0,0 };
    if (!hwnd) return rc;

    GetClientRectRaw(hwnd, &rc);
    POINT tl{ rc.left, rc.top };
    POINT br{ rc.right, rc.bottom };
    ClientToScreenRaw(hwnd, &tl);
    ClientToScreenRaw(hwnd, &br);

    rc.left = tl.x;
    rc.top = tl.y;
    rc.right = br.x;
    rc.bottom = br.y;
    return rc;
}

static RECT GetMonitorRect(HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfo(mon, &mi);
    return mi.rcMonitor;
}

static HWND FindMainWindowForThisProcess() {
    struct Ctx { DWORD pid; HWND best; int bestArea; } ctx{ GetCurrentProcessId(), nullptr, 0 };

    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != c->pid) return TRUE;

        if (!IsWindowVisible(w)) return TRUE;
        if (GetWindow(w, GW_OWNER) != nullptr) return TRUE;

        RECT r{};
        GetWindowRect(w, &r);
        int area = (r.right - r.left) * (r.bottom - r.top);
        if (area > c->bestArea) {
            c->best = w;
            c->bestArea = area;
        }
        return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.best;
}

// =============================================================================
// Window style helpers
// =============================================================================

static void ApplyBorderless(HWND hwnd) {
    if (!hwnd) return;

    RECT mr = GetMonitorRect(hwnd);

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    SetWindowPos(hwnd, HWND_TOP,
        mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top,
        SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

static void ApplyWindowed(HWND hwnd) {
    if (!hwnd) return;

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_POPUP);
    style |= WS_OVERLAPPEDWINDOW;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    int w = g_windowedRect.right - g_windowedRect.left;
    int h = g_windowedRect.bottom - g_windowedRect.top;
    if (w < 200) w = 1280;
    if (h < 200) h = 720;

    SetWindowPos(hwnd, HWND_NOTOPMOST,
        g_windowedRect.left, g_windowedRect.top, w, h,
        SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

// =============================================================================
// Mouse policy
// =============================================================================

static void ApplyMousePolicyNow() {
    if (!g_hwnd) return;

    if (g_cfg.disableClip || !IsGameForeground()) {
        ClipCursor(nullptr);
        ::ReleaseCapture();
        return;
    }

    RECT clip = GetClientRectScreen(g_hwnd);
    if (clip.right > clip.left && clip.bottom > clip.top) {
        ClipCursor(&clip);
    }
}

// =============================================================================
// WndProc hook
// =============================================================================

static LRESULT CALLBACK Hook_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    if (ShouldVirtualizeWin32(hwnd)) {
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        {
            LONG vw = 0, vh = 0;
            GetVirtualSize(vw, vh);

            LONG aw = 0, ah = 0;
            GetActualClientSize(hwnd, aw, ah);

            if (vw > 0 && vh > 0 && aw > 0 && ah > 0) {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);

                int sx = MulDiv(x, (int)vw, (int)aw);
                int sy = MulDiv(y, (int)vh, (int)ah);

                lParam = MAKELPARAM((short)sx, (short)sy);
            }
        } break;
        default:
            break;
        }
    }

    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) break;
        return DefWindowProc(hwnd, msg, wParam, lParam);

    case WM_SIZE:
        if (ShouldVirtualizeWin32(hwnd)) {
            LONG vw = 0, vh = 0;
            GetVirtualSize(vw, vh);
            if (vw > 0 && vh > 0) {
                lParam = MAKELPARAM((WORD)vw, (WORD)vh);
            }
        }
        break;

    case WM_ACTIVATEAPP:
        if (wParam == FALSE) {
            InterlockedExchange(&g_deactivated, 1);
            ClipCursor(nullptr);
            ::ReleaseCapture();
            if (g_cfg.ignoreDeactivate) return 0;
        }
        else {
            InterlockedExchange(&g_deactivated, 0);
            ApplyMousePolicyNow();
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            InterlockedExchange(&g_deactivated, 1);
            ClipCursor(nullptr);
            ::ReleaseCapture();
            if (g_cfg.ignoreDeactivate) return 0;
        }
        else {
            InterlockedExchange(&g_deactivated, 0);
            ApplyMousePolicyNow();
        }
        break;

    case WM_SETFOCUS:
        InterlockedExchange(&g_deactivated, 0);
        ApplyMousePolicyNow();
        break;

    case WM_KILLFOCUS:
        InterlockedExchange(&g_deactivated, 1);
        ClipCursor(nullptr);
        ::ReleaseCapture();
        if (g_cfg.ignoreDeactivate) return 0;
        break;

    case WM_EXITSIZEMOVE:
        PostMessage(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        PostMessage(hwnd, WM_SETFOCUS, 0, 0);
        break;
    }

    return CallWindowProc(g_origWndProc, hwnd, msg, wParam, lParam);
}

static void InstallWndProc(HWND hwnd) {
    if (!hwnd) return;
    if (hwnd == g_wndprocHwnd) return;

    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Hook_WndProc))
        );
    g_wndprocHwnd = hwnd;
}

// =============================================================================
// user32 hooks
// =============================================================================

using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
using SetCapture_t = HWND(WINAPI*)(HWND);
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using CDSExA_t = LONG(WINAPI*)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
using CDSExW_t = LONG(WINAPI*)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);
using GetClientRect_t = BOOL(WINAPI*)(HWND, LPRECT);
using ScreenToClient_t = BOOL(WINAPI*)(HWND, LPPOINT);
using ClientToScreen_t = BOOL(WINAPI*)(HWND, LPPOINT);

static ClipCursor_t      Real_ClipCursor = nullptr;
static SetCapture_t      Real_SetCapture = nullptr;
static SetCursorPos_t    Real_SetCursorPos = nullptr;
static CDSExA_t          Real_ChangeDisplaySettingsExA = nullptr;
static CDSExW_t          Real_ChangeDisplaySettingsExW = nullptr;
static GetClientRect_t   Real_GetClientRect = nullptr;
static ScreenToClient_t  Real_ScreenToClient = nullptr;
static ClientToScreen_t  Real_ClientToScreen = nullptr;

static BOOL GetClientRectRaw(HWND hwnd, RECT* rc) {
    if (Real_GetClientRect) return Real_GetClientRect(hwnd, rc);
    return ::GetClientRect(hwnd, rc);
}
static BOOL ScreenToClientRaw(HWND hwnd, POINT* pt) {
    if (Real_ScreenToClient) return Real_ScreenToClient(hwnd, pt);
    return ::ScreenToClient(hwnd, pt);
}
static BOOL ClientToScreenRaw(HWND hwnd, POINT* pt) {
    if (Real_ClientToScreen) return Real_ClientToScreen(hwnd, pt);
    return ::ClientToScreen(hwnd, pt);
}

static void GetVirtualSize(LONG& w, LONG& h) {
    w = InterlockedCompareExchange(&g_virtualW, 0, 0);
    h = InterlockedCompareExchange(&g_virtualH, 0, 0);
}

static bool GetActualClientSize(HWND hwnd, LONG& w, LONG& h) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT rc{};
    if (!GetClientRectRaw(hwnd, &rc)) return false;
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    return (w > 0 && h > 0);
}

static bool ShouldVirtualizeWin32(HWND hwnd) {
    if (InterlockedCompareExchange(&g_win32VirtEnabled, 0, 0) == 0) return false;
    if (!hwnd) return false;
    if (!g_hwnd || !IsWindow(g_hwnd)) return false;
    if (hwnd != g_hwnd) return false;

    LONG vw = 0, vh = 0;
    GetVirtualSize(vw, vh);
    if (vw <= 0 || vh <= 0) return false;

    // If the real client already matches the backbuffer, do nothing.
    LONG aw = 0, ah = 0;
    if (GetActualClientSize(hwnd, aw, ah) && aw == vw && ah == vh) {
        return false;
    }

    return true;
}

static BOOL WINAPI Hook_GetClientRect(HWND hwnd, LPRECT rc) {
    BOOL ok = GetClientRectRaw(hwnd, rc);
    if (!ok || !rc) return ok;

    if (ShouldVirtualizeWin32(hwnd)) {
        LONG vw = 0, vh = 0;
        GetVirtualSize(vw, vh);
        if (vw > 0 && vh > 0) {
            rc->left = 0;
            rc->top = 0;
            rc->right = vw;
            rc->bottom = vh;
        }
    }
    return ok;
}

static BOOL WINAPI Hook_ScreenToClient(HWND hwnd, LPPOINT pt) {
    BOOL ok = ScreenToClientRaw(hwnd, pt);
    if (!ok || !pt) return ok;

    if (ShouldVirtualizeWin32(hwnd)) {
        LONG vw = 0, vh = 0;
        GetVirtualSize(vw, vh);

        LONG aw = 0, ah = 0;
        if (vw > 0 && vh > 0 && GetActualClientSize(hwnd, aw, ah)) {
            pt->x = MulDiv(pt->x, vw, aw);
            pt->y = MulDiv(pt->y, vh, ah);
        }
    }
    return ok;
}

static BOOL WINAPI Hook_ClientToScreen(HWND hwnd, LPPOINT pt) {
    if (!pt) return ClientToScreenRaw(hwnd, pt);

    if (ShouldVirtualizeWin32(hwnd)) {
        LONG vw = 0, vh = 0;
        GetVirtualSize(vw, vh);

        LONG aw = 0, ah = 0;
        if (vw > 0 && vh > 0 && GetActualClientSize(hwnd, aw, ah)) {
            POINT p = *pt;
            p.x = MulDiv(p.x, aw, vw);
            p.y = MulDiv(p.y, ah, vh);
            BOOL ok = ClientToScreenRaw(hwnd, &p);
            if (ok) *pt = p;
            return ok;
        }
    }
    return ClientToScreenRaw(hwnd, pt);
}

static BOOL WINAPI Hook_ClipCursor(const RECT* r) {
    if (g_cfg.disableClip && r != nullptr) {
        if (Real_ClipCursor) Real_ClipCursor(nullptr);
        return TRUE;
    }
    return Real_ClipCursor ? Real_ClipCursor(r) : TRUE;
}

static HWND WINAPI Hook_SetCapture(HWND hwnd) {
    if (g_cfg.disableClip || (g_hwnd && GetRealForegroundWindow() != g_hwnd)) {
        ::ReleaseCapture();
        return nullptr;
    }
    return Real_SetCapture ? Real_SetCapture(hwnd) : hwnd;
}

static BOOL WINAPI Hook_SetCursorPos(int x, int y) {
    if (g_hwnd && GetRealForegroundWindow() != g_hwnd) {
        return TRUE;
    }
    return Real_SetCursorPos ? Real_SetCursorPos(x, y) : TRUE;
}

static bool LooksLikeModeSwitchA(const DEVMODEA* dm) {
    if (!dm) return false;
    DWORD f = dm->dmFields;
    return (f & (DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY)) != 0;
}

static bool LooksLikeModeSwitchW(const DEVMODEW* dm) {
    if (!dm) return false;
    DWORD f = dm->dmFields;
    return (f & (DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY)) != 0;
}

static LONG WINAPI Hook_ChangeDisplaySettingsExA(LPCSTR dev, DEVMODEA* dm, HWND hwnd, DWORD flags, LPVOID param) {
    if ((flags & CDS_FULLSCREEN) || LooksLikeModeSwitchA(dm)) {
        return DISP_CHANGE_SUCCESSFUL;
    }
    return Real_ChangeDisplaySettingsExA ? Real_ChangeDisplaySettingsExA(dev, dm, hwnd, flags, param)
        : DISP_CHANGE_SUCCESSFUL;
}

static LONG WINAPI Hook_ChangeDisplaySettingsExW(LPCWSTR dev, DEVMODEW* dm, HWND hwnd, DWORD flags, LPVOID param) {
    if ((flags & CDS_FULLSCREEN) || LooksLikeModeSwitchW(dm)) {
        return DISP_CHANGE_SUCCESSFUL;
    }
    return Real_ChangeDisplaySettingsExW ? Real_ChangeDisplaySettingsExW(dev, dm, hwnd, flags, param)
        : DISP_CHANGE_SUCCESSFUL;
}

static HWND WINAPI Hook_GetForegroundWindow() {
    HWND real = Real_GetForegroundWindow ? Real_GetForegroundWindow() : nullptr;

    if (!g_cfg.ignoreDeactivate) return real;

    // safety: avoid launchers/config processes that die quickly
    if (g_processStartMs == 0 || (GetTickCount64() - g_processStartMs) < 5000)
        return real;

    // don't spoof until we're actually presenting
    if (InterlockedCompareExchange(&g_seenPresent, 0, 0) == 0)
        return real;

    // only spoof while deactivated
    if (InterlockedCompareExchange(&g_deactivated, 0, 0) == 0)
        return real;

    if (!g_hwnd || !IsWindow(g_hwnd))
        return real;

    if (real == g_hwnd)
        return real;

    return g_hwnd;
}

static void MaybeInstallGfwHook() {
    if (InterlockedCompareExchange(&g_gfwHookInstalled, 0, 0) != 0)
        return;

    if (!g_cfg.ignoreDeactivate)
        return;

    if (!g_pGetForegroundWindow)
        return;

    if (g_processStartMs == 0 || (GetTickCount64() - g_processStartMs) < 5000)
        return;

    if (g_presentTotal < 120)
        return;

    if (MH_CreateHook(g_pGetForegroundWindow, (void*)&Hook_GetForegroundWindow,
        (void**)&Real_GetForegroundWindow) == MH_OK) {
        if (MH_EnableHook(g_pGetForegroundWindow) == MH_OK) {
            InterlockedExchange(&g_gfwHookInstalled, 1);
        }
    }
}

static HMODULE g_user32 = nullptr;

static HMODULE GetUser32Module() {
    if (g_user32) return g_user32;
    g_user32 = GetModuleHandleA("user32.dll");
    if (!g_user32) g_user32 = LoadLibraryA("user32.dll");
    return g_user32;
}

static void InstallUser32Hooks() {
    HMODULE user32 = GetUser32Module();
    if (!user32) return;

    auto hookIfPresent = [&](const char* name, void* detour, void** originalOut) {
        void* p = reinterpret_cast<void*>(GetProcAddress(user32, name));
        if (!p) return;
        if (MH_CreateHook(p, detour, originalOut) == MH_OK) {
            MH_EnableHook(p);
        }
        };

    hookIfPresent("ClipCursor", (void*)&Hook_ClipCursor, (void**)&Real_ClipCursor);
    hookIfPresent("SetCapture", (void*)&Hook_SetCapture, (void**)&Real_SetCapture);
    hookIfPresent("SetCursorPos", (void*)&Hook_SetCursorPos, (void**)&Real_SetCursorPos);

    hookIfPresent("ChangeDisplaySettingsExA", (void*)&Hook_ChangeDisplaySettingsExA, (void**)&Real_ChangeDisplaySettingsExA);
    hookIfPresent("ChangeDisplaySettingsExW", (void*)&Hook_ChangeDisplaySettingsExW, (void**)&Real_ChangeDisplaySettingsExW);

    g_pGetForegroundWindow = reinterpret_cast<void*>(GetProcAddress(user32, "GetForegroundWindow"));
}

// These are the "dangerous" hooks that can break some titles. Install only if we detect we need them.
static void InstallUser32VirtualHooks() {
    HMODULE user32 = GetUser32Module();
    if (!user32) return;

    auto hookIfPresent = [&](const char* name, void* detour, void** originalOut) {
        void* p = reinterpret_cast<void*>(GetProcAddress(user32, name));
        if (!p) return;
        if (MH_CreateHook(p, detour, originalOut) == MH_OK) {
            MH_EnableHook(p);
        }
        };

    hookIfPresent("GetClientRect", (void*)&Hook_GetClientRect, (void**)&Real_GetClientRect);
    hookIfPresent("ScreenToClient", (void*)&Hook_ScreenToClient, (void**)&Real_ScreenToClient);
    hookIfPresent("ClientToScreen", (void*)&Hook_ClientToScreen, (void**)&Real_ClientToScreen);
}

static void MaybeInstallUser32VirtualHooks() {
    if (InterlockedCompareExchange(&g_win32VirtEnabled, 0, 0) == 0) return;

    // 0 = not installed, 2 = installing, 1 = installed
    if (InterlockedCompareExchange(&g_win32VirtHooksInstalled, 2, 0) != 0) return;

    InstallUser32VirtualHooks();

    // Even if some hooks fail, we still consider this "installed enough" to avoid thrashing.
    InterlockedExchange(&g_win32VirtHooksInstalled, 1);
}

// =============================================================================
// DirectInput mouse (disable exclusive)
// =============================================================================

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using SetCoopLevel_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A* self, HWND hwnd, DWORD flags);
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A* self, DWORD cbData, LPVOID lpvData);
using Poll_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A* self);

static SetCoopLevel_t Real_SetCooperativeLevel = nullptr;
static HMODULE              g_realDInput8 = nullptr;
static DirectInput8Create_t Real_DirectInput8Create = nullptr;
static volatile LONG g_dinputHooksInstalled = 0;
static GetDeviceState_t Real_GetDeviceState = nullptr;
static Poll_t           Real_Poll = nullptr;

static bool IsMouseOrKeyboardDevice(IDirectInputDevice8A* self) {
    if (!self) return false;
    DIDEVICEINSTANCEA dii{};
    dii.dwSize = sizeof(dii);
    if (FAILED(self->GetDeviceInfo(&dii))) return false;
    const DWORD t = GET_DIDEVICE_TYPE(dii.dwDevType);
    return t == DI8DEVTYPE_MOUSE || t == DI8DEVTYPE_KEYBOARD;
}

static HRESULT STDMETHODCALLTYPE Hook_GetDeviceState(IDirectInputDevice8A* self, DWORD cbData, LPVOID lpvData) {
    HRESULT hr = Real_GetDeviceState ? Real_GetDeviceState(self, cbData, lpvData) : DIERR_GENERIC;

    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        if (IsMouseOrKeyboardDevice(self)) {
            self->Acquire();
            hr = Real_GetDeviceState ? Real_GetDeviceState(self, cbData, lpvData) : hr;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Poll(IDirectInputDevice8A* self) {
    HRESULT hr = Real_Poll ? Real_Poll(self) : DIERR_GENERIC;
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        if (IsMouseOrKeyboardDevice(self)) {
            self->Acquire();
            hr = Real_Poll ? Real_Poll(self) : hr;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_SetCooperativeLevel(IDirectInputDevice8A* self, HWND hwnd, DWORD flags) {
    DIDEVICEINSTANCEA dii{};
    dii.dwSize = sizeof(dii);

    const bool isMouse = (self && SUCCEEDED(self->GetDeviceInfo(&dii)) &&
        GET_DIDEVICE_TYPE(dii.dwDevType) == DI8DEVTYPE_MOUSE);

    if (isMouse) {
        flags &= ~DISCL_EXCLUSIVE;
        flags |= DISCL_NONEXCLUSIVE;

        flags &= ~DISCL_BACKGROUND;
        flags |= DISCL_FOREGROUND;
    }

    return Real_SetCooperativeLevel ? Real_SetCooperativeLevel(self, hwnd, flags) : DIERR_GENERIC;
}

static void EnsureRealDInput8Loaded() {
    if (g_realDInput8) return;

    char sysdir[MAX_PATH]{};
    GetSystemDirectoryA(sysdir, MAX_PATH);
    std::string path = std::string(sysdir) + "\\dinput8.dll";

    g_realDInput8 = LoadLibraryA(path.c_str());
    if (!g_realDInput8) return;

    Real_DirectInput8Create = reinterpret_cast<DirectInput8Create_t>(
        GetProcAddress(g_realDInput8, "DirectInput8Create")
        );
}

static void InstallDirectInputMouseHook() {
    EnsureRealDInput8Loaded();
    if (!Real_DirectInput8Create) return;
    if (InterlockedCompareExchange(&g_dinputHooksInstalled, 0, 0) != 0) return;

    IDirectInput8A* di = nullptr;
    HRESULT hr = Real_DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
        IID_IDirectInput8A, (void**)&di, nullptr);
    if (FAILED(hr) || !di) return;

    IDirectInputDevice8A* dev = nullptr;
    hr = di->CreateDevice(GUID_SysMouse, &dev, nullptr);
    if (FAILED(hr) || !dev) {
        di->Release();
        return;
    }

    void** vtbl = *(void***)dev;
    void* setCoopPtr = vtbl[13]; // stable for IDirectInputDevice8
    if (setCoopPtr) {
        if (MH_CreateHook(setCoopPtr, &Hook_SetCooperativeLevel,
            reinterpret_cast<void**>(&Real_SetCooperativeLevel)) == MH_OK)
        {
            MH_EnableHook(setCoopPtr);
        }
    }

    void* getStatePtr = vtbl[9]; // GetDeviceState
    if (getStatePtr) {
        if (MH_CreateHook(getStatePtr, &Hook_GetDeviceState,
            reinterpret_cast<void**>(&Real_GetDeviceState)) == MH_OK)
        {
            MH_EnableHook(getStatePtr);
        }
    }

    void* pollPtr = vtbl[25];
    if (pollPtr) {
        if (MH_CreateHook(pollPtr, &Hook_Poll,
            reinterpret_cast<void**>(&Real_Poll)) == MH_OK)
        {
            MH_EnableHook(pollPtr);
        }
    }

    InterlockedExchange(&g_dinputHooksInstalled, 1);

    dev->Release();
    di->Release();
}

// =============================================================================
// D3D9 proxy + hooks
// =============================================================================
using PFN_Direct3DCreate9 = IDirect3D9 * (WINAPI*)(UINT);
using PFN_Direct3DCreate9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pPP);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9* self, const RECT*, const RECT*, HWND, const RGNDATA*);
using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9* self, const D3DVIEWPORT9*);
using SwapChainPresent_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9* self,
    const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppDev);

static HMODULE g_realD3D9 = nullptr;
static PFN_Direct3DCreate9   Real_Direct3DCreate9 = nullptr;
static PFN_Direct3DCreate9Ex Real_Direct3DCreate9Ex = nullptr;
static CreateDevice_t      Real_CreateDevice = nullptr;
static Reset_t             Real_Reset = nullptr;
static Present_t           Real_Present = nullptr;
static SetViewport_t       Real_SetViewport = nullptr;
static SwapChainPresent_t  Real_SwapChainPresent = nullptr;

static void EnsureRealD3D9Loaded() {
    if (g_realD3D9) return;

    char sysdir[MAX_PATH]{};
    GetSystemDirectoryA(sysdir, MAX_PATH);
    std::string path = std::string(sysdir) + "\\d3d9.dll";

    g_realD3D9 = LoadLibraryA(path.c_str());
    if (!g_realD3D9) return;

    Real_Direct3DCreate9 = reinterpret_cast<PFN_Direct3DCreate9>(GetProcAddress(g_realD3D9, "Direct3DCreate9"));
    Real_Direct3DCreate9Ex = reinterpret_cast<PFN_Direct3DCreate9Ex>(GetProcAddress(g_realD3D9, "Direct3DCreate9Ex"));
}

// Cached backbuffer size for fast viewport clamping.
static volatile LONG g_bbW = 0;
static volatile LONG g_bbH = 0;
static void UpdateBackbufferSize(IDirect3DDevice9* dev) {
    if (!dev) return;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(bb->GetDesc(&d))) {
            InterlockedExchange(&g_bbW, (LONG)d.Width);
            InterlockedExchange(&g_bbH, (LONG)d.Height);
            // Keep Win32 virtualization in sync with the actual backbuffer.
            InterlockedExchange(&g_virtualW, (LONG)d.Width);
            InterlockedExchange(&g_virtualH, (LONG)d.Height);
        }
        bb->Release();
    }
}

static void ForceWindowedPP(D3DPRESENT_PARAMETERS& pp, HWND hwnd) {
    pp.Windowed = TRUE;
    pp.FullScreen_RefreshRateInHz = 0;
    if (hwnd) pp.hDeviceWindow = hwnd;
    if (pp.BackBufferCount == 0) pp.BackBufferCount = 1;
}

static HWND GetDeviceHwnd(IDirect3DDevice9* dev) {
    if (!dev) return nullptr;

    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(dev->GetCreationParameters(&cp)) && cp.hFocusWindow) {
        return cp.hFocusWindow;
    }

    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        D3DPRESENT_PARAMETERS pp{};
        if (SUCCEEDED(sc->GetPresentParameters(&pp)) && pp.hDeviceWindow) {
            sc->Release();
            return pp.hDeviceWindow;
        }
        sc->Release();
    }

    return nullptr;
}

static void RefreshHwndFromDevice(IDirect3DDevice9* dev) {
    HWND hwnd = GetDeviceHwnd(dev);
    if (hwnd && hwnd != g_hwnd) {
        g_hwnd = hwnd;
        InstallWndProc(g_hwnd);
    }
}

// =============================================================================
// Present stretching (shared helper)
// =============================================================================

static bool BuildClientDstRect(HWND wnd, RECT& outDst) {
    if (!wnd || !IsWindow(wnd)) return false;
    RECT cr{};
    if (!GetClientRectRaw(wnd, &cr)) return false;
    LONG w = cr.right - cr.left;
    LONG h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) return false;
    outDst = RECT{ 0,0,w,h };
    return true;
}

static const RECT* ChooseSrcRectFromViewport(IDirect3DDevice9* dev, const RECT* srcIn, RECT& srcOut) {
    if (srcIn) return srcIn;
    if (!dev) return nullptr;

    // Backbuffer size (for clamping)
    UINT bbw = 0, bbh = 0;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(bb->GetDesc(&d))) { bbw = d.Width; bbh = d.Height; }
        bb->Release();
    }

    D3DVIEWPORT9 vp{};
    if (FAILED(dev->GetViewport(&vp))) return nullptr;
    if (vp.Width == 0 || vp.Height == 0) return nullptr;

    // If viewport already covers the whole backbuffer, let D3D treat src as "entire surface".
    if (bbw && bbh && vp.X == 0 && vp.Y == 0 && vp.Width == bbw && vp.Height == bbh)
        return nullptr;

    LONG sx = (LONG)vp.X;
    LONG sy = (LONG)vp.Y;
    LONG sw = (LONG)vp.Width;
    LONG sh = (LONG)vp.Height;

    if (bbw && bbh) {
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx > (LONG)bbw) sx = (LONG)bbw;
        if (sy > (LONG)bbh) sy = (LONG)bbh;

        LONG maxW = (LONG)bbw - sx;
        LONG maxH = (LONG)bbh - sy;
        if (sw > maxW) sw = maxW;
        if (sh > maxH) sh = maxH;
    }

    if (sw <= 0 || sh <= 0) return nullptr;

    srcOut = RECT{ sx, sy, sx + sw, sy + sh };
    return &srcOut;
}

static HRESULT PresentStretch_Device(
    IDirect3DDevice9* dev,
    const RECT* srcIn,
    const RECT* dstIn,
    HWND hOverride,
    const RGNDATA* dirty)
{
    if (!Real_Present) return D3D_OK;

    HWND target = (hOverride && IsWindow(hOverride)) ? hOverride
        : (g_hwnd && IsWindow(g_hwnd)) ? g_hwnd
        : GetDeviceHwnd(dev);

    RECT dstFull{};
    if (!BuildClientDstRect(target, dstFull)) {
        return Real_Present(dev, srcIn, dstIn, hOverride, dirty);
    }

    bool overrideDst = true;
    if (dstIn) {
        LONG dw = dstIn->right - dstIn->left;
        LONG dh = dstIn->bottom - dstIn->top;
        if (dstIn->left == 0 && dstIn->top == 0 &&
            dw == (dstFull.right - dstFull.left) &&
            dh == (dstFull.bottom - dstFull.top))
        {
            overrideDst = false;
        }
    }

    RECT srcVP{};
    const RECT* srcUse = ChooseSrcRectFromViewport(dev, srcIn, srcVP);
    const RECT* dstUse = overrideDst ? &dstFull : dstIn;

    HWND callOverride = hOverride ? hOverride : target;
    return Real_Present(dev, srcUse, dstUse, callOverride, dirty);
}

static HRESULT STDMETHODCALLTYPE Hook_Present(
    IDirect3DDevice9* self,
    const RECT* src, const RECT* dst, HWND hOverride, const RGNDATA* dirty)
{
    InterlockedExchange(&g_seenPresent, 1);
    g_presentTotal++;
    MaybeInstallGfwHook();

    RefreshHwndFromDevice(self);

    if (!g_hwnd || !IsWindow(g_hwnd)) {
        g_hwnd = FindMainWindowForThisProcess();
        if (g_hwnd) InstallWndProc(g_hwnd);
    }

    ApplyMousePolicyNow();

    return PresentStretch_Device(self, src, dst, hOverride, dirty);
}

// =============================================================================
// Viewport clamping
// =============================================================================
static void MaybeEnableWin32VirtualFromViewport(const D3DVIEWPORT9& vp, LONG bbw, LONG bbh) {
    if (InterlockedCompareExchange(&g_win32VirtEnabled, 0, 0) != 0) return;

    // Don't enable until we've actually presented at least once; avoids launcher/config helpers.
    if (InterlockedCompareExchange(&g_seenPresent, 0, 0) == 0) return;

    if (!g_hwnd || !IsWindow(g_hwnd)) return;

    LONG aw = 0, ah = 0;
    if (!GetActualClientSize(g_hwnd, aw, ah)) return;

    auto absL = [](LONG v) -> LONG { return (v < 0) ? -v : v; };

    if (absL((LONG)vp.Width - aw) <= 32 && absL((LONG)vp.Height - ah) <= 32) {
        if (absL(aw - bbw) > 32 || absL(ah - bbh) > 32) {
            InterlockedExchange(&g_win32VirtEnabled, 1);
            MaybeInstallUser32VirtualHooks();
        }
    }
}

static HRESULT STDMETHODCALLTYPE Hook_SetViewport(IDirect3DDevice9* self, const D3DVIEWPORT9* vpIn) {
    if (!Real_SetViewport || !vpIn || !self) return D3D_OK;

    IDirect3DSurface9* rt = nullptr;
    if (FAILED(self->GetRenderTarget(0, &rt)) || !rt) {
        return Real_SetViewport(self, vpIn);
    }

    D3DSURFACE_DESC rtDesc{};
    if (FAILED(rt->GetDesc(&rtDesc)) || rtDesc.Width == 0 || rtDesc.Height == 0) {
        rt->Release();
        return Real_SetViewport(self, vpIn);
    }

    bool isBackbuffer = false;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(self->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        isBackbuffer = (bb == rt);
        bb->Release();
    }

    if (!isBackbuffer) {
        rt->Release();
        return Real_SetViewport(self, vpIn);
    }

    const LONG bbw = (LONG)rtDesc.Width;
    const LONG bbh = (LONG)rtDesc.Height;

    MaybeEnableWin32VirtualFromViewport(*vpIn, bbw, bbh);

    D3DVIEWPORT9 vp = *vpIn;

    if ((LONG)vp.X < 0) vp.X = 0;
    if ((LONG)vp.Y < 0) vp.Y = 0;
    if ((LONG)vp.X >= bbw) vp.X = 0;
    if ((LONG)vp.Y >= bbh) vp.Y = 0;

    LONG maxW = bbw - (LONG)vp.X;
    LONG maxH = bbh - (LONG)vp.Y;
    if (maxW < 1) maxW = 1;
    if (maxH < 1) maxH = 1;

    DWORD newW = vp.Width;
    DWORD newH = vp.Height;
    if ((LONG)newW > maxW) newW = (DWORD)maxW;
    if ((LONG)newH > maxH) newH = (DWORD)maxH;

    vp.Width = newW;
    vp.Height = newH;

    rt->Release();

    return Real_SetViewport(self, vpIn);
}

// =============================================================================
// SwapChain Present hook
// =============================================================================

static const RECT* ChooseSrcRectFromSwapChain(IDirect3DSwapChain9* sc, IDirect3DDevice9* dev, const RECT* srcIn, RECT& srcOut) {
    if (srcIn) return srcIn;
    if (!sc || !dev) return nullptr;

    UINT bbw = 0, bbh = 0;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(bb->GetDesc(&d))) { bbw = d.Width; bbh = d.Height; }
        bb->Release();
    }

    D3DVIEWPORT9 vp{};
    if (FAILED(dev->GetViewport(&vp))) return nullptr;
    if (vp.Width == 0 || vp.Height == 0) return nullptr;

    if (bbw && bbh && vp.X == 0 && vp.Y == 0 && vp.Width == bbw && vp.Height == bbh)
        return nullptr;

    LONG sx = (LONG)vp.X;
    LONG sy = (LONG)vp.Y;
    LONG sw = (LONG)vp.Width;
    LONG sh = (LONG)vp.Height;

    if (bbw && bbh) {
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx > (LONG)bbw) sx = (LONG)bbw;
        if (sy > (LONG)bbh) sy = (LONG)bbh;

        LONG maxW = (LONG)bbw - sx;
        LONG maxH = (LONG)bbh - sy;
        if (sw > maxW) sw = maxW;
        if (sh > maxH) sh = maxH;
    }

    if (sw <= 0 || sh <= 0) return nullptr;

    srcOut = RECT{ sx, sy, sx + sw, sy + sh };
    return &srcOut;
}

static HRESULT PresentStretch_SwapChain(
    IDirect3DSwapChain9* sc,
    const RECT* srcIn,
    const RECT* dstIn,
    HWND hOverride,
    const RGNDATA* dirty,
    DWORD flags)
{
    if (!Real_SwapChainPresent) return D3D_OK;

    IDirect3DDevice9* dev = nullptr;
    if (FAILED(sc->GetDevice(&dev)) || !dev) {
        return Real_SwapChainPresent(sc, srcIn, dstIn, hOverride, dirty, flags);
    }

    // Determine the window the swapchain is meant to present into.
    D3DPRESENT_PARAMETERS spp{};
    HWND chainWnd = nullptr;
    if (SUCCEEDED(sc->GetPresentParameters(&spp)) && spp.hDeviceWindow) {
        chainWnd = spp.hDeviceWindow;
    }

    HWND target = (hOverride && IsWindow(hOverride)) ? hOverride
        : (g_hwnd && IsWindow(g_hwnd)) ? g_hwnd
        : (chainWnd && IsWindow(chainWnd)) ? chainWnd
        : GetDeviceHwnd(dev);

    if (chainWnd && chainWnd != g_hwnd) {
        g_hwnd = chainWnd;
        InstallWndProc(g_hwnd);
    }

    RECT dstFull{};
    if (!BuildClientDstRect(target, dstFull)) {
        dev->Release();
        return Real_SwapChainPresent(sc, srcIn, dstIn, hOverride, dirty, flags);
    }

    bool overrideDst = true;
    if (dstIn) {
        LONG dw = dstIn->right - dstIn->left;
        LONG dh = dstIn->bottom - dstIn->top;
        if (dstIn->left == 0 && dstIn->top == 0 &&
            dw == (dstFull.right - dstFull.left) &&
            dh == (dstFull.bottom - dstFull.top))
        {
            overrideDst = false;
        }
    }

    RECT srcVP{};
    const RECT* srcUse = ChooseSrcRectFromSwapChain(sc, dev, srcIn, srcVP);
    const RECT* dstUse = overrideDst ? &dstFull : dstIn;

    dev->Release();
    HWND callOverride = hOverride ? hOverride : target;
    return Real_SwapChainPresent(sc, srcUse, dstUse, callOverride, dirty, flags);
}

static HRESULT STDMETHODCALLTYPE Hook_SwapChainPresent(
    IDirect3DSwapChain9* self,
    const RECT* src, const RECT* dst, HWND hOverride, const RGNDATA* dirty, DWORD flags)
{
    InterlockedExchange(&g_seenPresent, 1);
    g_presentTotal++;
    MaybeInstallGfwHook();

    ApplyMousePolicyNow();
    return PresentStretch_SwapChain(self, src, dst, hOverride, dirty, flags);
}

// =============================================================================
// Device hooks install
// =============================================================================

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pPP);

static void InstallDeviceHooks(IDirect3DDevice9* dev) {
    if (!dev) return;

    void** vtbl = *(void***)dev;

    // IDirect3DDevice9 vtable:
    //   Reset       = 16
    //   Present     = 17
    //   SetViewport = 47
    void* resetPtr = vtbl[16];
    void* presentPtr = vtbl[17];
    void* setViewportPtr = vtbl[47];

    if (resetPtr && !Real_Reset) {
        if (MH_CreateHook(resetPtr, &Hook_Reset, reinterpret_cast<void**>(&Real_Reset)) == MH_OK) {
            MH_EnableHook(resetPtr);
        }
    }

    if (presentPtr && !Real_Present) {
        if (MH_CreateHook(presentPtr, &Hook_Present, reinterpret_cast<void**>(&Real_Present)) == MH_OK) {
            MH_EnableHook(presentPtr);
        }
    }

    if (setViewportPtr && !Real_SetViewport) {
        if (MH_CreateHook(setViewportPtr, &Hook_SetViewport, reinterpret_cast<void**>(&Real_SetViewport)) == MH_OK) {
            MH_EnableHook(setViewportPtr);
        }
    }

    UpdateBackbufferSize(dev);
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        void** svtbl = *(void***)sc;
        void* scPresentPtr = svtbl[3];
        if (scPresentPtr && !Real_SwapChainPresent) {
            if (MH_CreateHook(scPresentPtr, &Hook_SwapChainPresent,
                reinterpret_cast<void**>(&Real_SwapChainPresent)) == MH_OK)
            {
                MH_EnableHook(scPresentPtr);
            }
        }
        sc->Release();
    }
}

// =============================================================================
// Reset hook
// =============================================================================

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pPP) {

    if (!g_hwnd || !IsWindow(g_hwnd)) {
        g_hwnd = FindMainWindowForThisProcess();
    }

    if (pPP) {
        ForceWindowedPP(*pPP, g_hwnd);
    }

    HRESULT hr = Real_Reset ? Real_Reset(self, pPP) : D3DERR_INVALIDCALL;

    if (SUCCEEDED(hr)) {
        UpdateBackbufferSize(self);
    }

    // Re-assert window style after a successful reset.
    if (SUCCEEDED(hr) && g_hwnd && IsWindow(g_hwnd)) {
        if (!g_cfg.startWindowed) ApplyBorderless(g_hwnd);
        else ApplyWindowed(g_hwnd);
    }

    return hr;
}

// =============================================================================
// CreateDevice hook
// =============================================================================

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    IDirect3D9* self,
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppDev)
{
    if (hFocusWindow) g_hwnd = hFocusWindow;
    if (!g_hwnd || !IsWindow(g_hwnd)) g_hwnd = FindMainWindowForThisProcess();

    if (g_hwnd) {
        InstallWndProc(g_hwnd);
        GetWindowRect(g_hwnd, &g_windowedRect);

        if (!g_cfg.startWindowed) ApplyBorderless(g_hwnd);
        else ApplyWindowed(g_hwnd);
    }

    if (pPP) {
        ForceWindowedPP(*pPP, g_hwnd);
    }

    HRESULT hr = Real_CreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppDev);
    if (SUCCEEDED(hr) && ppDev && *ppDev) {
        InstallDeviceHooks(*ppDev);
    }

    return hr;
}

static void HookCreateDeviceOn(IDirect3D9* d3d) {
    if (!d3d || Real_CreateDevice) return;

    void** vtbl = *(void***)d3d;
    void* createDevicePtr = vtbl[16];
    if (!createDevicePtr) return;

    if (MH_CreateHook(createDevicePtr, &Hook_CreateDevice,
        reinterpret_cast<void**>(&Real_CreateDevice)) == MH_OK)
    {
        MH_EnableHook(createDevicePtr);
    }
}

// =============================================================================
// Initialization
// =============================================================================

static volatile LONG g_inited = 0;

static void EnsureInit() {
    if (InterlockedCompareExchange(&g_inited, 1, 0) != 0) return;

    g_cfg.Load();

    if (MH_Initialize() != MH_OK) {
        return;
    }

    InstallUser32Hooks();
    InstallDirectInputMouseHook();
}

// =============================================================================
// Exports
// =============================================================================

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    EnsureInit();
    EnsureRealD3D9Loaded();
    if (!Real_Direct3DCreate9) return nullptr;

    IDirect3D9* d3d = Real_Direct3DCreate9(sdk);
    HookCreateDeviceOn(d3d);
    return d3d;
}

extern "C" __declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    EnsureInit();
    EnsureRealD3D9Loaded();
    if (!Real_Direct3DCreate9Ex) return E_NOTIMPL;

    HRESULT hr = Real_Direct3DCreate9Ex(sdk, out);
    if (SUCCEEDED(hr) && out && *out) {
        HookCreateDeviceOn(reinterpret_cast<IDirect3D9*>(*out));
    }
    return hr;
}

static BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_processStartMs = GetTickCount64();
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}