// ThreadSampler: statistical profiler for a running KidGame.exe.
//
// Repeatedly suspends each thread, records its instruction pointer and the
// first return addresses on its stack that land in KidGame.exe, and prints
// the hottest locations per thread. Used to find where the game and render
// threads wait each frame.
//
//   ThreadSampler <seconds> [samplesPerSecond]
//
// Build (x86): cl /nologo /O2 /EHsc ThreadSampler.cpp psapi.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

struct Module {
    uint32_t base;
    uint32_t size;
    std::string name;
};

std::vector<Module> g_modules;

std::string Describe(uint32_t address) {
    for (const Module& module : g_modules) {
        if (address >= module.base && address < module.base + module.size) {
            char text[160];
            std::snprintf(text, sizeof(text), "%s+0x%X", module.name.c_str(), address - module.base);
            return text;
        }
    }
    char text[32];
    std::snprintf(text, sizeof(text), "0x%08X", address);
    return text;
}

bool InGame(uint32_t address) {
    return !g_modules.empty() && address >= g_modules[0].base &&
           address < g_modules[0].base + g_modules[0].size;
}

}  // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 10.0;
    const int rate = argc > 2 ? std::atoi(argv[2]) : 500;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry = {sizeof(entry)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"KidGame.exe") == 0) pid = entry.th32ProcessID;
    }
    CloseHandle(snapshot);
    if (pid == 0) {
        std::fprintf(stderr, "KidGame.exe is not running\n");
        return 1;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    HMODULE modules[1024];
    DWORD needed = 0;
    EnumProcessModules(process, modules, sizeof(modules), &needed);
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
        MODULEINFO info = {};
        char name[MAX_PATH] = {};
        GetModuleInformation(process, modules[i], &info, sizeof(info));
        GetModuleBaseNameA(process, modules[i], name, sizeof(name));
        g_modules.push_back({static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info.lpBaseOfDll)),
                             info.SizeOfImage, name});
    }

    std::vector<DWORD> threadIds;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 thread = {sizeof(thread)};
    for (BOOL ok = Thread32First(snapshot, &thread); ok; ok = Thread32Next(snapshot, &thread)) {
        if (thread.th32OwnerProcessID == pid) threadIds.push_back(thread.th32ThreadID);
    }
    CloseHandle(snapshot);

    struct ThreadStats {
        HANDLE handle;
        FILETIME lastKernel, lastUser;
        double cpuSeconds = 0;
        unsigned samples = 0;
        std::map<std::string, unsigned> where;
    };
    std::map<DWORD, ThreadStats> stats;
    for (DWORD id : threadIds) {
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, id);
        if (handle != nullptr) {
            ThreadStats& s = stats[id];
            s.handle = handle;
            FILETIME a, b;
            GetThreadTimes(handle, &a, &b, &s.lastKernel, &s.lastUser);
        }
    }

    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000);
    while (GetTickCount64() < end) {
        for (auto& pair : stats) {
            ThreadStats& s = pair.second;
            if (SuspendThread(s.handle) == static_cast<DWORD>(-1)) continue;
            CONTEXT context = {};
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(s.handle, &context)) {
                std::string key = Describe(context.Eip);
                // First two return addresses into KidGame.exe identify the caller.
                uint32_t stack[256] = {};
                SIZE_T got = 0;
                ReadProcessMemory(process, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(context.Esp)),
                                  stack, sizeof(stack), &got);
                int found = 0;
                for (size_t i = 0; i < got / 4 && found < 2; ++i) {
                    if (InGame(stack[i])) {
                        key += " <- " + Describe(stack[i]);
                        ++found;
                    }
                }
                ++s.where[key];
                ++s.samples;
            }
            ResumeThread(s.handle);
        }
        Sleep(1000 / rate > 0 ? 1000 / rate : 1);
    }

    for (auto& pair : stats) {
        ThreadStats& s = pair.second;
        FILETIME a, b, kernel, user;
        GetThreadTimes(s.handle, &a, &b, &kernel, &user);
        auto toSeconds = [](FILETIME t) {
            return (static_cast<uint64_t>(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 1e7;
        };
        s.cpuSeconds = toSeconds(kernel) - toSeconds(s.lastKernel) + toSeconds(user) - toSeconds(s.lastUser);
    }
    std::vector<std::pair<DWORD, ThreadStats*>> order;
    for (auto& pair : stats) order.push_back({pair.first, &pair.second});
    std::sort(order.begin(), order.end(),
              [](auto& x, auto& y) { return x.second->cpuSeconds > y.second->cpuSeconds; });
    for (size_t t = 0; t < order.size() && t < 6; ++t) {
        ThreadStats& s = *order[t].second;
        std::printf("=== thread %lu: CPU %.0f%% of one core, %u samples\n", order[t].first,
                    100.0 * s.cpuSeconds / seconds, s.samples);
        std::vector<std::pair<unsigned, std::string>> top;
        for (auto& w : s.where) top.push_back({w.second, w.first});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 8; ++i) {
            std::printf("  %5.1f%%  %s\n", 100.0 * top[i].first / s.samples, top[i].second.c_str());
        }
    }
    return 0;
}
