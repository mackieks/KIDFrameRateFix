// Standalone loading: KidGame.exe statically imports DINPUT8.dll, so this
// plugin can be installed as Binaries/Win32/dinput8.dll and is then loaded by
// Windows before the game starts, with no ASI loader. The real DirectInput
// entry points are forwarded to the system copy of dinput8.dll.
//
// The same binary may instead be renamed to KIDFrameRateFix.asi for use with
// an ASI loader; the exports below are then simply unused.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

namespace {

HMODULE g_systemDInput8 = nullptr;
INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadSystemDInput8(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length + 14 >= MAX_PATH) {
        return TRUE;
    }
    lstrcatW(path, L"\\dinput8.dll");
    g_systemDInput8 = LoadLibraryW(path);
    return TRUE;
}

FARPROC SystemExport(const char* name) {
    InitOnceExecuteOnce(&g_loadOnce, LoadSystemDInput8, nullptr, nullptr);
    return g_systemDInput8 != nullptr ? GetProcAddress(g_systemDInput8, name) : nullptr;
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid,
                                        LPVOID* output, LPUNKNOWN outer) {
    using Function = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto function = reinterpret_cast<Function>(SystemExport("DirectInput8Create"));
    if (function == nullptr) {
        if (output != nullptr) {
            *output = nullptr;
        }
        return E_FAIL;
    }
    return function(instance, version, iid, output, outer);
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    using Function = HRESULT(WINAPI*)();
    auto function = reinterpret_cast<Function>(SystemExport("DllCanUnloadNow"));
    return function != nullptr ? function() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* output) {
    using Function = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto function = reinterpret_cast<Function>(SystemExport("DllGetClassObject"));
    if (function == nullptr) {
        if (output != nullptr) {
            *output = nullptr;
        }
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return function(clsid, iid, output);
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    using Function = HRESULT(WINAPI*)();
    auto function = reinterpret_cast<Function>(SystemExport("DllRegisterServer"));
    return function != nullptr ? function() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    using Function = HRESULT(WINAPI*)();
    auto function = reinterpret_cast<Function>(SystemExport("DllUnregisterServer"));
    return function != nullptr ? function() : E_FAIL;
}

}  // extern "C"
