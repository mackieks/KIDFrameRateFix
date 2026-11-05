#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct ModuleRange {
    uintptr_t begin;
    uintptr_t end;
    wchar_t name[MAX_PATH];
};

static const ModuleRange* FindModule(const std::vector<ModuleRange>& modules, uintptr_t address) {
    for (const auto& module : modules) {
        if (address >= module.begin && address < module.end) return &module;
    }
    return nullptr;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: CaptureThreadContext <pid> <tid>\n");
        return 2;
    }

    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
    const DWORD tid = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));

    std::vector<ModuleRange> modules;
    HANDLE moduleSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (moduleSnapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(moduleSnapshot, &entry)) {
            do {
                ModuleRange module = {};
                module.begin = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                module.end = module.begin + entry.modBaseSize;
                wcsncpy_s(module.name, entry.szModule, _TRUNCATE);
                modules.push_back(module);
            } while (Module32NextW(moduleSnapshot, &entry));
        }
        CloseHandle(moduleSnapshot);
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, tid);
    if (process == nullptr || thread == nullptr) {
        std::fwprintf(stderr, L"OpenProcess/OpenThread failed: %lu\n", GetLastError());
        if (process != nullptr) CloseHandle(process);
        if (thread != nullptr) CloseHandle(thread);
        return 3;
    }

    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        std::fwprintf(stderr, L"SuspendThread failed: %lu\n", GetLastError());
        CloseHandle(thread);
        CloseHandle(process);
        return 4;
    }

    CONTEXT context = {};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    const BOOL gotContext = GetThreadContext(thread, &context);
    if (!gotContext) {
        std::fwprintf(stderr, L"GetThreadContext failed: %lu\n", GetLastError());
        ResumeThread(thread);
        CloseHandle(thread);
        CloseHandle(process);
        return 5;
    }

    const ModuleRange* pcModule = FindModule(modules, context.Eip);
    if (pcModule != nullptr) {
        std::wprintf(L"TID %lu EIP=%08lX %ls+0x%lX ESP=%08lX EBP=%08lX\n",
                     tid, context.Eip, pcModule->name,
                     static_cast<DWORD>(context.Eip - pcModule->begin), context.Esp, context.Ebp);
    } else {
        std::wprintf(L"TID %lu EIP=%08lX ESP=%08lX EBP=%08lX\n",
                     tid, context.Eip, context.Esp, context.Ebp);
    }
    std::wprintf(L"EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX\n",
                 context.Eax, context.Ebx, context.Ecx, context.Edx,
                 context.Esi, context.Edi);

    constexpr SIZE_T kStackBytes = 64 * 1024;
    std::vector<BYTE> stack(kStackBytes);
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(process, reinterpret_cast<const void*>(context.Esp), stack.data(),
                          stack.size(), &bytesRead)) {
        std::wprintf(L"Potential code addresses on stack (%Iu bytes read):\n", bytesRead);
        int emitted = 0;
        for (SIZE_T offset = 0; offset + sizeof(DWORD) <= bytesRead && emitted < 200;
             offset += sizeof(DWORD)) {
            DWORD value = 0;
            memcpy(&value, stack.data() + offset, sizeof(value));
            const ModuleRange* module = FindModule(modules, value);
            if (module == nullptr) continue;
            std::wprintf(L"  ESP+0x%04IX  %08lX  %ls+0x%lX\n", offset, value,
                         module->name, static_cast<DWORD>(value - module->begin));
            ++emitted;
        }
    } else {
        std::fwprintf(stderr, L"ReadProcessMemory(stack) failed: %lu\n", GetLastError());
    }

    ResumeThread(thread);
    CloseHandle(thread);
    CloseHandle(process);
    return 0;
}
