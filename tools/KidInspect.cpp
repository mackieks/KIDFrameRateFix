// KidInspect: read-only external inspector for a running KidGame.exe.
//
// Used during development to verify UE3 object layouts, live UnrealScript
// bytecode and engine property offsets without modifying the game.
//
//   KidInspect objects <Name> [Outer]       list live objects named <Name>
//   KidInspect script  <Func> <Outer>       hex-dump a UFunction's Script
//   KidInspect props   <Class>              list a class's properties
//   KidInspect instances <Class>            list objects whose class is <Class>
//   KidInspect read <hexaddr> <bytes>       hex-dump raw memory
//   KidInspect list <count>                 list the first <count> objects
//   KidInspect get <hexobj> <Prop>[,...]    read properties via reflection
//   KidInspect watch <hexpawn>|auto <secs>  sample the player pawn's movement,
//                                           root-motion mode and controller
//                                           state ("auto" follows reloads)
//
// Build (x86): cl /nologo /O2 /EHsc KidInspect.cpp psapi.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kGNamesDataRva = 0x03D52848;
constexpr uint32_t kGNamesCountRva = 0x03D5284C;
constexpr uint32_t kObjectIndexOffset = 0x20;
constexpr uint32_t kObjectOuterOffset = 0x28;
constexpr uint32_t kObjectNameOffset = 0x2C;
constexpr uint32_t kObjectClassOffset = 0x34;
constexpr uint32_t kFieldNextOffset = 0x3C;
constexpr uint32_t kStructSuperOffset = 0x48;
constexpr uint32_t kStructChildrenOffset = 0x4C;
constexpr uint32_t kStructScriptOffset = 0x54;
constexpr uint32_t kPropertyOffsetOffset = 0x68;

HANDLE g_process = nullptr;
uint32_t g_base = 0;
uint32_t g_objects = 0;
int g_objectCount = 0;

template <typename T>
bool Read(uint32_t address, T* value) {
    SIZE_T got = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)),
                             value, sizeof(T), &got) && got == sizeof(T);
}

bool ReadBytes(uint32_t address, void* out, SIZE_T size) {
    SIZE_T got = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)),
                             out, size, &got) && got == size;
}

uint32_t U32(uint32_t address) {
    uint32_t value = 0;
    Read(address, &value);
    return value;
}

std::string NameByIndex(int index) {
    uint32_t names = U32(g_base + kGNamesDataRva);
    int count = static_cast<int>(U32(g_base + kGNamesCountRva));
    if (names == 0 || index < 0 || index >= count) return "<badname>";
    uint32_t entry = U32(names + 4u * static_cast<uint32_t>(index));
    if (entry == 0) return "<null>";
    uint8_t flags = 0;
    Read(entry + 8, &flags);
    if (flags & 1) {
        wchar_t wide[128] = {};
        ReadBytes(entry + 0x10, wide, sizeof(wide) - 2);
        char narrow[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow), nullptr, nullptr);
        return narrow;
    }
    char ansi[128] = {};
    ReadBytes(entry + 0x10, ansi, sizeof(ansi) - 1);
    return ansi;
}

std::string ObjectName(uint32_t object) {
    if (object == 0) return "None";
    int parts[2] = {};
    if (!ReadBytes(object + kObjectNameOffset, parts, sizeof(parts))) return "<unreadable>";
    std::string name = NameByIndex(parts[0]);
    if (parts[1] > 0) name += "_" + std::to_string(parts[1] - 1);
    return name;
}

std::string FullName(uint32_t object) {
    std::string path = ObjectName(object);
    uint32_t outer = U32(object + kObjectOuterOffset);
    for (int depth = 0; outer != 0 && depth < 16; ++depth) {
        path = ObjectName(outer) + "." + path;
        outer = U32(outer + kObjectOuterOffset);
    }
    return ObjectName(U32(object + kObjectClassOffset)) + " " + path;
}

bool AttachToGame() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry = {sizeof(entry)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"KidGame.exe") == 0) pid = entry.th32ProcessID;
    }
    CloseHandle(snapshot);
    if (pid == 0) {
        std::fprintf(stderr, "KidGame.exe is not running\n");
        return false;
    }
    g_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (g_process == nullptr) return false;
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(g_process, modules, sizeof(modules), &needed)) return false;
    g_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(modules[0]));
    return true;
}

// UObject::GObjObjects is a TArray<UObject*> in .data. Locate it by its
// invariant: GObjObjects(i)->Index == i.
bool FindObjectArray() {
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS32 nt = {};
    ReadBytes(g_base, &dos, sizeof(dos));
    ReadBytes(g_base + dos.e_lfanew, &nt, sizeof(nt));
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    ReadBytes(g_base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
                  nt.FileHeader.SizeOfOptionalHeader,
              sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER));
    for (const auto& section : sections) {
        if (std::memcmp(section.Name, ".data", 5) != 0) continue;
        std::vector<uint32_t> data(section.Misc.VirtualSize / 4);
        ReadBytes(g_base + section.VirtualAddress, data.data(), data.size() * 4);
        for (size_t i = 0; i + 2 < data.size(); ++i) {
            uint32_t pointer = data[i];
            int count = static_cast<int>(data[i + 1]);
            int max = static_cast<int>(data[i + 2]);
            if (pointer < 0x10000 || count < 10000 || count > 4000000 || max < count) continue;
            bool good = true;
            for (int k = 1; k < 48 && good; ++k) {
                uint32_t object = 0;
                uint32_t index = 0;
                good = Read(pointer + 4u * k, &object) && object != 0 &&
                       Read(object + kObjectIndexOffset, &index) &&
                       index == static_cast<uint32_t>(k);
            }
            if (good) {
                g_objects = pointer;
                g_objectCount = count;
                std::printf("GObjObjects at KidGame.exe+0x%08X (%d objects)\n",
                            g_base + section.VirtualAddress + static_cast<uint32_t>(i * 4) - g_base,
                            count);
                return true;
            }
        }
    }
    return false;
}

template <typename Fn>
void ForEachObject(Fn fn) {
    std::vector<uint32_t> table(static_cast<size_t>(g_objectCount));
    ReadBytes(g_objects, table.data(), table.size() * 4);
    for (uint32_t object : table) {
        if (object != 0) fn(object);
    }
}

bool OuterChainHas(uint32_t object, const char* name) {
    for (uint32_t outer = U32(object + kObjectOuterOffset); outer != 0;
         outer = U32(outer + kObjectOuterOffset)) {
        if (ObjectName(outer) == name) return true;
    }
    return false;
}

uint32_t g_watchMeshOffset = 0;
uint32_t g_watchControllerOffset = 0;

// Offset of a named property on an object's class chain, or 0.
uint32_t PropertyOffset(uint32_t object, const char* name) {
    for (uint32_t cls = U32(object + kObjectClassOffset); cls != 0; cls = U32(cls + kStructSuperOffset)) {
        for (uint32_t child = U32(cls + kStructChildrenOffset); child != 0;
             child = U32(child + kFieldNextOffset)) {
            if (ObjectName(child) == name) return U32(child + kPropertyOffsetOffset);
        }
    }
    return 0;
}

void DumpHex(uint32_t address, const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i < bytes.size(); i += 16) {
        std::printf("%04X  ", static_cast<unsigned>(i));
        for (size_t j = i; j < i + 16 && j < bytes.size(); ++j) std::printf("%02X ", bytes[j]);
        std::printf("\n");
    }
    (void)address;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: KidInspect objects|script|props|instances|read ...\n");
        return 2;
    }
    if (!AttachToGame()) return 1;
    const std::string command = argv[1];
    if (command == "read" && argc >= 4) {
        uint32_t address = std::strtoul(argv[2], nullptr, 16);
        std::vector<uint8_t> bytes(std::strtoul(argv[3], nullptr, 0));
        ReadBytes(address, bytes.data(), bytes.size());
        DumpHex(address, bytes);
        return 0;
    }
    if (command == "poke" && argc >= 4) {
        // poke <hexaddress> f:<float> | b:<byte> | i:<int>
        uint32_t address = std::strtoul(argv[2], nullptr, 16);
        std::string value = argv[3];
        uint8_t buffer[4] = {};
        size_t size = 4;
        if (value.rfind("f:", 0) == 0) { float f = std::strtof(value.c_str() + 2, nullptr); std::memcpy(buffer, &f, 4); }
        else if (value.rfind("b:", 0) == 0) { buffer[0] = static_cast<uint8_t>(std::strtoul(value.c_str() + 2, nullptr, 0)); size = 1; }
        else if (value.rfind("i:", 0) == 0) { int i = std::strtol(value.c_str() + 2, nullptr, 0); std::memcpy(buffer, &i, 4); }
        std::vector<uint8_t> bytes(buffer, buffer + size);
        if (value.rfind("x:", 0) == 0) {  // x:<hex bytes>, written with the game suspended
            bytes.clear();
            for (size_t i = 2; i + 1 < value.size(); i += 2) {
                bytes.push_back(static_cast<uint8_t>(std::strtoul(value.substr(i, 2).c_str(), nullptr, 16)));
            }
        }
        using SuspendFunction = LONG(NTAPI*)(HANDLE);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto suspend = reinterpret_cast<SuspendFunction>(GetProcAddress(ntdll, "NtSuspendProcess"));
        auto resume = reinterpret_cast<SuspendFunction>(GetProcAddress(ntdll, "NtResumeProcess"));
        HANDLE control = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, GetProcessId(g_process));
        if (control) suspend(control);
        DWORD oldProtect = 0;
        VirtualProtectEx(g_process, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), bytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect);
        SIZE_T written = 0;
        const BOOL ok = WriteProcessMemory(g_process, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), bytes.data(), bytes.size(), &written);
        VirtualProtectEx(g_process, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), bytes.size(), oldProtect, &oldProtect);
        FlushInstructionCache(g_process, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), bytes.size());
        if (control) { resume(control); CloseHandle(control); }
        std::puts(ok ? "ok" : "failed");
        return ok ? 0 : 1;
    }
    if (!FindObjectArray()) {
        std::fprintf(stderr, "GObjObjects not found\n");
        return 1;
    }

    if (command == "objects" && argc >= 3) {
        ForEachObject([&](uint32_t object) {
            if (ObjectName(object) == argv[2] && (argc < 4 || OuterChainHas(object, argv[3]))) {
                std::printf("%08X %s\n", object, FullName(object).c_str());
            }
        });
    } else if (command == "script" && argc >= 4) {
        ForEachObject([&](uint32_t object) {
            if (ObjectName(object) != argv[2] || ObjectName(U32(object + kObjectOuterOffset)) != argv[3]) return;
            std::string className = ObjectName(U32(object + kObjectClassOffset));
            if (className != "Function") return;
            uint32_t data = U32(object + kStructScriptOffset);
            int count = static_cast<int>(U32(object + kStructScriptOffset + 4));
            std::printf("%08X %s script=%08X size=%d\n", object, FullName(object).c_str(), data, count);
            uint32_t child = U32(object + kStructChildrenOffset);
            while (child != 0) {
                std::printf("  param/local %-24s %-16s offset=0x%X\n", ObjectName(child).c_str(),
                            ObjectName(U32(child + kObjectClassOffset)).c_str(),
                            U32(child + kPropertyOffsetOffset));
                child = U32(child + kFieldNextOffset);
            }
            if (count > 0 && count < 1 << 20) {
                std::vector<uint8_t> bytes(static_cast<size_t>(count));
                ReadBytes(data, bytes.data(), bytes.size());
                DumpHex(data, bytes);
            }
        });
    } else if (command == "props" && argc >= 3) {
        ForEachObject([&](uint32_t object) {
            if (ObjectName(object) != argv[2] || ObjectName(U32(object + kObjectClassOffset)) != "Class") return;
            std::printf("%08X %s super=%s\n", object, FullName(object).c_str(),
                        ObjectName(U32(object + kStructSuperOffset)).c_str());
            for (uint32_t child = U32(object + kStructChildrenOffset); child != 0;
                 child = U32(child + kFieldNextOffset)) {
                uint32_t raw[12] = {};
                ReadBytes(child + 0x68, raw, sizeof(raw));
                std::printf("  %-36s %-18s offset=0x%X  tail:", ObjectName(child).c_str(),
                            ObjectName(U32(child + kObjectClassOffset)).c_str(), raw[0]);
                for (uint32_t value : raw) std::printf(" %08X", value);
                std::printf("\n");
            }
        });
    } else if (command == "list" && argc >= 3) {
        int limit = std::atoi(argv[2]);
        int shown = 0;
        ForEachObject([&](uint32_t object) {
            if (shown++ < limit) std::printf("%08X idx=%u %s\n", object, U32(object + kObjectIndexOffset), FullName(object).c_str());
        });
    } else if (command == "get" && argc >= 4) {
        // get <hexobject> <Prop>[,<Prop>...]: read properties via reflection.
        uint32_t object = std::strtoul(argv[2], nullptr, 16);
        std::string list = argv[3];
        size_t start = 0;
        while (start <= list.size()) {
            size_t comma = list.find(',', start);
            std::string want = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            start = comma == std::string::npos ? list.size() + 1 : comma + 1;
            bool found = false;
            for (uint32_t cls = U32(object + kObjectClassOffset); cls != 0 && !found;
                 cls = U32(cls + kStructSuperOffset)) {
                for (uint32_t child = U32(cls + kStructChildrenOffset); child != 0;
                     child = U32(child + kFieldNextOffset)) {
                    if (ObjectName(child) != want) continue;
                    found = true;
                    std::string type = ObjectName(U32(child + kObjectClassOffset));
                    uint32_t offset = U32(child + kPropertyOffsetOffset);
                    uint32_t raw[3] = {};
                    ReadBytes(object + offset, raw, sizeof(raw));
                    float f[3];
                    std::memcpy(f, raw, sizeof(f));
                    std::printf("%s (%s @0x%X) = ", want.c_str(), type.c_str(), offset);
                    if (type == "FloatProperty") std::printf("%g\n", f[0]);
                    else if (type == "IntProperty") std::printf("%d\n", static_cast<int>(raw[0]));
                    else if (type == "ByteProperty") std::printf("%u\n", raw[0] & 0xFF);
                    else if (type == "BoolProperty") std::printf("%d\n", (raw[0] & U32(child + 0x78)) != 0);
                    else if (type == "ObjectProperty" || type == "ClassProperty" || type == "ComponentProperty")
                        std::printf("%08X %s\n", raw[0], raw[0] ? FullName(raw[0]).c_str() : "None");
                    else if (type == "NameProperty") std::printf("%s\n", NameByIndex(static_cast<int>(raw[0])).c_str());
                    else if (type == "StructProperty") std::printf("(%g, %g, %g) / ints (%d, %d, %d)\n", f[0], f[1], f[2],
                                                                  static_cast<int>(raw[0]), static_cast<int>(raw[1]), static_cast<int>(raw[2]));
                    else std::printf("%08X %08X %08X\n", raw[0], raw[1], raw[2]);
                    break;
                }
            }
            if (!found) std::printf("%s: not found\n", want.c_str());
        }
    } else if (command == "watch" && argc >= 4) {
        // watch <hexpawn> <seconds>: sample Location/Base/Physics/Velocity.
        // "watch auto <seconds>" follows the live KidPawn_Mondo across reloads.
        const bool follow = std::strcmp(argv[2], "auto") == 0;
        auto findPawn = [&]() -> uint32_t {
            uint32_t found = 0;
            ForEachObject([&](uint32_t object) {
                if (found == 0 && ObjectName(U32(object + kObjectClassOffset)) == "KidPawn_Mondo" &&
                    ObjectName(object).rfind("Default__", 0) != 0) {
                    found = object;
                }
            });
            return found;
        };
        uint32_t pawn = follow ? 0 : std::strtoul(argv[2], nullptr, 16);
        while (follow && pawn == 0) {
            FindObjectArray();
            pawn = findPawn();
            if (pawn == 0) Sleep(2000);
        }
        uint32_t pawnIndex = U32(pawn + kObjectIndexOffset);
        ULONGLONG lastValidation = GetTickCount64();
        const double seconds = std::atof(argv[3]);
        g_watchMeshOffset = PropertyOffset(pawn, "Mesh");
        g_watchControllerOffset = PropertyOffset(pawn, "Controller");
        std::printf("Mesh @0x%X Controller @0x%X\n", g_watchMeshOffset, g_watchControllerOffset);
        LARGE_INTEGER frequency, start, now;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        uint32_t lastBase = 0xFFFFFFFF;
        int lastPhysics = -1;
        float last[3] = {};
        do {
            QueryPerformanceCounter(&now);
            const double t = double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
            if (follow && GetTickCount64() - lastValidation > 1000) {
                lastValidation = GetTickCount64();
                const bool valid = ObjectName(U32(pawn + kObjectClassOffset)) == "KidPawn_Mondo" &&
                                   U32(pawn + kObjectIndexOffset) == pawnIndex;
                if (!valid) {
                    std::printf("%8.3f pawn lost; searching\n", t);
                    std::fflush(stdout);
                    uint32_t next = 0;
                    while (next == 0) {
                        FindObjectArray();
                        next = findPawn();
                        if (next == 0) Sleep(2000);
                    }
                    pawn = next;
                    pawnIndex = U32(pawn + kObjectIndexOffset);
                    lastBase = 0xFFFFFFFF;
                    QueryPerformanceCounter(&now);
                    std::printf("%8.3f now watching pawn %08X\n",
                                double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart), pawn);
                }
            }
            float location[3] = {}, velocity[3] = {};
            ReadBytes(pawn + 0x54, location, sizeof(location));
            ReadBytes(pawn + 0x138, velocity, sizeof(velocity));
            uint32_t base = U32(pawn + 0xA0);
            int physics = 0;
            Read(pawn + 0x94, reinterpret_cast<uint8_t*>(&physics));
            const float dx = location[0] - last[0], dy = location[1] - last[1], dz = location[2] - last[2];
            // Fixed offsets (Location 0x54, Physics 0x94, Velocity 0x138,
            // Acceleration 0x144, Mesh.RootMotionMode 0x5B4, controller fields)
            // were checked with "get" against this build. Mesh and Controller
            // are resolved by reflection once at start.
            float acceleration[3] = {};
            ReadBytes(pawn + 0x144, acceleration, sizeof(acceleration));
            const uint32_t controller = U32(pawn + g_watchControllerOffset);
            const uint32_t mesh = g_watchMeshOffset ? U32(pawn + g_watchMeshOffset) : 0;
            uint8_t rootMotion = 0xFF;
            if (mesh) Read(mesh + 0x5B4, &rootMotion);
            const uint32_t cinematic = controller ? U32(controller + 0x35C) : 0;
            uint8_t ignoreMove = 0;
            if (controller) Read(controller + 0x371, &ignoreMove);
            const uint32_t kidFlags = controller ? U32(controller + 0x62C) : 0;
            std::string state = "-";
            const uint32_t stateFrame = controller ? U32(controller + 0x14) : 0;
            if (stateFrame) state = ObjectName(U32(stateFrame + 0x28));
            const std::string signature = state + std::to_string(rootMotion) +
                std::to_string(cinematic & 0x10000) + std::to_string(ignoreMove) + std::to_string(kidFlags & 6);
            static std::string lastSignature;
            if (base != lastBase || physics != lastPhysics || dx * dx + dy * dy + dz * dz > 0.25f ||
                signature != lastSignature) {
                lastSignature = signature;
                std::printf("%8.3f loc=(%8.1f %8.1f %8.1f) vel=(%7.1f %7.1f %7.1f) acc=(%7.1f %7.1f) "
                            "rmm=%u cine=%d ignMove=%u kidIgn=%u state=%s phys=%d base=%s\n", t,
                            location[0], location[1], location[2], velocity[0], velocity[1], velocity[2],
                            acceleration[0], acceleration[1], rootMotion, (cinematic & 0x10000) != 0,
                            ignoreMove, (kidFlags >> 1) & 3, state.c_str(),
                            physics, base == lastBase ? "=" : (base ? FullName(base).c_str() : "None"));
                std::memcpy(last, location, sizeof(last));
                lastBase = base;
                lastPhysics = physics;
            }
            Sleep(2);
        } while (double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart) < seconds);
    } else if (command == "instances" && argc >= 3) {
        ForEachObject([&](uint32_t object) {
            if (ObjectName(U32(object + kObjectClassOffset)) == argv[2]) {
                std::printf("%08X %s\n", object, FullName(object).c_str());
            }
        });
    } else {
        std::fprintf(stderr, "bad command\n");
        return 2;
    }
    return 0;
}
