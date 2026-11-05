// Startup resolution. UE3 accepts ResX=/ResY= on its command line, and that
// overrides the [SystemSettings] values in KidEngine.ini. The stock INI opens
// the game at 1280x720 and the options menu tops out at 1920x1080, so the mod
// appends ResX/ResY for the desktop resolution (or a configured size) to the
// process command line before the game reads it.
//
// The command line is read through GetCommandLineW/A, which return buffers
// cached by KernelBase, and through the PEB. All three are replaced with an
// extended copy. This runs from DllMain, before the executable's CRT startup
// and WinMain, when the dinput8 proxy is used; as an .asi it runs before
// WinMain, which is where UE3 reads the command line.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <intrin.h>

#include <cstdio>
#include <cwchar>

void KidLog(const wchar_t* format, ...);

namespace {

struct CountedAnsiString {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
};

// Finds a UNICODE_STRING / ANSI_STRING in a module's writable sections whose
// Buffer points at `buffer`.
template <typename StringType>
StringType* FindCountedString(HMODULE module, const void* buffer) {
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0) {
            continue;
        }
        BYTE* begin = base + section->VirtualAddress;
        const SIZE_T size = section->Misc.VirtualSize;
        for (SIZE_T offset = 0; offset + sizeof(StringType) <= size; offset += sizeof(void*)) {
            auto* candidate = reinterpret_cast<StringType*>(begin + offset);
            if (candidate->Buffer == buffer && candidate->Length != 0 &&
                candidate->MaximumLength >= candidate->Length) {
                return candidate;
            }
        }
    }
    return nullptr;
}

bool HasArgument(const wchar_t* commandLine, const wchar_t* name) {
    // Case-insensitive search for "ResX=" etc. anywhere in the command line.
    const size_t length = wcslen(name);
    for (const wchar_t* cursor = commandLine; *cursor != L'\0'; ++cursor) {
        if (_wcsnicmp(cursor, name, length) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

// mode: 0 = leave the game's resolution alone, 1 = desktop resolution,
// 2 = the configured width/height.
void ApplyStartupResolution(int mode, int width, int height) {
    if (mode == 0) {
        return;
    }
    const wchar_t* current = GetCommandLineW();
    if (current == nullptr) {
        return;
    }
    if (HasArgument(current, L"ResX=") || HasArgument(current, L"ResY=")) {
        KidLog(L"Resolution: ResX/ResY already on the command line; leaving them unchanged.");
        return;
    }
    if (mode == 1) {
        DEVMODEW display = {};
        display.dmSize = sizeof(display);
        if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &display) ||
            display.dmPelsWidth < 640 || display.dmPelsHeight < 480) {
            KidLog(L"Resolution: could not read the desktop resolution; using the game's setting.");
            return;
        }
        width = static_cast<int>(display.dmPelsWidth);
        height = static_cast<int>(display.dmPelsHeight);
    }
    if (width < 640 || height < 480 || width > 16384 || height > 16384) {
        KidLog(L"Resolution: %dx%d is out of range; using the game's setting.", width, height);
        return;
    }

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    const char* currentAnsi = GetCommandLineA();
    auto* wideCached = kernelBase ? FindCountedString<UNICODE_STRING>(kernelBase, current) : nullptr;
    auto* ansiCached = kernelBase ? FindCountedString<CountedAnsiString>(kernelBase, currentAnsi) : nullptr;
    if (wideCached == nullptr || ansiCached == nullptr) {
        KidLog(L"Resolution: command-line storage not found; using the game's setting.");
        return;
    }

    // Allocated once for the lifetime of the process, as the originals are.
    wchar_t suffix[64] = {};
    _snwprintf_s(suffix, sizeof(suffix) / sizeof(suffix[0]), _TRUNCATE, L" ResX=%d ResY=%d", width, height);
    const size_t wideLength = wcslen(current) + wcslen(suffix);
    auto* wide = static_cast<wchar_t*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (wideLength + 1) * sizeof(wchar_t)));
    auto* ansi = static_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, wideLength * 3 + 1));
    if (wide == nullptr || ansi == nullptr || wideLength * sizeof(wchar_t) > 0xFFF0) {
        return;
    }
    wcscpy_s(wide, wideLength + 1, current);
    wcscat_s(wide, wideLength + 1, suffix);
    const int ansiBytes = WideCharToMultiByte(CP_ACP, 0, wide, -1, ansi,
                                              static_cast<int>(wideLength * 3 + 1), nullptr, nullptr);
    if (ansiBytes <= 0) {
        return;
    }

    wideCached->Buffer = wide;
    wideCached->Length = static_cast<USHORT>(wideLength * sizeof(wchar_t));
    wideCached->MaximumLength = static_cast<USHORT>((wideLength + 1) * sizeof(wchar_t));
    ansiCached->Buffer = ansi;
    ansiCached->Length = static_cast<USHORT>(ansiBytes - 1);
    ansiCached->MaximumLength = static_cast<USHORT>(ansiBytes);

    // Keep the PEB copy consistent for anything that reads it directly.
    PEB* peb = reinterpret_cast<PEB*>(__readfsdword(0x30));
    if (peb != nullptr && peb->ProcessParameters != nullptr) {
        peb->ProcessParameters->CommandLine.Buffer = wide;
        peb->ProcessParameters->CommandLine.Length = wideCached->Length;
        peb->ProcessParameters->CommandLine.MaximumLength = wideCached->MaximumLength;
    }
    KidLog(L"Resolution: starting at %dx%d (%s).", width, height,
           mode == 1 ? L"desktop resolution" : L"configured");
}
