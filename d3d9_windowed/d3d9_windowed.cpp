// -----------------------------------------------------------------------------
// d3d9.dll proxy for legacy DirectX 9 games.
//
// Philosophy:
//   - No on-disk patching.
//   - No scanning/patching game-specific code.
//   - Only intercept Win32 / DirectX / DirectInput boundaries at runtime.
//
// Features (controlled by .\preferences.ini):
//     StartBorderless=1        -> 1 = borderless fullscreen window, 0 = normal window
//     IgnoreDeactivate=1       -> swallow focus-loss notifications to the game
//     DisableClipCursor=1      -> prevent cursor confinement/capture
//
// Rendering policy:
//   - Always windowed (prevents exclusive fullscreen).
//   - Always stretch-to-fill.
// -----------------------------------------------------------------------------

#include <windows.h>

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
    bool startBorderless = true;
    bool ignoreDeactivate = true;
    bool disableClip = true;

    static bool ReadIniBool(const char* section, const char* key, bool def,
        const char* path = ".\\preferences.ini")
    {
        char buf[32]{};
        GetPrivateProfileStringA(section, key, def ? "1" : "0", buf, sizeof(buf), path);
        // Treat any leading '0' as false. Anything else -> true.
        return buf[0] != '0';
    }

    void Load(const char* path = ".\\preferences.ini") {
        startBorderless = ReadIniBool("Borderless", "StartBorderless", true, path);
        ignoreDeactivate = ReadIniBool("Borderless", "IgnoreDeactivate", true, path);
        disableClip = ReadIniBool("Borderless", "DisableClipCursor", true, path);
    }
};

static Config g_cfg{};

// =============================================================================
// Globals / state
// =============================================================================

static HWND    g_hwnd = nullptr;     // best-known game window
static HWND    g_wndprocHwnd = nullptr;     // window actually subclassed
static WNDPROC g_origWndProc = nullptr;     // original WndProc

static RECT g_windowedRect{ 100, 100, 1380, 880 }; // restored window rect when leaving borderless

static bool IsGameForeground() {
    return g_hwnd && (GetForegroundWindow() == g_hwnd);
}

// Convert client rect -> screen-space rect. Useful for ClipCursor.
static RECT GetClientRectScreen(HWND hwnd) {
    RECT rc{ 0,0,0,0 };
    if (!hwnd) return rc;

    GetClientRect(hwnd, &rc);
    POINT tl{ rc.left, rc.top };
    POINT br{ rc.right, rc.bottom };
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);

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

// Find the "main" window.
static HWND FindMainWindowForThisProcess() {
    struct Ctx { DWORD pid; HWND best; int bestArea; } ctx{ GetCurrentProcessId(), nullptr, 0 };

    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != c->pid) return TRUE;

        if (!IsWindowVisible(w)) return TRUE;
        if (GetWindow(w, GW_OWNER) != nullptr) return TRUE; // skip owned/tool windows

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

    // Remove borders/caption and make it a popup.
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    // Resize to monitor.
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
// Prevent mouse trapping:
//   - If DisableClipCursor is enabled
//   - If not the foreground window
//   - Otherwise, confine the cursor to the game's client rectangle.

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
// Purpose:
//   - When IgnoreDeactivate is enabled, swallow activation/focus-loss.
//   - Release cursor on deactivate.

static LRESULT CALLBACK Hook_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ACTIVATEAPP:
        if (wParam == FALSE) {
            // App deactivated (Alt-Tab)
            ClipCursor(nullptr);
            ::ReleaseCapture();
            if (g_cfg.ignoreDeactivate) return 0;
        }
        else {
            // App activated
            ApplyMousePolicyNow();
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            ClipCursor(nullptr);
            ::ReleaseCapture();
            if (g_cfg.ignoreDeactivate) return 0;
        }
        else {
            ApplyMousePolicyNow();
        }
        break;

    case WM_SETFOCUS:
        ApplyMousePolicyNow();
        break;

    case WM_KILLFOCUS:
        ClipCursor(nullptr);
        ::ReleaseCapture();
        if (g_cfg.ignoreDeactivate) return 0;
        break;
    }

    return CallWindowProc(g_origWndProc, hwnd, msg, wParam, lParam);
}

static void InstallWndProc(HWND hwnd) {
    if (!hwnd) return;

    // Only install on a new HWND.
    if (hwnd == g_wndprocHwnd) return;

    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Hook_WndProc))
        );
    g_wndprocHwnd = hwnd;
}

// =============================================================================
// user32 hooks
// =============================================================================
// ClipCursor: block attempts to confine when DisableClipCursor is enabled.
// SetCapture: block capture when DisableClipCursor is enabled OR inactive.
// ChangeDisplaySettingsEx*: block display mode switches (exclusive fullscreen).

using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
using SetCapture_t = HWND(WINAPI*)(HWND);
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using CDSExA_t = LONG(WINAPI*)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
using CDSExW_t = LONG(WINAPI*)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);

static ClipCursor_t    Real_ClipCursor = nullptr;
static SetCapture_t    Real_SetCapture = nullptr;
static SetCursorPos_t  Real_SetCursorPos = nullptr;
static CDSExA_t        Real_ChangeDisplaySettingsExA = nullptr;
static CDSExW_t        Real_ChangeDisplaySettingsExW = nullptr;

static BOOL WINAPI Hook_ClipCursor(const RECT* r) {
    if (g_cfg.disableClip && r != nullptr) {
        if (Real_ClipCursor) Real_ClipCursor(nullptr);
        return TRUE;
    }
    return Real_ClipCursor ? Real_ClipCursor(r) : TRUE;
}

static HWND WINAPI Hook_SetCapture(HWND hwnd) {
    // Avoid "mouse snaps back" when Alt-Tabbed.
    if (g_cfg.disableClip || (g_hwnd && GetForegroundWindow() != g_hwnd)) {
        ::ReleaseCapture();
        return nullptr;
    }
    return Real_SetCapture ? Real_SetCapture(hwnd) : hwnd;
}

static BOOL WINAPI Hook_SetCursorPos(int x, int y) {
    if (g_hwnd && GetForegroundWindow() != g_hwnd) {
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

static void InstallUser32Hooks() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) user32 = LoadLibraryA("user32.dll");
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
}

// =============================================================================
// DirectInput mouse (disable exclusive)
// =============================================================================

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static HMODULE            g_realDInput8 = nullptr;
static DirectInput8Create_t Real_DirectInput8Create = nullptr;

using SetCoopLevel_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A* self, HWND hwnd, DWORD flags);
static SetCoopLevel_t Real_SetCooperativeLevel = nullptr;

static HRESULT STDMETHODCALLTYPE Hook_SetCooperativeLevel(IDirectInputDevice8A* self, HWND hwnd, DWORD flags) {
    DIDEVICEINSTANCEA dii{};
    dii.dwSize = sizeof(dii);

    const bool isMouse = (self && SUCCEEDED(self->GetDeviceInfo(&dii)) &&
        GET_DIDEVICE_TYPE(dii.dwDevType) == DI8DEVTYPE_MOUSE);

    if (isMouse) {
        // Remove exclusive/background flags for mouse devices.
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
    if (Real_SetCooperativeLevel) return; // already hooked

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

    // IDirectInputDevice8 vtable layout is stable; SetCooperativeLevel is index 13.
    void** vtbl = *(void***)dev;
    void* setCoopPtr = vtbl[13];
    if (setCoopPtr) {
        if (MH_CreateHook(setCoopPtr, &Hook_SetCooperativeLevel,
            reinterpret_cast<void**>(&Real_SetCooperativeLevel)) == MH_OK)
        {
            MH_EnableHook(setCoopPtr);
        }
    }

    dev->Release();
    di->Release();
}

// =============================================================================
// D3D9 proxy + hooks
// =============================================================================

static HMODULE g_realD3D9 = nullptr;

using PFN_Direct3DCreate9 = IDirect3D9 * (WINAPI*)(UINT);
using PFN_Direct3DCreate9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

static PFN_Direct3DCreate9   Real_Direct3DCreate9 = nullptr;
static PFN_Direct3DCreate9Ex Real_Direct3DCreate9Ex = nullptr;

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

using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppDev);

using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pPP);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9* self, const RECT*, const RECT*, HWND, const RGNDATA*);

static CreateDevice_t Real_CreateDevice = nullptr;
static Reset_t        Real_Reset = nullptr;
static Present_t      Real_Present = nullptr;

static void ForceWindowedPP(D3DPRESENT_PARAMETERS& pp, HWND hwnd) {
    // Core "no exclusive fullscreen".
    pp.Windowed = TRUE;
    pp.FullScreen_RefreshRateInHz = 0;
    if (hwnd) pp.hDeviceWindow = hwnd;
}

static HWND GetDeviceHwnd(IDirect3DDevice9* dev) {
    if (!dev) return nullptr;

    // Creation parameters
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(dev->GetCreationParameters(&cp)) && cp.hFocusWindow) {
        return cp.hFocusWindow;
    }

    // Fallback: swap chain present parameters
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

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pPP) {
    // Some games attempt to reset into fullscreen; we always keep it windowed.
    if (!g_hwnd || !IsWindow(g_hwnd)) {
        g_hwnd = FindMainWindowForThisProcess();
    }

    if (pPP) {
        ForceWindowedPP(*pPP, g_hwnd);
    }

    HRESULT hr = Real_Reset ? Real_Reset(self, pPP) : D3DERR_INVALIDCALL;

    // Re-assert window style after a successful reset.
    if (SUCCEEDED(hr) && g_hwnd) {
        if (g_cfg.startBorderless) ApplyBorderless(g_hwnd);
        else ApplyWindowed(g_hwnd);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Present(
    IDirect3DDevice9* self,
    const RECT* src, const RECT* dst, HWND hOverride, const RGNDATA* dirty)
{
    // Keep HWND + WndProc in sync even if the game creates/replaces devices.
    RefreshHwndFromDevice(self);

    if (!g_hwnd || !IsWindow(g_hwnd)) {
        g_hwnd = FindMainWindowForThisProcess();
        if (g_hwnd) InstallWndProc(g_hwnd);
    }

    // Apply mouse policy.
    ApplyMousePolicyNow();

    if (!g_hwnd || !Real_Present) {
        return Real_Present ? Real_Present(self, src, dst, hOverride, dirty) : D3D_OK;
    }

    // Force stretch-to-fill.
    RECT cr{};
    GetClientRect(g_hwnd, &cr);
    RECT out{ 0, 0, cr.right - cr.left, cr.bottom - cr.top };

    // If client size is invalid, fall back to original call.
    if (out.right <= 0 || out.bottom <= 0) {
        return Real_Present(self, src, dst, hOverride, dirty);
    }

    return Real_Present(self, nullptr, &out, nullptr, dirty);
}

static void InstallDeviceHooks(IDirect3DDevice9* dev) {
    if (!dev) return;

    // IDirect3DDevice9 vtable:
    //   Reset   = 16
    //   Present = 17
    void** vtbl = *(void***)dev;
    void* resetPtr = vtbl[16];
    void* presentPtr = vtbl[17];

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
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    IDirect3D9* self,
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppDev)
{
    // Capture HWND early.
    if (hFocusWindow) g_hwnd = hFocusWindow;
    if (!g_hwnd || !IsWindow(g_hwnd)) g_hwnd = FindMainWindowForThisProcess();

    if (g_hwnd) {
        InstallWndProc(g_hwnd);
        GetWindowRect(g_hwnd, &g_windowedRect);

        if (g_cfg.startBorderless) ApplyBorderless(g_hwnd);
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

    // IDirect3D9 vtable:
    //   CreateDevice = 16
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

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
