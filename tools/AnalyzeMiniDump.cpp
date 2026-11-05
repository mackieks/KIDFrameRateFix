#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <vector>

struct ModuleRange {
    ULONG64 begin;
    ULONG64 end;
    const wchar_t* name;
};

static const ModuleRange* FindModule(const std::vector<ModuleRange>& modules, ULONG64 address) {
    for (const auto& module : modules) {
        if (address >= module.begin && address < module.end) return &module;
    }
    return nullptr;
}

static const BYTE* FindMemory(const BYTE* dumpBase, const MINIDUMP_MEMORY64_LIST* memory,
                              ULONG64 address, ULONG64* available) {
    ULONG64 rva = memory->BaseRva;
    for (ULONG64 i = 0; i < memory->NumberOfMemoryRanges; ++i) {
        const auto& range = memory->MemoryRanges[i];
        const ULONG64 start = range.StartOfMemoryRange;
        const ULONG64 size = range.DataSize;
        if (address >= start && address < start + size) {
            const ULONG64 offset = address - start;
            *available = size - offset;
            return dumpBase + rva + offset;
        }
        rva += size;
    }
    return nullptr;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        std::fwprintf(stderr, L"usage: AnalyzeMiniDump <dump.dmp> [thread-id]\n");
        return 2;
    }
    const DWORD requestedTid = argc == 3 ? std::wcstoul(argv[2], nullptr, 10) : 0;

    HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"CreateFile failed: %lu\n", GetLastError());
        return 3;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        std::fwprintf(stderr, L"CreateFileMapping failed: %lu\n", GetLastError());
        CloseHandle(file);
        return 4;
    }
    const BYTE* base = static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (base == nullptr) {
        std::fwprintf(stderr, L"MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        CloseHandle(file);
        return 5;
    }

    PMINIDUMP_DIRECTORY directory = nullptr;
    void* stream = nullptr;
    ULONG streamSize = 0;
    if (!MiniDumpReadDumpStream(const_cast<BYTE*>(base), ModuleListStream, &directory,
                                &stream, &streamSize)) {
        std::fwprintf(stderr, L"No module list: %lu\n", GetLastError());
        return 6;
    }
    const auto* moduleList = static_cast<const MINIDUMP_MODULE_LIST*>(stream);
    std::vector<ModuleRange> modules;
    modules.reserve(moduleList->NumberOfModules);
    for (ULONG i = 0; i < moduleList->NumberOfModules; ++i) {
        const auto& item = moduleList->Modules[i];
        const auto* string = reinterpret_cast<const MINIDUMP_STRING*>(base + item.ModuleNameRva);
        modules.push_back({item.BaseOfImage, item.BaseOfImage + item.SizeOfImage, string->Buffer});
    }

    const MINIDUMP_MEMORY64_LIST* memory64 = nullptr;
    stream = nullptr;
    if (MiniDumpReadDumpStream(const_cast<BYTE*>(base), Memory64ListStream, &directory,
                               &stream, &streamSize)) {
        memory64 = static_cast<const MINIDUMP_MEMORY64_LIST*>(stream);
    }

    stream = nullptr;
    if (MiniDumpReadDumpStream(const_cast<BYTE*>(base), SystemInfoStream, &directory,
                               &stream, &streamSize)) {
        const auto* systemInfo = static_cast<const MINIDUMP_SYSTEM_INFO*>(stream);
        std::wprintf(L"ProcessorArchitecture=%u\n", systemInfo->ProcessorArchitecture);
    }

    stream = nullptr;
    if (!MiniDumpReadDumpStream(const_cast<BYTE*>(base), ThreadListStream, &directory,
                                &stream, &streamSize)) {
        std::fwprintf(stderr, L"No thread list: %lu\n", GetLastError());
        return 7;
    }
    const auto* threadList = static_cast<const MINIDUMP_THREAD_LIST*>(stream);
    std::wprintf(L"Threads: %lu; modules: %lu\n", threadList->NumberOfThreads,
                 moduleList->NumberOfModules);

    for (ULONG i = 0; i < threadList->NumberOfThreads; ++i) {
        const auto& thread = threadList->Threads[i];
        if (requestedTid != 0 && thread.ThreadId != requestedTid) continue;
        if (thread.ThreadContext.DataSize < sizeof(CONTEXT)) {
            std::wprintf(L"TID %lu context too small: %lu\n", thread.ThreadId,
                         thread.ThreadContext.DataSize);
            continue;
        }
        const auto* context = reinterpret_cast<const CONTEXT*>(
            base + thread.ThreadContext.Rva);
        const ULONG64 pc = context->Rip;
        const ULONG64 sp = context->Rsp;
        const ULONG64 bp = context->Rbp;
        const ModuleRange* pcModule = FindModule(modules, pc);
        if (pcModule != nullptr) {
            std::wprintf(L"TID %lu PC=%08llX %ls+0x%llX SP=%08llX BP=%08llX context=%lu\n",
                         thread.ThreadId, pc, pcModule->name,
                         pc - pcModule->begin, sp, bp, thread.ThreadContext.DataSize);
        } else {
            std::wprintf(L"TID %lu PC=%08llX SP=%08llX BP=%08llX context=%lu\n",
                         thread.ThreadId, pc, sp, bp, thread.ThreadContext.DataSize);
        }
        std::wprintf(L"  AX=%08llX BX=%08llX CX=%08llX DX=%08llX SI=%08llX DI=%08llX\n",
                     context->Rax, context->Rbx, context->Rcx, context->Rdx,
                     context->Rsi, context->Rdi);
        if (requestedTid != 0) {
            const auto* raw = reinterpret_cast<const DWORD*>(base + thread.ThreadContext.Rva);
            const ULONG dwordCount = std::min<ULONG>(thread.ThreadContext.DataSize / 4, 220);
            std::wprintf(L"  Raw context DWORDs:\n");
            for (ULONG word = 0; word < dwordCount; word += 8) {
                std::wprintf(L"    +0x%03lX ", word * 4);
                for (ULONG j = 0; j < 8 && word + j < dwordCount; ++j)
                    std::wprintf(L"%08lX ", raw[word + j]);
                std::wprintf(L"\n");
            }
        }

        const BYTE* stack = nullptr;
        ULONG64 stackBytes = 0;
        if (thread.Stack.Memory.Rva != 0 && thread.Stack.Memory.DataSize != 0) {
            stack = base + thread.Stack.Memory.Rva;
            stackBytes = thread.Stack.Memory.DataSize;
        } else if (memory64 != nullptr) {
            stack = FindMemory(base, memory64, sp, &stackBytes);
        }
        if (stack == nullptr) {
            std::wprintf(L"  Stack memory unavailable.\n");
            continue;
        }
        stackBytes = std::min<ULONG64>(stackBytes, 128 * 1024);
        std::wprintf(L"  Potential code addresses (%llu stack bytes):\n", stackBytes);
        int emitted = 0;
        for (ULONG64 offset = 0; offset + sizeof(DWORD) <= stackBytes && emitted < 240;
             offset += sizeof(DWORD)) {
            DWORD value = 0;
            memcpy(&value, stack + offset, sizeof(value));
            const ModuleRange* module = FindModule(modules, value);
            if (module == nullptr) continue;
            std::wprintf(L"    ESP+0x%05llX %08lX %ls+0x%llX\n", offset, value,
                         module->name, value - module->begin);
            ++emitted;
        }
    }

    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return 0;
}
