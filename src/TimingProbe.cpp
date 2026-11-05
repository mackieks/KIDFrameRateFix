// [Diagnostics] TimingProbe: a 10-second census of KidGame.exe's timer and
// wait calls. The executable's import table entries for
// QueryPerformanceCounter, Sleep, WaitForSingleObject(Ex),
// WaitForMultipleObjects and timeGetTime are pointed at counting wrappers,
// which record each call's return address, thread, count and time spent
// waiting. The report is written to the log, and the original import table
// entries are restored. Diagnostic only; off by default.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>

#pragma intrinsic(_ReturnAddress)

void KidLog(const wchar_t* format, ...);

namespace {

enum Kind : uint32_t { kQpc, kSleep, kWaitOne, kWaitOneEx, kWaitMany, kTimeGetTime, kKindCount };
const wchar_t* const kKindNames[kKindCount] = {
    L"QueryPerformanceCounter", L"Sleep", L"WaitForSingleObject",
    L"WaitForSingleObjectEx", L"WaitForMultipleObjects", L"timeGetTime"};

struct Site {
    volatile LONG key;      // return address; 0 = empty
    volatile LONG kind;
    volatile LONG thread;
    volatile LONG count;
    volatile LONG64 waitTicks;
    volatile LONG64 timeoutMs;
};

constexpr SIZE_T kSiteSlots = 8192;
Site g_sites[kSiteSlots] = {};
volatile LONG g_active = 0;
LONGLONG g_frequency = 0;
BYTE* g_base = nullptr;

using QpcFn = BOOL(WINAPI*)(LARGE_INTEGER*);
using SleepFn = void(WINAPI*)(DWORD);
using WaitOneFn = DWORD(WINAPI*)(HANDLE, DWORD);
using WaitOneExFn = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
using WaitManyFn = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD);
using TimeGetTimeFn = DWORD(WINAPI*)();

QpcFn g_qpc = nullptr;
SleepFn g_sleep = nullptr;
WaitOneFn g_waitOne = nullptr;
WaitOneExFn g_waitOneEx = nullptr;
WaitManyFn g_waitMany = nullptr;
TimeGetTimeFn g_timeGetTime = nullptr;

LONGLONG Now() {
    LARGE_INTEGER value;
    g_qpc(&value);
    return value.QuadPart;
}

void Record(void* returnAddress, Kind kind, LONGLONG waitTicks, DWORD timeoutMs) {
    if (g_active == 0) {
        return;
    }
    const LONG key = static_cast<LONG>(reinterpret_cast<uintptr_t>(returnAddress));
    SIZE_T slot = ((static_cast<uint32_t>(key) >> 2) * 2654435761u + kind) & (kSiteSlots - 1);
    for (SIZE_T probe = 0; probe < 64; ++probe) {
        Site& site = g_sites[(slot + probe) & (kSiteSlots - 1)];
        LONG existing = site.key;
        if (existing == 0) {
            existing = InterlockedCompareExchange(&site.key, key, 0);
            if (existing == 0) {
                site.kind = kind;
                existing = key;
            }
        }
        if (existing == key && site.kind == static_cast<LONG>(kind)) {
            site.thread = static_cast<LONG>(GetCurrentThreadId());
            InterlockedIncrement(&site.count);
            if (waitTicks != 0) InterlockedExchangeAdd64(&site.waitTicks, waitTicks);
            if (timeoutMs != INFINITE) InterlockedExchangeAdd64(&site.timeoutMs, timeoutMs);
            return;
        }
    }
}

BOOL WINAPI ProbeQpc(LARGE_INTEGER* value) {
    Record(_ReturnAddress(), kQpc, 0, INFINITE);
    return g_qpc(value);
}

void WINAPI ProbeSleep(DWORD ms) {
    void* caller = _ReturnAddress();
    const LONGLONG start = Now();
    g_sleep(ms);
    Record(caller, kSleep, Now() - start, ms);
}

DWORD WINAPI ProbeWaitOne(HANDLE handle, DWORD ms) {
    void* caller = _ReturnAddress();
    const LONGLONG start = Now();
    const DWORD result = g_waitOne(handle, ms);
    Record(caller, kWaitOne, Now() - start, ms);
    return result;
}

DWORD WINAPI ProbeWaitOneEx(HANDLE handle, DWORD ms, BOOL alertable) {
    void* caller = _ReturnAddress();
    const LONGLONG start = Now();
    const DWORD result = g_waitOneEx(handle, ms, alertable);
    Record(caller, kWaitOneEx, Now() - start, ms);
    return result;
}

DWORD WINAPI ProbeWaitMany(DWORD count, const HANDLE* handles, BOOL all, DWORD ms) {
    void* caller = _ReturnAddress();
    const LONGLONG start = Now();
    const DWORD result = g_waitMany(count, handles, all, ms);
    Record(caller, kWaitMany, Now() - start, ms);
    return result;
}

DWORD WINAPI ProbeTimeGetTime() {
    Record(_ReturnAddress(), kTimeGetTime, 0, INFINITE);
    return g_timeGetTime();
}

struct Patch {
    ptrdiff_t rva;
    void* replacement;
    void** original;
};

// Import address table slots in the supported KidGame.exe.
Patch g_patches[] = {
    {0x2E97350, reinterpret_cast<void*>(&ProbeQpc), reinterpret_cast<void**>(&g_qpc)},
    {0x2E97300, reinterpret_cast<void*>(&ProbeSleep), reinterpret_cast<void**>(&g_sleep)},
    {0x2E97168, reinterpret_cast<void*>(&ProbeWaitOne), reinterpret_cast<void**>(&g_waitOne)},
    {0x2E972A0, reinterpret_cast<void*>(&ProbeWaitOneEx), reinterpret_cast<void**>(&g_waitOneEx)},
    {0x2E97250, reinterpret_cast<void*>(&ProbeWaitMany), reinterpret_cast<void**>(&g_waitMany)},
    {0x2E97B9C, reinterpret_cast<void*>(&ProbeTimeGetTime), reinterpret_cast<void**>(&g_timeGetTime)},
};

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

}  // namespace

// Installs the import hooks; recording starts with TimingProbeStart().
bool TimingProbeInstall(BYTE* executableBase) {
    g_base = executableBase;
    // Each slot's current target is the original; calling through it keeps
    // any other hook already present in the chain.
    for (Patch& patch : g_patches) {
        auto** slot = reinterpret_cast<void**>(executableBase + patch.rva);
        MEMORY_BASIC_INFORMATION info = {};
        if (*slot == nullptr || VirtualQuery(*slot, &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT) {
            KidLog(L"Timing probe: import slot +0x%X is not valid; probe not installed.",
                   static_cast<unsigned>(patch.rva));
            return false;
        }
        *patch.original = *slot;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_frequency = frequency.QuadPart;
    for (Patch& patch : g_patches) {
        WriteSlot(reinterpret_cast<void**>(executableBase + patch.rva), patch.replacement);
    }
    KidLog(L"Timing probe installed; it records for 10 s once the first UI frame is reached.");
    return true;
}

void TimingProbeStart() {
    InterlockedExchange(&g_active, 1);
}

void TimingProbeFinish(double seconds, DWORD gameThread) {
    InterlockedExchange(&g_active, 0);
    for (Patch& patch : g_patches) {
        WriteSlot(reinterpret_cast<void**>(g_base + patch.rva), *patch.original);
    }
    // Copy and sort: top sites by wait time, then by call count.
    static Site sites[kSiteSlots];
    SIZE_T used = 0;
    for (SIZE_T index = 0; index < kSiteSlots; ++index) {
        if (g_sites[index].key != 0) sites[used++] = g_sites[index];
    }
    auto report = [&](bool byWait) {
        bool taken[kSiteSlots] = {};
        for (int rank = 0; rank < 15; ++rank) {
            SIZE_T best = SIZE_MAX;
            for (SIZE_T index = 0; index < used; ++index) {
                if (taken[index]) continue;
                const LONG64 value = byWait ? sites[index].waitTicks : sites[index].count;
                const LONG64 bestValue = best == SIZE_MAX ? -1
                    : (byWait ? sites[best].waitTicks : sites[best].count);
                if (value > bestValue) best = index;
            }
            if (best == SIZE_MAX) break;
            taken[best] = true;
            const Site& site = sites[best];
            if (byWait && site.waitTicks == 0) break;
            const double waitMs = 1000.0 * site.waitTicks / g_frequency;
            KidLog(L"  %-24s from KidGame.exe+0x%07X thread %5ld%s: %8ld calls/%.0fs (%7.1f/s), waited %8.1f ms (%.3f ms/call), mean timeout %.2f ms",
                   kKindNames[site.kind],
                   static_cast<unsigned>(site.key - static_cast<LONG>(reinterpret_cast<uintptr_t>(g_base))),
                   site.thread, static_cast<DWORD>(site.thread) == gameThread ? L" (game)" : L"",
                   site.count, seconds, site.count / seconds, waitMs,
                   site.count ? waitMs / site.count : 0.0,
                   site.count ? static_cast<double>(site.timeoutMs) / site.count : 0.0);
        }
    };
    KidLog(L"Timing probe report (%.0f s), top waits:", seconds);
    report(true);
    KidLog(L"Timing probe report, most frequent calls:");
    report(false);
    KidLog(L"Timing probe removed.");
}
