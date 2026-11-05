#include <windows.h>

#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: MakeUELibPackage <decompressed-input> <output>\n");
        return 2;
    }
    if (!CopyFileW(argv[1], argv[2], FALSE)) {
        std::fwprintf(stderr, L"CopyFile failed: %lu\n", GetLastError());
        return 3;
    }
    HANDLE file = CreateFileW(argv[2], GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 4;
    LARGE_INTEGER offset = {};
    // UE3 package summary compression flags. The decompressor expands all chunks,
    // so UELib must be told that the resulting file is no longer compressed.
    offset.QuadPart = 0x6D;
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return 5;
    }
    const DWORD noCompression = 0;
    DWORD written = 0;
    const BOOL okay = WriteFile(file, &noCompression, sizeof(noCompression), &written, nullptr);
    CloseHandle(file);
    return okay && written == sizeof(noCompression) ? 0 : 6;
}
