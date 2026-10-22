#include <windows.h>

#include <atomic>
#include <cstdint>

#include "game.hpp"
#include "log.hpp"
#include "present.hpp"
#include "stereo.hpp"

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID,
                                              LPVOID*, LPUNKNOWN);
using DllCanUnloadNowFn = HRESULT(WINAPI*)();
using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using GetdfDIJoystickFn = const void*(WINAPI*)();

constexpr DWORD kWatchIntervalMs = 250;
constexpr DWORD kWatchTimeoutMs = 120000;

HMODULE g_real = nullptr;
std::atomic<bool> g_initStarted{false};

FARPROC Real(const char* name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

bool CodeIsDecrypted() {
    using namespace tc2vr::game;
    const uint8_t prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89};
    return memcmp(At<const void*>(rva::RenderSchedulerFrame), prologue,
                  sizeof(prologue)) == 0;
}

void Initialize() {
    if (g_initStarted.exchange(true)) {
        return;
    }
    LOG("initializing, image base 0x%llx",
        static_cast<unsigned long long>(tc2vr::game::Base()));
    if (!tc2vr::game::VerifyBuild()) {
        LOG("unsupported TheCrew2.exe build, mod disabled");
        return;
    }
    bool camera = tc2vr::stereo::InstallCameraHook();
    bool present = tc2vr::present::InstallPresentHook();
    LOG("hooks: camera=%d present=%d", camera, present);
}

// Fallback when the game never calls DirectInput8Create.
DWORD WINAPI WatchThread(LPVOID) {
    for (DWORD waited = 0; waited < kWatchTimeoutMs;
         waited += kWatchIntervalMs) {
        if (g_initStarted) {
            return 0;
        }
        if (CodeIsDecrypted() && tc2vr::game::GameObject()) {
            Initialize();
            return 0;
        }
        Sleep(kWatchIntervalMs);
    }
    LOG("watch thread timed out before the game initialized");
    return 0;
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version,
                                        REFIID iid, LPVOID* out,
                                        LPUNKNOWN outer) {
    Initialize();
    auto fn = reinterpret_cast<DirectInput8CreateFn>(
        Real("DirectInput8Create"));
    return fn ? fn(inst, version, iid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto fn = reinterpret_cast<DllCanUnloadNowFn>(Real("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID iid,
                                       LPVOID* out) {
    auto fn = reinterpret_cast<DllGetClassObjectFn>(
        Real("DllGetClassObject"));
    return fn ? fn(clsid, iid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    auto fn = reinterpret_cast<DllCanUnloadNowFn>(Real("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto fn =
        reinterpret_cast<DllCanUnloadNowFn>(Real("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}

const void* WINAPI Proxy_GetdfDIJoystick() {
    auto fn = reinterpret_cast<GetdfDIJoystickFn>(Real("GetdfDIJoystick"));
    return fn ? fn() : nullptr;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        tc2vr::log::Open();
        LOG("tc2vr loaded");
        HANDLE thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0,
                                     nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
