#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>

static const BYTE* FindMemory(const BYTE* dumpBase, const MINIDUMP_MEMORY64_LIST* memory,
                              ULONG64 address, ULONG64* available) {
    ULONG64 rva = memory->BaseRva;
    for (ULONG64 i = 0; i < memory->NumberOfMemoryRanges; ++i) {
        const auto& range = memory->MemoryRanges[i];
        if (address >= range.StartOfMemoryRange &&
            address < range.StartOfMemoryRange + range.DataSize) {
            const ULONG64 offset = address - range.StartOfMemoryRange;
            *available = range.DataSize - offset;
            return dumpBase + rva + offset;
        }
        rva += range.DataSize;
    }
    return nullptr;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 4) {
        std::fwprintf(stderr, L"usage: ReadMiniDumpMemory <dump.dmp> <hex-address> [byte-count]\n");
        return 2;
    }
    const ULONG64 address = _wcstoui64(argv[2], nullptr, 0);
    const ULONG64 requested = argc == 4 ? _wcstoui64(argv[3], nullptr, 0) : 256;

    HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 3;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) return 4;
    const BYTE* base = static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!base) return 5;

    PMINIDUMP_DIRECTORY directory = nullptr;
    void* stream = nullptr;
    ULONG streamSize = 0;
    if (!MiniDumpReadDumpStream(const_cast<BYTE*>(base), Memory64ListStream, &directory,
                                &stream, &streamSize)) {
        std::fwprintf(stderr, L"No Memory64ListStream\n");
        return 6;
    }
    const auto* memory = static_cast<const MINIDUMP_MEMORY64_LIST*>(stream);
    ULONG64 available = 0;
    const BYTE* data = FindMemory(base, memory, address, &available);
    if (!data) {
        std::fwprintf(stderr, L"Address not present in dump\n");
        return 7;
    }
    const ULONG64 count = std::min(requested, available);
    for (ULONG64 offset = 0; offset < count; offset += 16) {
        std::wprintf(L"%08llX  ", address + offset);
        for (ULONG64 i = 0; i < 16; ++i) {
            if (offset + i < count) std::wprintf(L"%02X ", data[offset + i]);
            else std::wprintf(L"   ");
        }
        std::wprintf(L" ");
        for (ULONG64 i = 0; i < 16 && offset + i < count; ++i) {
            const BYTE c = data[offset + i];
            std::wprintf(L"%lc", c >= 32 && c < 127 ? static_cast<wchar_t>(c) : L'.');
        }
        std::wprintf(L"\n");
    }
    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return 0;
}
