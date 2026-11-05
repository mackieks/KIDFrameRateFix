#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

class DumpReader {
public:
    ~DumpReader() {
        if (base_) UnmapViewOfFile(base_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }

    bool Open(const wchar_t* path) {
        file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) return false;
        base_ = static_cast<const BYTE*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!base_) return false;
        PMINIDUMP_DIRECTORY directory = nullptr;
        void* stream = nullptr;
        ULONG streamSize = 0;
        if (!MiniDumpReadDumpStream(const_cast<BYTE*>(base_), Memory64ListStream, &directory,
                                    &stream, &streamSize)) return false;
        memory_ = static_cast<const MINIDUMP_MEMORY64_LIST*>(stream);
        return true;
    }

    const BYTE* At(ULONG64 address, size_t count = 1) const {
        ULONG64 rva = memory_->BaseRva;
        for (ULONG64 i = 0; i < memory_->NumberOfMemoryRanges; ++i) {
            const auto& range = memory_->MemoryRanges[i];
            if (address >= range.StartOfMemoryRange &&
                address + count <= range.StartOfMemoryRange + range.DataSize) {
                return base_ + rva + (address - range.StartOfMemoryRange);
            }
            rva += range.DataSize;
        }
        return nullptr;
    }

    template <typename T> bool Read(ULONG64 address, T& value) const {
        const BYTE* p = At(address, sizeof(T));
        if (!p) return false;
        std::memcpy(&value, p, sizeof(T));
        return true;
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const BYTE* base_ = nullptr;
    const MINIDUMP_MEMORY64_LIST* memory_ = nullptr;
};

static constexpr uint32_t kGNames = 0x238A0000;

static std::string Name(const DumpReader& dump, uint32_t index) {
    uint32_t entry = 0;
    if (!dump.Read(kGNames + static_cast<ULONG64>(index) * 4, entry) || !entry) return "<?>";
    const char* text = reinterpret_cast<const char*>(dump.At(entry + 0x10));
    if (!text) return "<?>";
    size_t length = 0;
    while (length < 512 && text[length]) ++length;
    return std::string(text, length);
}

static void Object(const DumpReader& dump, uint32_t address) {
    uint32_t nameIndex = 0, nameNumber = 0, outer = 0, classObject = 0;
    if (!dump.Read(address + 0x2c, nameIndex) || !dump.Read(address + 0x30, nameNumber) ||
        !dump.Read(address + 0x28, outer) || !dump.Read(address + 0x34, classObject)) {
        std::printf("%08X  <unreadable>\n", address);
        return;
    }
    std::string objectName = Name(dump, nameIndex);
    if (nameNumber) objectName += "_" + std::to_string(nameNumber - 1);
    std::string className = "<?>";
    uint32_t classNameIndex = 0;
    if (classObject && dump.Read(classObject + 0x2c, classNameIndex)) className = Name(dump, classNameIndex);
    std::printf("%08X  %-42s class=%-32s outer=%08X\n", address, objectName.c_str(),
                className.c_str(), outer);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fwprintf(stderr, L"usage: InspectUE3Dump <dump.dmp> <hex-object> [...]\n");
        return 2;
    }
    DumpReader dump;
    if (!dump.Open(argv[1])) return 3;
    for (int i = 2; i < argc; ++i) Object(dump, static_cast<uint32_t>(_wcstoui64(argv[i], nullptr, 0)));
    return 0;
}
