// [Diagnostics] CrashDump: on the first access violation raised by code inside
// KidGame.exe, write a full-memory minidump (KIDFrameRateFix-crash.dmp beside
// the DLL) before the engine's own handler runs. Diagnostic only; off by
// default. A vectored handler sees the exception before UE3's __try blocks, so
// the dump captures the faulting state even when the engine's crash path
// would otherwise terminate the process.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

void KidLog(const wchar_t* format, ...);

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

MiniDumpWriteDumpFn g_writeDump = nullptr;
BYTE* g_codeBegin = nullptr;
BYTE* g_codeEnd = nullptr;
wchar_t g_dumpPath[MAX_PATH] = {};
volatile LONG g_dumped = 0;

struct DumpRequest {
    EXCEPTION_POINTERS* pointers;
    DWORD threadId;
};

DWORD WINAPI WriteDumpThread(void* parameter) {
    auto* request = static_cast<DumpRequest*>(parameter);
    HANDLE file = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 1;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception = {};
    exception.ThreadId = request->threadId;
    exception.ExceptionPointers = request->pointers;
    exception.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules);
    const BOOL ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                &exception, nullptr, nullptr);
    CloseHandle(file);
    return ok ? 0 : 2;
}

LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    auto* address = static_cast<BYTE*>(record->ExceptionAddress);
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        address < g_codeBegin || address >= g_codeEnd ||
        InterlockedExchange(&g_dumped, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Write from a separate thread: MiniDumpWriteDump must not run on the
    // faulting thread, which waits here until the dump is complete.
    DumpRequest request = {pointers, GetCurrentThreadId()};
    HANDLE thread = CreateThread(nullptr, 0, WriteDumpThread, &request, 0, nullptr);
    if (thread != nullptr) {
        WaitForSingleObject(thread, 120000);
        DWORD result = 0;
        GetExitCodeThread(thread, &result);
        CloseHandle(thread);
        KidLog(L"Crash dump: access violation at KidGame.exe+0x%X reading 0x%p; full dump %s (%lu).",
               static_cast<unsigned>(address - g_codeBegin + 0x1000),
               reinterpret_cast<void*>(record->ExceptionInformation[1]),
               result == 0 ? L"written" : L"FAILED", result);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void CrashDumpInstall(BYTE* textBegin, SIZE_T textSize, const wchar_t* dumpPath) {
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    g_writeDump = dbghelp != nullptr
        ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
        : nullptr;
    if (g_writeDump == nullptr) {
        KidLog(L"Crash dump: MiniDumpWriteDump unavailable; diagnostic inactive.");
        return;
    }
    g_codeBegin = textBegin;
    g_codeEnd = textBegin + textSize;
    lstrcpynW(g_dumpPath, dumpPath, MAX_PATH);
    AddVectoredExceptionHandler(1, CrashHandler);
    KidLog(L"Crash dump armed: a full dump is written on the first access violation in KidGame.exe.");
}
