#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <cstdarg>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if !defined(_M_IX86)
#error KIDFrameRateFix must be compiled as 32-bit x86.
#endif

bool TimingProbeInstall(BYTE* executableBase);
void TimingProbeStart();
void TimingProbeFinish(double seconds, DWORD gameThread);
void KidLog(const wchar_t* format, ...);
void CrashDumpInstall(BYTE* textBegin, SIZE_T textSize, const wchar_t* dumpPath);
void ApplyStartupResolution(int mode, int width, int height);

#ifndef KIDFRAMERATEFIX_VERSION
#define KIDFRAMERATEFIX_VERSION "1.1.0"
#endif

namespace {

constexpr DWORD kSupportedTimestamp = 0x53548489;
constexpr DWORD kSupportedImageSize = 0x04237000;
constexpr int kDefaultUiTickFps = 30;
constexpr int kDefaultMaxCatchUpTicks = 8;
constexpr float kDefaultMaxDeltaSeconds = 0.250f;
constexpr ptrdiff_t kUObjectClassOffset = 0x34;
constexpr ptrdiff_t kFFrameNodeOffset = 0x10;
constexpr ptrdiff_t kFFrameCodeOffset = 0x18;
constexpr ptrdiff_t kFFrameLocalsOffset = 0x1C;
constexpr ptrdiff_t kUObjectIndexOffset = 0x20;
constexpr ptrdiff_t kUObjectOuterOffset = 0x28;
constexpr ptrdiff_t kUObjectNameOffset = 0x2C;
constexpr ptrdiff_t kFieldNextOffset = 0x3C;
constexpr ptrdiff_t kStructSuperOffset = 0x48;
constexpr ptrdiff_t kStructChildrenOffset = 0x4C;
constexpr ptrdiff_t kStructScriptOffset = 0x54;
constexpr ptrdiff_t kPropertyOffsetOffset = 0x68;
constexpr ptrdiff_t kBoolPropertyMaskOffset = 0x78;
constexpr ptrdiff_t kGObjObjectsRva = 0x03D965D8;
constexpr SIZE_T kProcessInternalHookLength = 13;
constexpr SIZE_T kMovieTickHookLength = 8;
constexpr SIZE_T kScriptHookLength = 10;
constexpr SIZE_T kFrameConstructorHookLength = 13;
constexpr SIZE_T kCollisionUpdateHookLength = 8;
constexpr SIZE_T kMaxProtectedRibbonParticles = 1024;
constexpr SIZE_T kCollisionStateSlots = 4096;
constexpr SIZE_T kKismetLatentStateSlots = 64;
constexpr SIZE_T kKismetLatentActionCount = 4;
constexpr SIZE_T kSoundRetriggerStateSlots = 64;
constexpr SIZE_T kSoundRetriggerTargetCount = 4;
// Discovery runs through the universal FFrame constructor. Remembering
// functions which are not sound targets prevents repeated GNames/object-name
// walks on every invocation of ordinary script functions.
constexpr SIZE_T kSoundRetriggerProbeSlots = 2048;
constexpr SIZE_T kFixedStepStateSlots = 32;
constexpr SIZE_T kFixedStepTargetCount = 4;
constexpr SIZE_T kUiFrameGateStateSlots = 512;
constexpr SIZE_T kUiFrameGateTargetCount = 11;
constexpr SIZE_T kUiFrameGateDispatchSlots = 256;
constexpr int kCollisionCounterSentinel = 0x20000000;
constexpr float kExpiredParticleRelativeTime = 1.0001f;
constexpr ptrdiff_t kMovieTickFromAdvancePattern = -0x12C;
constexpr ptrdiff_t kGNamesDataRva = 0x03D52848;
constexpr ptrdiff_t kGNamesCountRva = 0x03D5284C;

HMODULE g_module = nullptr;
BYTE* g_executableBase = nullptr;
bool g_enabled = true;
bool g_newMessageTimingFix = true;
bool g_smoothUiExperimental = true;
bool g_cardiacMassageFix = true;
bool g_cardiacMassageDiagnostics = false;
bool g_particleCollisionFix = true;
bool g_kismetLatentTimingFix = true;
bool g_sebastianStunFallFix = true;
bool g_soundRetriggerFix = true;
bool g_giantHeadStunTimingFix = true;
bool g_thomasMotionTimingFix = true;
bool g_giantHeadBeamTimingFix = true;
bool g_sebastianAbsorbTimingFix = true;
bool g_bossFinisherDiagnostics = false;
bool g_collectableAbsorbFix = true;
bool g_droppedPickupFix = true;
bool g_giantHeadThrowSpinFix = true;
bool g_cinematicRootMotionFix = true;
bool g_audioDeviceFix = true;
bool g_unlockFrameRate = true;
int g_frameRateLimit = 0;
int g_uiTickFps = kDefaultUiTickFps;
int g_maxCatchUpTicks = kDefaultMaxCatchUpTicks;
float g_maxDeltaSeconds = kDefaultMaxDeltaSeconds;
double g_uiAccumulatorSeconds = 0.0;
double g_newMessageAccumulatorSeconds = 0.0;
float g_movieAdvanceDelta = 1.0f / 30.0f;
bool g_fullscreenFadeFix = true;
bool g_fovHorPlus = true;   // [Display] FOVMode=hor+ (false = vert-, the retail behaviour)
float g_fov = 60.0f;        // [Display] FOV, gameplay horizontal FOV at 16:9
float g_fovScale = 1.0f;    // tan(FOV/2) / tan(30 degrees)
float g_supersampling = 1.0f; // [Display] Supersampling, render scale per axis (1 = off)
int g_resolutionMode = 1;  // 0 = game setting, 1 = desktop, 2 = configured size
int g_resolutionWidth = 0;
int g_resolutionHeight = 0;
bool g_frameRateReport = false;
bool g_timingProbe = false;
bool g_crashDump = false;
// Size of the cinematic fade quad. Retail hard-codes 1920x1080.
float g_fadeWidth = 1920.0f;
float g_fadeHeight = 1080.0f;
PVOID volatile g_cardiacDownScaleOperand = nullptr;
PVOID volatile g_cardiacDecayScaleOperand = nullptr;
PVOID volatile g_uiFadeDivisorOperands[2] = {};
volatile LONG g_uiFadePatchActivated = 0;
volatile LONG g_scriptTickSeen = 0;
volatile LONG g_movieTickSeen = 0;
volatile LONG g_uiCatchUpClamped = 0;
volatile LONG g_scriptTickGeneration = 0;
volatile LONG g_scriptTickLimiterInstalled = 0;
PVOID volatile g_newMessageTickFunction = nullptr;
PVOID volatile g_newMessageReturnCode = nullptr;
volatile LONG g_newMessageTickNameIndex = -1;
volatile LONG g_newMessageClassNameIndex = -1;
volatile LONG g_newMessageFixActivated = 0;
volatile LONG g_uiManagerTickNameIndex = -1;
volatile LONG g_uiManagerClassNameIndex = -1;
volatile LONG g_cardiacActivated = 0;
volatile LONG g_cardiacTargetSeen = 0;
volatile LONG g_cardiacTargetNameIndex = -1;
volatile LONG g_particleCollisionSeen = 0;
volatile LONG g_particleCollisionKillSeen = 0;
volatile LONG g_particleCollisionCapacityWarning = 0;
volatile LONG g_kismetLatentCapacityWarning = 0;
volatile LONG g_bossFinisherOnAdjustNameIndex = -1;
volatile LONG g_bossFinisherOnAnimEndNameIndex = -1;
volatile LONG g_bossFinisherSebastianClassNameIndex = -1;
volatile LONG g_bossFinisherAdrenalineStateNameIndex = -1;
volatile LONG g_bossFinisherActivatedNameIndex = -1;
volatile LONG g_bossFinisherToggleCinematicClassNameIndex = -1;
volatile LONG g_bossFinisherLoadingTipsClassNameIndex = -1;
volatile LONG g_bossFinisherCinePackageNameIndex = -1;
volatile LONG g_bossFinisherCastlePackageNameIndex = -1;
volatile LONG g_sebastianControllerClassNameIndex = -1;
volatile LONG g_sebastianTickNameIndex = -1;
volatile LONG g_sebastianStunStateNameIndex = -1;
volatile LONG g_sebastianRecoveryStateNameIndex = -1;
volatile LONG g_sebastianAbsorbStateNameIndex = -1;
volatile LONG g_sebastianBeginStateNameIndex = -1;
volatile LONG g_sebastianHasLandedNameIndex = -1;
volatile LONG g_sebastianLandedNameIndex = -1;
volatile LONG g_sebastianTryStunEndNameIndex = -1;
ULONGLONG g_bossFinisherTraceUntilMs = 0;
ULONGLONG g_bossFinisherLastAdjustMs = 0;
ULONGLONG g_sebastianThirdCutStartMs = 0;
ULONGLONG g_sebastianStunStartMs = 0;
ULONGLONG g_sebastianFirstLandedMs = 0;
ULONGLONG g_sebastianStunAnimEndMs = 0;
void* g_sebastianStunController = nullptr;
void* g_sebastianAdjustPawn = nullptr;
int g_sebastianAdjustCount = 0;
bool g_sebastianLandedSinceStun = false;
bool g_sebastianMissingLandingLogged = false;
bool g_sebastianTryStunEndLogged = false;
bool g_sebastianMatineeBoundarySeen = false;
PVOID volatile g_sebastianVerticalTickFunction = nullptr;
PVOID volatile g_sebastianVelocityClampStateOperand = nullptr;
volatile LONG g_sebastianVerticalTargetSeen = 0;
void* g_sebastianExpectedStunGlobalTick = nullptr;
ULONGLONG g_sebastianAbsorbStateNameValue = 0;
ULONGLONG g_sebastianStunStateNameValue = 0;
wchar_t g_iniPath[MAX_PATH] = {};
wchar_t g_logPath[MAX_PATH] = {};

using TickFunction = void(__thiscall*)(void* tickableObject, float deltaTime);
using FrameConstructorFunction = void*(__thiscall*)(
    void* frame, void* object, void* function, int codeOffset,
    void* locals, void* previousFrame);
using CollisionUpdateFunction = void(__thiscall*)(
    void* module, void* emitter, int payloadOffset, float deltaTime);
using KillParticlesFunction = void(__thiscall*)(void* emitter);
TickFunction g_originalMovieTick = nullptr;
TickFunction g_originalScriptTick = nullptr;
FrameConstructorFunction g_originalFrameConstructor = nullptr;
CollisionUpdateFunction g_originalCollisionUpdate = nullptr;

struct KismetLatentActionTarget {
    const char* className;
    const wchar_t* classNameWide;
    volatile LONG classNameIndex;
    PVOID volatile updateFunction;
    PVOID volatile returnTrueCode;
};

KismetLatentActionTarget g_kismetLatentTargets[kKismetLatentActionCount] = {
    {"KidSeqAct_MissionComplete", L"KidSeqAct_MissionComplete", -1, nullptr, nullptr},
    {"KidSeqAct_MissionWave", L"KidSeqAct_MissionWave", -1, nullptr, nullptr},
    {"KidSeqAct_TicketGet", L"KidSeqAct_TicketGet", -1, nullptr, nullptr},
    {"KidSeqAct_WeaponGet", L"KidSeqAct_WeaponGet", -1, nullptr, nullptr},
};

struct KismetLatentState {
    void* object;
    int targetIndex;
    LONG lastGeneration;
};

KismetLatentState g_kismetLatentStates[kKismetLatentStateSlots] = {};

// These are narrow, known frame-polled sound emitters. Their script bodies
// only generate feedback, so suppressing duplicate executions does not alter
// damage, collision, input, or state progression.
struct SoundRetriggerTarget {
    const char* functionName;
    const wchar_t* functionNameWide;
    const char* outerName;
    const wchar_t* outerNameWide;
    volatile LONG functionNameIndex;
    volatile LONG outerNameIndex;
    PVOID volatile function;
    PVOID volatile returnCode;
    volatile LONG suppressionLogged;
};

SoundRetriggerTarget g_soundRetriggerTargets[kSoundRetriggerTargetCount] = {
    {"OnFinishDrillCancel", L"OnFinishDrillCancel", "KidWeapon_Drill",
     L"KidWeapon_Drill", -1, -1, nullptr, nullptr, 0},
    {"PlayDamageSoundCueAndParticleSystem", L"PlayDamageSoundCueAndParticleSystem",
     "KidPlayerController", L"KidPlayerController", -1, -1, nullptr, nullptr, 0},
    {"PlayLeverSoundSE", L"PlayLeverSoundSE", "KidPlayerSpecialMove_Jigoro",
     L"KidPlayerSpecialMove_Jigoro", -1, -1, nullptr, nullptr, 0},
    {"PlayLeverSoundSE", L"PlayLeverSoundSE", "KidSeqAct_ScarletCutScene",
     L"KidSeqAct_ScarletCutScene", -1, -1, nullptr, nullptr, 0},
};

struct SoundRetriggerState {
    void* object;
    int targetIndex;
    LONG lastGeneration;
};

SoundRetriggerState g_soundRetriggerStates[kSoundRetriggerStateSlots] = {};

struct SoundRetriggerProbe {
    void* function;
};

// This is deliberately a direct-mapped, allocation-free cache. UFunction
// objects have stable addresses for the process lifetime, and a collision can
// only cause a harmless one-time recheck of the displaced function.
SoundRetriggerProbe g_soundRetriggerProbes[kSoundRetriggerProbeSlots] = {};

// In smooth-UI mode the manager runs every rendered frame. These helpers
// contain discrete retail-frame counters and therefore remain on the shared
// 30 Hz logical generation while DeltaTime-safe UI helpers run at render rate.
struct UiFrameGateTarget {
    const char* functionName;
    const wchar_t* functionNameWide;
    const char* outerName;
    const wchar_t* outerNameWide;
    volatile LONG functionNameIndex;
    volatile LONG outerNameIndex;
    PVOID volatile function;
    PVOID volatile returnCode;
};

UiFrameGateTarget g_uiFrameGateTargets[kUiFrameGateTargetCount] = {
    {"PauseEventTick", L"PauseEventTick", "KidGFxMgr", L"KidGFxMgr", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_ABVision", L"KidGFx_ABVision", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_BurstVision", L"KidGFx_BurstVision", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_ButtonGuide", L"KidGFx_ButtonGuide", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_ClearStats", L"KidGFx_ClearStats", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_Leaderboard", L"KidGFx_Leaderboard", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_MondoGauge", L"KidGFx_MondoGauge", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_MondoStatus", L"KidGFx_MondoStatus", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_MovieStatus", L"KidGFx_MovieStatus", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_WeaponChange", L"KidGFx_WeaponChange", -1, -1, nullptr, nullptr},
    {"Tick", L"Tick", "KidGFx_MovieResultContent", L"KidGFx_MovieResultContent", -1, -1, nullptr, nullptr},
};

struct UiFrameGateState {
    void* object;
    int targetIndex;
    LONG lastGeneration;
};

UiFrameGateState g_uiFrameGateStates[kUiFrameGateStateSlots] = {};

struct UiFrameGateDispatch {
    void* function;
    int targetIndex;
};

UiFrameGateDispatch g_uiFrameGateDispatch[kUiFrameGateDispatchSlots] = {};

// These functions combine ordinary DeltaTime work with explicit 30-FPS frame
// counters or per-call impulses. Run their whole retail script body on an
// elapsed-time 30 Hz cadence so both categories retain their original phase.
struct FixedStepTarget {
    const char* functionName;
    const wchar_t* functionNameWide;
    const char* stateName;
    const wchar_t* stateNameWide;
    const char* ownerName;
    const wchar_t* ownerNameWide;
    bool* enabled;
    volatile LONG functionNameIndex;
    volatile LONG stateNameIndex;
    volatile LONG ownerNameIndex;
    PVOID volatile function;
    PVOID volatile returnCode;
    volatile LONG suppressionLogged;
    volatile LONG parameterFailureLogged;
};

FixedStepTarget g_fixedStepTargets[kFixedStepTargetCount] = {
    {"Tick", L"Tick", "StunBackState", L"StunBackState",
     "KidAIController_GiantHead", L"KidAIController_GiantHead",
     &g_giantHeadStunTimingFix, -1, -1, -1, nullptr, nullptr, 0, 0},
    {"MotionFunctionTick", L"MotionFunctionTick", "KidAIController_ThomasBase",
     L"KidAIController_ThomasBase", "KidAIController_ThomasBase",
     L"KidAIController_ThomasBase", &g_thomasMotionTimingFix,
     -1, -1, -1, nullptr, nullptr, 0, 0},
    {"Tick", L"Tick", "TracePlayerBeamesState", L"TracePlayerBeamesState",
     "KidAIController_GiantHead", L"KidAIController_GiantHead",
     &g_giantHeadBeamTimingFix, -1, -1, -1, nullptr, nullptr, 0, 0},
    {"Tick", L"Tick", "AbsorbState", L"AbsorbState",
     "KidAIController_Sebastian", L"KidAIController_Sebastian",
     &g_sebastianAbsorbTimingFix, -1, -1, -1, nullptr, nullptr, 0, 0},
};

struct FixedStepState {
    void* object;
    int targetIndex;
    double accumulatedDelta;
    ULONGLONG lastCallMs;
    LONG lastGeneration;
    bool hasRun;
};

FixedStepState g_fixedStepStates[kFixedStepStateSlots] = {};

template <typename T, size_t N>
constexpr size_t ArrayCount(const T (&)[N]) {
    return N;
}

// The plugin may be loaded as KIDFrameRateFix.asi (ASI loader) or as the
// standalone dinput8.dll proxy; its companion files always use the
// KIDFrameRateFix base name beside the module.
void BuildSiblingPath(const wchar_t* extension, wchar_t* output, size_t outputCount) {
    GetModuleFileNameW(g_module, output, static_cast<DWORD>(outputCount));
    wchar_t* slash = wcsrchr(output, L'\\');
    wchar_t* name = slash == nullptr ? output : slash + 1;
    _snwprintf_s(name, outputCount - static_cast<size_t>(name - output), _TRUNCATE,
                 L"KIDFrameRateFix%s", extension);
}

void Log(const wchar_t* format, ...) {
    wchar_t wideLine[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(wideLine, ArrayCount(wideLine), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t stamped[1200] = {};
    _snwprintf_s(stamped, ArrayCount(stamped), _TRUNCATE,
                 L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                 now.wSecond, now.wMilliseconds, wideLine);

    char utf8[2400] = {};
    const int byteCount = WideCharToMultiByte(CP_UTF8, 0, stamped, -1, utf8,
                                               static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (byteCount <= 1) {
        return;
    }

    HANDLE file = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, utf8, static_cast<DWORD>(byteCount - 1), &written, nullptr);
    CloseHandle(file);
}

void LoadConfiguration() {
    const int enabled = GetPrivateProfileIntW(L"Limiter", L"Enabled", 1, g_iniPath);
    if (enabled == 0) {
        g_enabled = false;
        return;
    }

    int uiTick = GetPrivateProfileIntW(L"Limiter", L"UITickFPS", kDefaultUiTickFps, g_iniPath);
    if (uiTick < 1 || uiTick > 240) {
        Log(L"Invalid UITickFPS=%d; using %d.", uiTick, kDefaultUiTickFps);
        uiTick = kDefaultUiTickFps;
    }

    int catchUpTicks = GetPrivateProfileIntW(
        L"Limiter", L"MaxCatchUpTicks", kDefaultMaxCatchUpTicks, g_iniPath);
    if (catchUpTicks < 1 || catchUpTicks > 60) {
        catchUpTicks = kDefaultMaxCatchUpTicks;
    }

    int maxDeltaMilliseconds = GetPrivateProfileIntW(
        L"Limiter", L"MaxDeltaMilliseconds",
        static_cast<int>(kDefaultMaxDeltaSeconds * 1000.0f), g_iniPath);
    if (maxDeltaMilliseconds < 16 || maxDeltaMilliseconds > 1000) {
        maxDeltaMilliseconds = static_cast<int>(kDefaultMaxDeltaSeconds * 1000.0f);
    }

    g_cardiacMassageFix = GetPrivateProfileIntW(
        L"Gameplay", L"CardiacMassageDeltaFix", 1, g_iniPath) != 0;
    g_cardiacMassageDiagnostics = GetPrivateProfileIntW(
        L"Gameplay", L"CardiacMassageDiagnostics", 0, g_iniPath) != 0;
    g_particleCollisionFix = GetPrivateProfileIntW(
        L"Gameplay", L"ParticleCollisionFix", 1, g_iniPath) != 0;
    g_kismetLatentTimingFix = GetPrivateProfileIntW(
        L"Gameplay", L"KismetLatentTimingFix", 1, g_iniPath) != 0;
    g_sebastianStunFallFix = GetPrivateProfileIntW(
        L"Gameplay", L"SebastianStunFallFix", 1, g_iniPath) != 0;
    g_soundRetriggerFix = GetPrivateProfileIntW(
        L"Gameplay", L"SoundRetriggerFix", 1, g_iniPath) != 0;
    g_giantHeadStunTimingFix = GetPrivateProfileIntW(
        L"Gameplay", L"GiantHeadStunTimingFix", 1, g_iniPath) != 0;
    g_thomasMotionTimingFix = GetPrivateProfileIntW(
        L"Gameplay", L"ThomasMotionTimingFix", 1, g_iniPath) != 0;
    g_giantHeadBeamTimingFix = GetPrivateProfileIntW(
        L"Gameplay", L"GiantHeadBeamTimingFix", 1, g_iniPath) != 0;
    g_sebastianAbsorbTimingFix = GetPrivateProfileIntW(
        L"Gameplay", L"SebastianAbsorbTimingFix", 1, g_iniPath) != 0;
    g_collectableAbsorbFix = GetPrivateProfileIntW(
        L"Gameplay", L"CollectableAbsorbFix", 1, g_iniPath) != 0;
    g_droppedPickupFix = GetPrivateProfileIntW(
        L"Gameplay", L"DroppedPickupFix", 1, g_iniPath) != 0;
    g_giantHeadThrowSpinFix = GetPrivateProfileIntW(
        L"Gameplay", L"GiantHeadThrowSpinFix", 1, g_iniPath) != 0;
    g_cinematicRootMotionFix = GetPrivateProfileIntW(
        L"Gameplay", L"CinematicRootMotionFix", 1, g_iniPath) != 0;
    g_audioDeviceFix = GetPrivateProfileIntW(
        L"Gameplay", L"AudioDeviceFix", 1, g_iniPath) != 0;
    g_unlockFrameRate = GetPrivateProfileIntW(
        L"FrameRate", L"UnlockFrameRate", 1, g_iniPath) != 0;
    g_crashDump = GetPrivateProfileIntW(
        L"Diagnostics", L"CrashDump", 0, g_iniPath) != 0;
    g_timingProbe = GetPrivateProfileIntW(
        L"Diagnostics", L"TimingProbe", 0, g_iniPath) != 0;
    g_frameRateReport = GetPrivateProfileIntW(
        L"Diagnostics", L"FrameRateReport", 0, g_iniPath) != 0;
    wchar_t resolution[64] = {};
    GetPrivateProfileStringW(L"Display", L"Resolution", L"auto", resolution,
                             static_cast<DWORD>(ArrayCount(resolution)), g_iniPath);
    int width = 0;
    int height = 0;
    if (_wcsicmp(resolution, L"auto") == 0) {
        g_resolutionMode = 1;
    } else if (swscanf_s(resolution, L"%dx%d", &width, &height) == 2 ||
               swscanf_s(resolution, L"%dX%d", &width, &height) == 2) {
        g_resolutionMode = 2;
        g_resolutionWidth = width;
        g_resolutionHeight = height;
    } else {
        g_resolutionMode = 0;
    }
    g_fullscreenFadeFix = GetPrivateProfileIntW(
        L"Display", L"FullscreenFadeFix", 1, g_iniPath) != 0;
    wchar_t fovText[32] = {};
    GetPrivateProfileStringW(L"Display", L"FOVMode", L"hor+", fovText,
                             static_cast<DWORD>(ArrayCount(fovText)), g_iniPath);
    if (_wcsicmp(fovText, L"hor+") == 0) {
        g_fovHorPlus = true;
    } else if (_wcsicmp(fovText, L"vert-") == 0) {
        g_fovHorPlus = false;
    } else {
        g_fovHorPlus = true;
        Log(L"Unknown FOVMode=%s; using hor+.", fovText);
    }
    GetPrivateProfileStringW(L"Display", L"FOV", L"60", fovText,
                             static_cast<DWORD>(ArrayCount(fovText)), g_iniPath);
    g_fov = static_cast<float>(_wtof(fovText));
    if (!(g_fov >= 30.0f && g_fov <= 120.0f)) {
        Log(L"FOV=%s is outside 30-120; using 60.", fovText);
        g_fov = 60.0f;
    }
    g_fovScale = tanf(g_fov * 0.5f * 3.14159265f / 180.0f) / tanf(30.0f * 3.14159265f / 180.0f);
    GetPrivateProfileStringW(L"Display", L"Supersampling", L"1", fovText,
                             static_cast<DWORD>(ArrayCount(fovText)), g_iniPath);
    g_supersampling = static_cast<float>(_wtof(fovText));
    if (!(g_supersampling >= 1.0f && g_supersampling <= 4.0f)) {
        Log(L"Supersampling=%s is outside 1-4; supersampling is off.", fovText);
        g_supersampling = 1.0f;
    }
    int frameRateLimit = GetPrivateProfileIntW(
        L"FrameRate", L"FrameRateLimit", 0, g_iniPath);
    if (frameRateLimit != 0 && (frameRateLimit < 30 || frameRateLimit > 1000)) {
        Log(L"Invalid FrameRateLimit=%d; the limiter is disabled.", frameRateLimit);
        frameRateLimit = 0;
    }
    g_frameRateLimit = frameRateLimit;
    g_bossFinisherDiagnostics = GetPrivateProfileIntW(
        L"Diagnostics", L"BossFinisherTiming", 0, g_iniPath) != 0;
    g_newMessageTimingFix = GetPrivateProfileIntW(
        L"Limiter", L"NewMessageTimingFix", 1, g_iniPath) != 0;
    g_smoothUiExperimental = GetPrivateProfileIntW(
        L"Limiter", L"SmoothUIExperimental", 1, g_iniPath) != 0;
    g_uiTickFps = uiTick;
    g_maxCatchUpTicks = catchUpTicks;
    g_maxDeltaSeconds = static_cast<float>(maxDeltaMilliseconds) / 1000.0f;
    g_uiAccumulatorSeconds = 0.0;
    g_newMessageAccumulatorSeconds = 0.0;
}

struct PatternByte {
    BYTE value;
    bool wildcard;
};

// FGFxEngine's game-timed movie loop. The retail port loads a shared 1/30
// constant twice and passes it to Movie::Advance and PostAdvance every rendered
// frame. Only these operands are redirected, leaving other 1/30 users alone.
constexpr PatternByte kMovieAdvancePattern[] = {
    {0xD9, false}, {0x05, false},
    {0x00, true}, {0x00, true}, {0x00, true}, {0x00, true},
    {0x8B, false}, {0x11, false},
    {0x8B, false}, {0x42, false}, {0x5C, false},
    {0x6A, false}, {0x02, false}, {0x51, false},
    {0xD9, false}, {0x1C, false}, {0x24, false},
    {0xFF, false}, {0xD0, false},
    {0x8B, false}, {0x4E, false}, {0x60, false},
    {0xDD, false}, {0xD8, false},
    {0x85, false}, {0xC9, false}, {0x74, false}, {0x14, false},
    {0x8B, false}, {0x11, false},
    {0xD9, false}, {0x05, false},
    {0x00, true}, {0x00, true}, {0x00, true}, {0x00, true},
    {0x8B, false}, {0x82, false}, {0x3C, false}, {0x01, false}, {0x00, false}, {0x00, false},
    {0x51, false}, {0xD9, false}, {0x1C, false}, {0x24, false},
    {0xFF, false}, {0xD0, false},
};

constexpr SIZE_T kAdvanceOperandOffset = 2;
constexpr SIZE_T kPostAdvanceOperandOffset = 32;

// Cinematic fade DrawTile(Canvas, X=0, Y=0, Z, XL, YL, U=0, V=0, UL=1, VL=1, ...)
// in the HUD render path. XL/YL are loaded from a shared .rdata pair holding
// 1920.0/1080.0, so the black fade leaves uncovered borders at any other
// resolution. Only these two loads are redirected.
//   fld [1080.0]; fstp [esp+10h]; fld [1920.0]; fstp [esp+0Ch];
//   fld [Z]; fstp [esp+8]; fst [esp+4]; fstp [esp]; push ebp; call DrawTile
constexpr PatternByte kFadeTilePattern[] = {
    {0xD9, false}, {0x05, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xD9, false}, {0x5C, false}, {0x24, false}, {0x10, false},
    {0xD9, false}, {0x05, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xD9, false}, {0x5C, false}, {0x24, false}, {0x0C, false},
    {0xD9, false}, {0x05, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xD9, false}, {0x5C, false}, {0x24, false}, {0x08, false},
    {0xD9, false}, {0x54, false}, {0x24, false}, {0x04, false},
    {0xD9, false}, {0x1C, false}, {0x24, false},
    {0x55, false}, {0xE8, false},
};
constexpr SIZE_T kFadeHeightOperandOffset = 2;
constexpr SIZE_T kFadeWidthOperandOffset = 12;

// ULocalPlayer::CalcSceneView, unconstrained-aspect branch:
//   movss xmm0,[esp+2Ch]        ; FOV in degrees
//   mov eax,[esp+44h]           ; SizeX
//   mov ecx,[esp+40h]           ; SizeY
//   mulss xmm0,[deg->rad]; mulss xmm0,[0.5]
//   movss [esp+24h],xmm0        ; half FOV, used for both projection axes  <- hook
//   cmp eax,ecx; jbe; cmp [esi+AspectRatioAxisConstraint],dl; je; cmp [esi+...],1; jne
// With AspectRatio_MaintainXFOV (retail) the half angle is horizontal.
constexpr PatternByte kProjectionFovPattern[] = {
    {0xF3, false}, {0x0F, false}, {0x10, false}, {0x44, false}, {0x24, false}, {0x2C, false},
    {0x8B, false}, {0x44, false}, {0x24, false}, {0x44, false},
    {0x8B, false}, {0x4C, false}, {0x24, false}, {0x40, false},
    {0xF3, false}, {0x0F, false}, {0x59, false}, {0x05, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xF3, false}, {0x0F, false}, {0x59, false}, {0x05, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xF3, false}, {0x0F, false}, {0x11, false}, {0x44, false}, {0x24, false}, {0x24, false},
    {0x3B, false}, {0xC1, false}, {0x76, false}, {0x08, false},
    {0x38, false}, {0x96, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x74, false}, {0x09, false},
    {0x80, false}, {0xBE, false}, {0, true}, {0, true}, {0, true}, {0, true}, {0x01, false},
    {0x75, false},
};
constexpr SIZE_T kProjectionFovHookOffset = 30;      // movss [esp+24h],xmm0 (6 bytes)
constexpr SIZE_T kProjectionFovConstraintOffset = 42; // disp32 of AspectRatioAxisConstraint

// Screen-percentage plumbing in FSystemSettings and FSceneRenderTargets.
// UE3 renders the scene into a ScreenPercentage-sized rectangle of the scene
// buffers and scales it to the viewport in its final pass. Retail clamps the
// percentage to 100, centres the reduced rectangle, and sizes the buffers to
// the viewport, so values above 100 render nothing.
//
// FSystemSettings::NeedsUpscale:
//   cmp [ecx+bUpscaleScreenPercentage],0; je; movss xmm0,[100.0];
//   comiss xmm0,[ecx+ScreenPercentage]; jbe; cmp [ecx+...],0; jne; mov eax,1; ret
constexpr PatternByte kNeedsUpscalePattern[] = {
    {0x83, false}, {0xB9, false}, {0, true}, {0, true}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x74, false}, {0x20, false},
    {0xF3, false}, {0x0F, false}, {0x10, false}, {0x05, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x0F, false}, {0x2F, false}, {0x81, false}, {0, true}, {0, true}, {0x00, false}, {0x00, false},
    {0x76, false}, {0x0F, false},
    {0x83, false}, {0xB9, false}, {0, true}, {0, true}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x75, false}, {0x06, false},
    {0xB8, false}, {0x01, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0xC3, false},
    {0x33, false}, {0xC0, false}, {0xC3, false},
};
constexpr SIZE_T kNeedsUpscaleHookLength = 7;

// FSystemSettings::ScaleScreenCoords(INT& X, INT& Y, UINT& SizeX, UINT& SizeY),
// called by ULocalPlayer::CalcSceneView:
//   movss xmm0,[GSystemSettings.ScreenPercentage]; sub esp,0Ch; ucomiss xmm0,[100.0];
//   lahf; test ah,44h; jnp; cmp [ecx+...],0; jne; mulss xmm0,[0.01]
constexpr PatternByte kScaleScreenCoordsPattern[] = {
    {0xF3, false}, {0x0F, false}, {0x10, false}, {0x05, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x83, false}, {0xEC, false}, {0x0C, false},
    {0x0F, false}, {0x2E, false}, {0x05, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x9F, false}, {0xF6, false}, {0xC4, false}, {0x44, false},
    {0x0F, false}, {0x8B, false}, {0xEA, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x83, false}, {0xB9, false}, {0, true}, {0, true}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x0F, false}, {0x85, false}, {0xDD, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0xF3, false}, {0x0F, false}, {0x59, false}, {0x05, false},
};
constexpr SIZE_T kScaleScreenCoordsHookLength = 8;

// FSystemSettings::UnScaleScreenCoords(INT& X, INT& Y, UINT& SizeX, UINT& SizeY,
//   FLOAT ScaledX, FLOAT ScaledY, FLOAT ScaledSizeX, FLOAT ScaledSizeY), used by
// the renderer's final pass to map the scene rectangle back to the viewport.
constexpr PatternByte kUnScaleScreenCoordsPattern[] = {
    {0x83, false}, {0xB9, false}, {0, true}, {0, true}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x0F, false}, {0x84, false}, {0x03, false}, {0x01, false}, {0x00, false}, {0x00, false},
    {0xF3, false}, {0x0F, false}, {0x10, false}, {0x05, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x0F, false}, {0x2F, false}, {0x81, false}, {0, true}, {0, true}, {0x00, false}, {0x00, false},
    {0x0F, false}, {0x86, false}, {0xEE, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x83, false}, {0xB9, false}, {0, true}, {0, true}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x0F, false}, {0x85, false}, {0xE1, false}, {0x00, false}, {0x00, false}, {0x00, false},
};
constexpr SIZE_T kUnScaleScreenCoordsHookLength = 7;

// FSceneRenderTargets::Allocate(UINT MinSizeX, UINT MinSizeY): grows the scene
// buffers to at least the requested size (rounded up to a multiple of 8).
constexpr PatternByte kSceneTargetsAllocatePattern[] = {
    {0x6A, false}, {0xFF, false},
    {0x68, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x64, false}, {0xA1, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x50, false}, {0x53, false}, {0x55, false}, {0x56, false}, {0x57, false},
    {0xA1, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x33, false}, {0xC4, false}, {0x50, false},
    {0x8D, false}, {0x44, false}, {0x24, false}, {0x14, false},
    {0x64, false}, {0xA3, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x8B, false}, {0xF1, false},
    {0xE8, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x85, false}, {0xC0, false}, {0x75, false}, {0x50, false},
};
constexpr SIZE_T kSceneTargetsAllocateHookLength = 7;

// FSceneRenderer's constructor sets each view's scene-buffer region (the
// rectangle the post-process chain and the final pass read) to
//   RenderTargetSizeX = Min(appTrunc(View.SizeX), RenderTarget->GetSizeX())
// and the same for Y, which cuts a supersampled view back to the window size:
//   cvttss2si edi,xmm0; call GetSizeX/Y; cmp edi,eax; jg +2; mov eax,edi; ...
// The two jg are replaced with nops so the region is the full view.
constexpr PatternByte kViewTargetClampXPattern[] = {
    {0xF3, false}, {0x0F, false}, {0x2C, false}, {0xF8, false}, {0xFF, false}, {0xD0, false},
    {0x3B, false}, {0xF8, false}, {0x7F, false}, {0x02, false}, {0x8B, false}, {0xC7, false},
    {0xF3, false}, {0x0F, false}, {0x10, false}, {0x46, false}, {0x44, false},
    {0x89, false}, {0x46, false}, {0x50, false},
};
constexpr PatternByte kViewTargetClampYPattern[] = {
    {0xF3, false}, {0x0F, false}, {0x2C, false}, {0xF8, false}, {0xFF, false}, {0xD0, false},
    {0x3B, false}, {0xF8, false}, {0x7F, false}, {0x02, false}, {0x8B, false}, {0xC7, false},
    {0x89, false}, {0x46, false}, {0x54, false}, {0xDB, false}, {0x05, false},
};
constexpr SIZE_T kViewTargetClampJumpOffset = 8;

// The final pass's UnScaleScreenCoords call (FSceneRenderer finish, which
// copies the scene region to the viewport):
//   mov ecx,offset GSystemSettings; mov [esp+34h],ebx; call UnScaleScreenCoords;
//   cmp [ebp+4FCh],ebx; je
// Its other callers clear the scene buffers around the views and must cover the
// whole scaled region, so only this call maps back to viewport size.
constexpr PatternByte kFinalPassUnScaleCallPattern[] = {
    {0xB9, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x89, false}, {0x5C, false}, {0x24, false}, {0x34, false},
    {0xE8, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x39, false}, {0x9D, false}, {0xFC, false}, {0x04, false}, {0x00, false}, {0x00, false},
    {0x74, false}, {0x31, false},
};
constexpr SIZE_T kFinalPassUnScaleCallOffset = 9;

// The scene-colour clear at the start of a frame binds the scene buffers and
// sets the viewport to the view family's render target (the window):
//   mov ecx,offset GSceneRenderTargets; call BeginRenderingSceneColor;
//   mov ecx,[ebx+10h]; mov edx,[ecx]; mov eax,[edx+8]; call eax  ; GetSizeX
//   mov ecx,[ebx+10h]; mov edx,[ecx]; mov esi,eax; mov eax,[edx+0Ch]; call eax  ; GetSizeY
//   mov ecx,[GDynamicRHI] ...   (edi = SizeY; RHISetViewport(0,0,0,esi,edi,1))
// Supersampled, that clears only the top-left part of the scene buffers. The
// two size queries are replaced with a stub that scales them.
constexpr PatternByte kSceneColorViewportPattern[] = {
    {0xB9, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xE8, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x8B, false}, {0x4B, false}, {0x10, false}, {0x8B, false}, {0x11, false},
    {0x8B, false}, {0x42, false}, {0x08, false}, {0xFF, false}, {0xD0, false},
    {0x8B, false}, {0x4B, false}, {0x10, false}, {0x8B, false}, {0x11, false},
    {0x8B, false}, {0xF0, false}, {0x8B, false}, {0x42, false}, {0x0C, false},
    {0xFF, false}, {0xD0, false},
    {0x8B, false}, {0x0D, false},
};
constexpr SIZE_T kSceneColorViewportSiteOffset = 10;
constexpr SIZE_T kSceneColorViewportSiteLength = 22;

// FGFxEngine::Tick starts 0x12C bytes before the unique Movie::Advance sequence.
// The retail function accepts DeltaTime but ignores it for game-timed movies.
constexpr BYTE kMovieTickPrologue[] = {
    0x55,                   // push ebp
    0x8B, 0xEC,             // mov ebp,esp
    0x83, 0xE4, 0xF8,       // and esp,-8
    0x6A, 0xFF,             // push -1
};

// UKidGFxMgr::Tick. This drives the game's frame-counted UnrealScript HUD code.
constexpr PatternByte kScriptTickPattern[] = {
    {0x56, false},                         // push esi
    {0x8D, false}, {0x71, false}, {0xC4, false}, // lea esi,[ecx-3Ch]
    {0x89, false}, {0x35, false},
    {0x00, true}, {0x00, true}, {0x00, true}, {0x00, true},
    {0x8B, false}, {0x46, false}, {0x14, false},
    {0x85, false}, {0xC0, false}, {0x74, false}, {0x09, false},
    {0xF7, false}, {0x40, false}, {0x2C, false},
    {0x00, false}, {0x00, false}, {0x02, false}, {0x00, false},
};

// FFrame::FFrame(UObject*, UStruct*, INT, void*, FFrame*). Every interpreted
// UnrealScript function passes its live UFunction and deserialized Script array
// through this constructor immediately before execution. Hooking it lets the
// cardiac fix activate on demand, even when KidPawnBase loads long after the
// main menu, without polling the entire process address space.
constexpr PatternByte kFrameConstructorPattern[] = {
    {0x6A, false}, {0xFF, false},
    {0x68, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x64, false}, {0xA1, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x50, false}, {0x51, false}, {0x53, false}, {0x55, false}, {0x56, false}, {0x57, false},
    {0xA1, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x33, false}, {0xC4, false}, {0x50, false},
    {0x8D, false}, {0x44, false}, {0x24, false}, {0x18, false},
    {0x64, false}, {0xA3, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x8B, false}, {0xF1, false},
    {0x89, false}, {0x74, false}, {0x24, false}, {0x14, false},
    {0xB8, false}, {0x01, false}, {0x00, false}, {0x00, false}, {0x00, false},
};

// UObject::ProcessInternal. Unique prologue in the supported executable:
//   push -1; push SEH; mov eax,fs:[0]; push eax; sub esp,5Ch; ...
//   mov esi,[esp+80h] (Stack); mov eax,[esi+10h] (Stack.Node);
//   mov ebp,[eax+84h] (FunctionFlags)
// UXAudio2Device::Init (+0x20CAD00, reached through the device vtable):
//   push -1; push SEH; mov eax,fs:[0]; ...; mov esi,ecx; xor ebp,ebp;
//   mov edi,1; mov [esi+220h],ebp; cmp [esi+40h],edi; jge +7;
//   mov dword [esi+40h],32          ; if (MaxChannels < 1) MaxChannels = 32
constexpr PatternByte kAudioDeviceInitPattern[] = {
    {0x6A, false}, {0xFF, false},
    {0x68, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x64, false}, {0xA1, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x50, false}, {0x83, false}, {0xEC, false}, {0x08, false},
    {0x53, false}, {0x55, false}, {0x56, false}, {0x57, false},
    {0xA1, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x33, false}, {0xC4, false}, {0x50, false},
    {0x8D, false}, {0x44, false}, {0x24, false}, {0x1C, false},
    {0x64, false}, {0xA3, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x8B, false}, {0xF1, false},
    {0x33, false}, {0xED, false},
    {0xBF, false}, {0x01, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x89, false}, {0xAE, false}, {0x20, false}, {0x02, false}, {0x00, false}, {0x00, false},
    {0x39, false}, {0x7E, false}, {0x40, false},
    {0x7D, false}, {0x07, false},
    {0xC7, false}, {0x46, false}, {0x40, false}, {0x20, false}, {0x00, false}, {0x00, false}, {0x00, false},
};
constexpr SIZE_T kAudioDeviceInitHookLength = 13;

constexpr PatternByte kProcessInternalPattern[] = {
    {0x6A, false}, {0xFF, false},
    {0x68, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x64, false}, {0xA1, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x50, false}, {0x83, false}, {0xEC, false}, {0x5C, false},
    {0xA1, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x33, false}, {0xC4, false}, {0x89, false}, {0x44, false}, {0x24, false}, {0x58, false},
    {0x53, false}, {0x55, false}, {0x56, false}, {0x57, false},
    {0xA1, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x33, false}, {0xC4, false}, {0x50, false},
    {0x8D, false}, {0x44, false}, {0x24, false}, {0x70, false},
    {0x64, false}, {0xA3, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x8B, false}, {0xB4, false}, {0x24, false}, {0x80, false}, {0x00, false}, {0x00, false}, {0x00, false},
    {0x8B, false}, {0x46, false}, {0x10, false},
    {0x8B, false}, {0xA8, false}, {0x84, false}, {0x00, false}, {0x00, false}, {0x00, false},
};

// UParticleModuleCollision::Update. CollisionCompletionOption == EPCC_Kill
// removes a particle with a generic active-index swap. That is invalid for a
// ribbon emitter because its packed previous/next links are left untouched.
// The complete prologue is unique in the supported retail executable.
constexpr PatternByte kCollisionUpdatePattern[] = {
    {0x55, false}, {0x8B, false}, {0xEC, false},
    {0x83, false}, {0xE4, false}, {0xF0, false},
    {0x6A, false}, {0xFF, false},
    {0x68, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x64, false}, {0xA1, false}, {0x00, false}, {0x00, false},
    {0x00, false}, {0x00, false},
    {0x50, false},
    {0x81, false}, {0xEC, false}, {0x08, false}, {0x03, false},
    {0x00, false}, {0x00, false},
    {0x53, false}, {0x56, false}, {0x57, false},
    {0xA1, false}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x33, false}, {0xC4, false}, {0x50, false},
    {0x8D, false}, {0x84, false}, {0x24, false}, {0x18, false},
    {0x03, false}, {0x00, false}, {0x00, false},
    {0x64, false}, {0xA3, false}, {0x00, false}, {0x00, false},
    {0x00, false}, {0x00, false},
    {0x8B, false}, {0xF1, false},
    {0x89, false}, {0x74, false}, {0x24, false}, {0x14, false},
    {0x68, false}, {0xBA, false}, {0x04, false}, {0x00, false}, {0x00, false},
};

constexpr ptrdiff_t kRibbonKillParticlesRva = 0x011FF8A0;
constexpr ptrdiff_t kRibbonRecalculateTangentsRva = 0x011E28D0;
constexpr ptrdiff_t kEmitterParticleDataOffset = 0x40;
constexpr ptrdiff_t kEmitterParticleIndicesOffset = 0x44;
constexpr ptrdiff_t kEmitterParticleStrideOffset = 0xD0;
constexpr ptrdiff_t kEmitterActiveParticlesOffset = 0xD4;
constexpr ptrdiff_t kParticleRelativeTimeOffset = 0x0C;
constexpr ptrdiff_t kCollisionCompletionOptionOffset = 0x9C;
constexpr ptrdiff_t kCollisionCountPayloadOffset = 0x18;
constexpr SIZE_T kRibbonKillParticlesVtableSlot = 0x88 / sizeof(void*);
constexpr SIZE_T kRibbonRecalculateTangentsVtableSlot = 0xE0 / sizeof(void*);

// Anchor in TickMikaCardiacMassage's loaded UnrealScript bytecode:
//   mRevive_MassageSpeed *= 0.9 * (1.0 / mRevive_MassageHealtDownRate)
// UObject operands are pointers after the package has been deserialized.
constexpr PatternByte kCardiacDecayPattern[] = {
    {0xB6, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xAB, false}, {0x1E, false}, {0x66, false}, {0x66, false}, {0x66, false}, {0x3F, false},
    {0xAC, false}, {0x1E, false}, {0x00, false}, {0x00, false}, {0x80, false}, {0x3F, false},
    {0x01, false}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x16, false}, {0x16, false}, {0x16, false},
};

constexpr PatternByte kCardiacRecoveryPattern[] = {
    {0xB8, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xAB, false}, {0xAB, false},
    {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x1E, false}, {0x00, false}, {0x00, false}, {0xA0, false}, {0x41, false}, {0x16, false},
    {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x16, false}, {0x16, false},
};

constexpr PatternByte kCardiacButtonDownPattern[] = {
    {0xB6, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xAB, false}, {0x1E, false}, {0x33, false}, {0x33, false}, {0x73, false}, {0x3F, false},
    {0xAC, false}, {0x1E, false}, {0x00, false}, {0x00, false}, {0x80, false}, {0x3F, false},
    {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x16, false}, {0x16, false}, {0x16, false},
};

constexpr PatternByte kCardiacDownAddPattern[] = {
    {0xB8, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xAB, false}, {0x1E, false}, {0x00, false}, {0x00, false}, {0x80, false}, {0x3F, false},
    {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x16, false}, {0x16, false},
};

constexpr PatternByte kCardiacDownFloorPattern[] = {
    {0xB0, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x16, false}, {0x0F, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
};

constexpr PatternByte kCardiacDivisorPattern[] = {
    {0x0F, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xAC, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x1E, false}, {0x00, false}, {0x00, false}, {0xC8, false}, {0x42, false}, {0x16, false},
};

constexpr PatternByte kCardiacDeltaThirtyPattern[] = {
    {0xB9, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0xAB, false}, {0x00, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x1E, false}, {0x00, false}, {0x00, false}, {0xF0, false}, {0x41, false},
    {0x16, false}, {0x16, false},
};

constexpr ptrdiff_t kCardiacRecoveryFromDecay = -0x196;
constexpr ptrdiff_t kCardiacButtonDownFromDecay = -0xD7;
constexpr ptrdiff_t kCardiacDownAddFromDecay = -0xA5;
constexpr ptrdiff_t kCardiacDownFloorFromDecay = -0x87;
constexpr ptrdiff_t kCardiacDivisorFromDecay = 0x35;
constexpr ptrdiff_t kCardiacDeltaThirtyFromDecay = 0x5F;
constexpr SIZE_T kCardiacFloatPayloadOffset = 12;

bool ValidateExecutable(HMODULE executable, BYTE** textBegin, SIZE_T* textSize) {
    auto* base = reinterpret_cast<BYTE*>(executable);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        return false;
    }
    if (nt->FileHeader.TimeDateStamp != kSupportedTimestamp ||
        nt->OptionalHeader.SizeOfImage != kSupportedImageSize) {
        Log(L"Unsupported KidGame.exe (timestamp 0x%08X, image size 0x%08X).",
            nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return false;
    }

    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD sectionIndex = 0; sectionIndex < nt->FileHeader.NumberOfSections; ++sectionIndex) {
        const IMAGE_SECTION_HEADER& section = sections[sectionIndex];
        if (memcmp(section.Name, ".text", 5) != 0) {
            continue;
        }
        *textBegin = base + section.VirtualAddress;
        *textSize = section.Misc.VirtualSize;
        return true;
    }
    return false;
}

BYTE* FindUniquePattern(BYTE* begin, SIZE_T sectionSize,
                        const PatternByte* pattern, SIZE_T patternSize,
                        const wchar_t* label) {
    BYTE* match = nullptr;
    unsigned matchCount = 0;
    for (SIZE_T offset = 0; offset + patternSize <= sectionSize; ++offset) {
        bool found = true;
        for (SIZE_T index = 0; index < patternSize; ++index) {
            if (!pattern[index].wildcard && begin[offset + index] != pattern[index].value) {
                found = false;
                break;
            }
        }
        if (found) {
            match = begin + offset;
            ++matchCount;
        }
    }

    if (matchCount != 1) {
        Log(L"%s signature matched %u locations; hook not installed.", label, matchCount);
        return nullptr;
    }
    return match;
}

bool MatchesPattern(const BYTE* begin, const PatternByte* pattern, SIZE_T patternSize) {
    for (SIZE_T index = 0; index < patternSize; ++index) {
        if (!pattern[index].wildcard && begin[index] != pattern[index].value) {
            return false;
        }
    }
    return true;
}

void WriteRelativeJump(BYTE* source, const void* destination) {
    source[0] = 0xE9;
    const intptr_t displacement = reinterpret_cast<const BYTE*>(destination) - (source + 5);
    *reinterpret_cast<LONG*>(source + 1) = static_cast<LONG>(displacement);
}

float SanitizeDelta(float incomingDeltaTime) {
    if (!std::isfinite(incomingDeltaTime) || incomingDeltaTime <= 0.0f) {
        return 1.0f / static_cast<float>(g_uiTickFps);
    }
    return incomingDeltaTime > g_maxDeltaSeconds ? g_maxDeltaSeconds : incomingDeltaTime;
}

float SanitizeParticleDelta(float incomingDeltaTime) {
    if (!std::isfinite(incomingDeltaTime) || incomingDeltaTime <= 0.0f) {
        return 1.0f / 30.0f;
    }
    return incomingDeltaTime > g_maxDeltaSeconds ? g_maxDeltaSeconds : incomingDeltaTime;
}

struct CollisionBudgetState {
    void* emitter = nullptr;
    BYTE* particle = nullptr;
    int remaining = 0;
    float lastRelativeTime = 0.0f;
    double decrementCredit = 0.0;
    bool hasCollided = false;
    uint32_t lastSeen = 0;
};

struct ProtectedRibbonParticle {
    BYTE* particle;
    int* collisionCount;
    CollisionBudgetState* state;
};

CollisionBudgetState g_collisionStates[kCollisionStateSlots] = {};
uint32_t g_collisionUpdateSerial = 0;

SIZE_T CollisionStateHash(void* emitter, const BYTE* particle) {
    uintptr_t value = reinterpret_cast<uintptr_t>(emitter) >> 4;
    value ^= reinterpret_cast<uintptr_t>(particle) >> 4;
    value ^= value >> 11;
    return static_cast<SIZE_T>(value) & (kCollisionStateSlots - 1);
}

CollisionBudgetState* FindCollisionState(void* emitter, BYTE* particle, uint32_t serial) {
    const SIZE_T start = CollisionStateHash(emitter, particle);
    CollisionBudgetState* replacement = nullptr;
    uint32_t oldestAge = 0;
    for (SIZE_T probe = 0; probe < 64; ++probe) {
        CollisionBudgetState& state =
            g_collisionStates[(start + probe) & (kCollisionStateSlots - 1)];
        if (state.emitter == emitter && state.particle == particle) {
            state.lastSeen = serial;
            return &state;
        }
        if (state.emitter == nullptr) {
            replacement = &state;
            break;
        }
        const uint32_t age = serial - state.lastSeen;
        if (age != 0 && (replacement == nullptr || age > oldestAge)) {
            replacement = &state;
            oldestAge = age;
        }
    }
    if (replacement == nullptr) {
        for (SIZE_T index = 0; index < kCollisionStateSlots; ++index) {
            CollisionBudgetState& state = g_collisionStates[index];
            if (state.emitter == emitter && state.particle == particle) {
                state.lastSeen = serial;
                return &state;
            }
            if (state.emitter == nullptr) {
                replacement = &state;
                break;
            }
            const uint32_t age = serial - state.lastSeen;
            if (age != 0 && (replacement == nullptr || age > oldestAge)) {
                replacement = &state;
                oldestAge = age;
            }
        }
    }
    if (replacement == nullptr) {
        return nullptr;
    }
    *replacement = {};
    replacement->emitter = emitter;
    replacement->particle = particle;
    replacement->lastSeen = serial;
    return replacement;
}

bool IsRibbonEmitter(void* emitter, KillParticlesFunction* killParticles) {
    if (emitter == nullptr || g_executableBase == nullptr || killParticles == nullptr) {
        return false;
    }
    void** vtable = *static_cast<void***>(emitter);
    if (vtable == nullptr ||
        vtable[kRibbonKillParticlesVtableSlot] !=
            g_executableBase + kRibbonKillParticlesRva ||
        vtable[kRibbonRecalculateTangentsVtableSlot] !=
            g_executableBase + kRibbonRecalculateTangentsRva) {
        return false;
    }
    *killParticles = reinterpret_cast<KillParticlesFunction>(
        vtable[kRibbonKillParticlesVtableSlot]);
    return true;
}

void ResetCollisionState(CollisionBudgetState* state, int remaining,
                         float relativeTime) {
    state->remaining = remaining;
    state->lastRelativeTime = relativeTime;
    // Preserve the first discrete impact exactly as retail does. Further
    // qualifying contacts consume DeltaTime*30 credit, which is one unit per
    // update at the original 30 FPS and remains 30 units/second at any FPS.
    state->decrementCredit = 0.0;
    state->hasCollided = false;
}

void __fastcall DeltaTimedRibbonCollisionUpdate(
    void* module, void*, void* emitter, int payloadOffset, float incomingDeltaTime) {
    if (!g_particleCollisionFix || module == nullptr || emitter == nullptr ||
        payloadOffset < 0 || payloadOffset > 64 * 1024 ||
        *reinterpret_cast<BYTE*>(static_cast<BYTE*>(module) +
                                 kCollisionCompletionOptionOffset) != 0) {
        g_originalCollisionUpdate(module, emitter, payloadOffset, incomingDeltaTime);
        return;
    }

    KillParticlesFunction killParticles = nullptr;
    if (!IsRibbonEmitter(emitter, &killParticles)) {
        g_originalCollisionUpdate(module, emitter, payloadOffset, incomingDeltaTime);
        return;
    }

    BYTE* particleData = *reinterpret_cast<BYTE**>(
        static_cast<BYTE*>(emitter) + kEmitterParticleDataOffset);
    auto* particleIndices = *reinterpret_cast<uint16_t**>(
        static_cast<BYTE*>(emitter) + kEmitterParticleIndicesOffset);
    const int particleStride = *reinterpret_cast<int*>(
        static_cast<BYTE*>(emitter) + kEmitterParticleStrideOffset);
    const int activeParticles = *reinterpret_cast<int*>(
        static_cast<BYTE*>(emitter) + kEmitterActiveParticlesOffset);
    if (particleData == nullptr || particleIndices == nullptr || particleStride <= 0 ||
        particleStride > 64 * 1024 || activeParticles <= 0 ||
        activeParticles > static_cast<int>(kMaxProtectedRibbonParticles)) {
        if (activeParticles > static_cast<int>(kMaxProtectedRibbonParticles) &&
            InterlockedCompareExchange(&g_particleCollisionCapacityWarning, 1, 0) == 0) {
            Log(L"Ribbon collision fix skipped an emitter with %d active particles.",
                activeParticles);
        }
        g_originalCollisionUpdate(module, emitter, payloadOffset, incomingDeltaTime);
        return;
    }

    const uint32_t serial = ++g_collisionUpdateSerial;
    ProtectedRibbonParticle protectedParticles[kMaxProtectedRibbonParticles] = {};
    int protectedCount = 0;
    for (int activeIndex = 0; activeIndex < activeParticles; ++activeIndex) {
        BYTE* particle = particleData +
            static_cast<SIZE_T>(particleIndices[activeIndex]) * particleStride;
        int* collisionCount = reinterpret_cast<int*>(
            particle + payloadOffset + kCollisionCountPayloadOffset);
        const int engineRemaining = *collisionCount;
        const float relativeTime = *reinterpret_cast<float*>(
            particle + kParticleRelativeTimeOffset);
        if (engineRemaining <= 0 || engineRemaining >= kCollisionCounterSentinel ||
            !std::isfinite(relativeTime)) {
            continue;
        }

        CollisionBudgetState* state = FindCollisionState(emitter, particle, serial);
        if (state == nullptr) {
            continue;
        }
        if (state->remaining <= 0 || state->remaining != engineRemaining ||
            relativeTime + 1.0e-5f < state->lastRelativeTime) {
            ResetCollisionState(state, engineRemaining, relativeTime);
        } else {
            state->lastRelativeTime = relativeTime;
        }

        protectedParticles[protectedCount++] = {particle, collisionCount, state};
        *collisionCount = kCollisionCounterSentinel;
    }

    if (protectedCount == 0) {
        g_originalCollisionUpdate(module, emitter, payloadOffset, incomingDeltaTime);
        return;
    }

    if (InterlockedCompareExchange(&g_particleCollisionSeen, 1, 0) == 0) {
        Log(L"Delta-timed ribbon collision path reached. First incoming delta %.8f.",
            incomingDeltaTime);
    }

    g_originalCollisionUpdate(module, emitter, payloadOffset, incomingDeltaTime);

    const double deltaUnits =
        static_cast<double>(SanitizeParticleDelta(incomingDeltaTime)) * 30.0;
    bool shouldKill = false;
    for (int index = 0; index < protectedCount; ++index) {
        ProtectedRibbonParticle& protectedParticle = protectedParticles[index];
        CollisionBudgetState* state = protectedParticle.state;
        const int observedCounter = *protectedParticle.collisionCount;
        const bool collided = observedCounter < kCollisionCounterSentinel;
        if (collided) {
            int decrement = 0;
            if (!state->hasCollided) {
                // A distinct first impact costs one count at 30 FPS too; do not
                // delay it merely because the rendering step is shorter.
                state->hasCollided = true;
                decrement = 1;
            } else {
                state->decrementCredit += deltaUnits;
                decrement = static_cast<int>(
                    std::floor(state->decrementCredit + 1.0e-9));
                if (decrement > 0) {
                    state->decrementCredit -= static_cast<double>(decrement);
                }
            }
            if (decrement > 0) {
                state->remaining = decrement >= state->remaining
                    ? 0 : state->remaining - decrement;
            }
        }
        *protectedParticle.collisionCount = state->remaining;
        if (state->remaining == 0) {
            *reinterpret_cast<float*>(protectedParticle.particle +
                                      kParticleRelativeTimeOffset) =
                kExpiredParticleRelativeTime;
            state->emitter = nullptr;
            state->particle = nullptr;
            shouldKill = true;
        }
    }

    if (shouldKill) {
        if (InterlockedCompareExchange(&g_particleCollisionKillSeen, 1, 0) == 0) {
            Log(L"Ribbon collision completion redirected through trail-aware KillParticles.");
        }
        killParticles(emitter);
    }
}

void UpdateCardiacDeltaConstants(float deltaTime) {
    void* downOperand = InterlockedCompareExchangePointer(
        &g_cardiacDownScaleOperand, nullptr, nullptr);
    void* decayOperand = InterlockedCompareExchangePointer(
        &g_cardiacDecayScaleOperand, nullptr, nullptr);
    if (downOperand == nullptr || decayOperand == nullptr) {
        return;
    }

    // Preserve the bytecode's original operators and update only their existing
    // FloatConst payloads. Both writes occur on the main game tick thread.
    const float downScale = deltaTime * 30.0f;
    const float decayScale = std::pow(0.9f, deltaTime * 30.0f);
    memcpy(downOperand, &downScale, sizeof(downScale));
    memcpy(decayOperand, &decayScale, sizeof(decayScale));
}

void UpdateUiFadeDeltaConstants(float deltaTime) {
    if (!g_smoothUiExperimental ||
        InterlockedCompareExchange(&g_uiFadePatchActivated, 0, 0) == 0) {
        return;
    }
    const float divisor = 1.0f / deltaTime;
    for (SIZE_T index = 0; index < ArrayCount(g_uiFadeDivisorOperands); ++index) {
        void* operand = g_uiFadeDivisorOperands[index];
        if (operand != nullptr) {
            memcpy(operand, &divisor, sizeof(divisor));
        }
    }
}

// [Diagnostics] FrameRateReport: average, minimum-interval and maximum-interval
// frame rate every five seconds, measured at FGFxEngine::Tick.
void ReportFrameRate() {
    if (!g_frameRateReport) {
        return;
    }
    static LONGLONG frequency = 0;
    static LONGLONG windowStart = 0;
    static LONGLONG lastFrame = 0;
    static LONGLONG shortest = LLONG_MAX;
    static LONGLONG longest = 0;
    static unsigned frames = 0;
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (frequency == 0) {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        frequency = value.QuadPart;
        windowStart = lastFrame = now.QuadPart;
        return;
    }
    const LONGLONG interval = now.QuadPart - lastFrame;
    lastFrame = now.QuadPart;
    shortest = interval < shortest ? interval : shortest;
    longest = interval > longest ? interval : longest;
    ++frames;
    const LONGLONG elapsed = now.QuadPart - windowStart;
    if (elapsed >= frequency * 5) {
        Log(L"Frame rate: %.1f FPS average (fastest frame %.1f FPS, slowest %.1f FPS).",
            frames * static_cast<double>(frequency) / elapsed,
            static_cast<double>(frequency) / shortest, static_cast<double>(frequency) / longest);
        windowStart = now.QuadPart;
        frames = 0;
        shortest = LLONG_MAX;
        longest = 0;
    }
}

void ApplyFrameRateUnlock();

bool g_timingProbeInstalled = false;

// Starts the timing probe 15 s after the first UI frame (so startup and
// loading are excluded), records for 10 s, then reports and removes it.
void ServiceTimingProbe() {
    if (!g_timingProbeInstalled) {
        return;
    }
    static ULONGLONG firstFrame = 0;
    static ULONGLONG started = 0;
    const ULONGLONG now = GetTickCount64();
    if (firstFrame == 0) {
        firstFrame = now;
        return;
    }
    if (started == 0 && now - firstFrame >= 15000) {
        started = now;
        TimingProbeStart();
        Log(L"Timing probe recording.");
    } else if (started != 0 && now - started >= 10000) {
        TimingProbeFinish((now - started) / 1000.0, GetCurrentThreadId());
        g_timingProbeInstalled = false;
    }
}
void LimitFrameRate();

// The game's render window. UE3 sizes its client area to the render
// resolution in fullscreen, borderless and windowed modes, so the client
// rectangle is the canvas size the fade must cover.
HWND g_gameWindow = nullptr;
ULONGLONG g_gameWindowSearchMs = 0;
volatile LONG g_fadeSizeLogged = 0;

BOOL CALLBACK SelectLargestWindow(HWND window, LPARAM parameter) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId() || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    RECT client = {};
    GetClientRect(window, &client);
    auto* best = reinterpret_cast<HWND*>(parameter);
    RECT bestClient = {};
    if (*best != nullptr) {
        GetClientRect(*best, &bestClient);
    }
    if (static_cast<LONGLONG>(client.right) * client.bottom >
        static_cast<LONGLONG>(bestClient.right) * bestClient.bottom) {
        *best = window;
    }
    return TRUE;
}

void UpdateFadeExtent() {
    if (!g_fullscreenFadeFix) {
        return;
    }
    if (g_gameWindow == nullptr || !IsWindow(g_gameWindow)) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_gameWindowSearchMs < 1000) {
            return;
        }
        g_gameWindowSearchMs = now;
        HWND best = nullptr;
        EnumWindows(SelectLargestWindow, reinterpret_cast<LPARAM>(&best));
        g_gameWindow = best;
        if (g_gameWindow == nullptr) {
            return;
        }
    }
    RECT client = {};
    if (!GetClientRect(g_gameWindow, &client) || client.right < 320 || client.bottom < 200 ||
        IsIconic(g_gameWindow)) {
        return;
    }
    const float width = static_cast<float>(client.right);
    const float height = static_cast<float>(client.bottom);
    if (width != g_fadeWidth || height != g_fadeHeight) {
        g_fadeWidth = width;
        g_fadeHeight = height;
        if (InterlockedExchange(&g_fadeSizeLogged, 1) == 0 ||
            width != 1920.0f || height != 1080.0f) {
            Log(L"Cinematic fade now covers %.0fx%.0f.", width, height);
        }
    }
}

// FGFxEngine::Tick runs once per rendered frame on the game thread with the
// real (undilated) frame time. It is the per-frame service point for every
// constant that must follow real frame time.
void __fastcall DynamicMovieTick(void* tickableObject, void*, float incomingDeltaTime) {
    LimitFrameRate();
    ReportFrameRate();
    ServiceTimingProbe();
    ApplyFrameRateUnlock();
    UpdateFadeExtent();
    const float deltaTime = SanitizeDelta(incomingDeltaTime);
    g_movieAdvanceDelta = deltaTime;
    if (InterlockedCompareExchange(&g_movieTickSeen, 1, 0) == 0) {
        Log(L"FGFxEngine dynamic tick reached. First incoming delta %.8f.", incomingDeltaTime);
    }
    g_originalMovieTick(tickableObject, deltaTime);
}

void __fastcall RateLimitedScriptTick(void* tickableObject, void*, float incomingDeltaTime) {
    if (InterlockedCompareExchange(&g_scriptTickSeen, 1, 0) == 0) {
        Log(L"KidGFxMgr script tick reached. First incoming delta %.8f.", incomingDeltaTime);
    }

    const double deltaTime = static_cast<double>(SanitizeDelta(incomingDeltaTime));
    const double messageTickStep = 1.0 / static_cast<double>(kDefaultUiTickFps);
    g_newMessageAccumulatorSeconds += deltaTime;
    int messageSteps = 0;
    while (g_newMessageAccumulatorSeconds + 1.0e-9 >= messageTickStep &&
           messageSteps < g_maxCatchUpTicks) {
        InterlockedIncrement(&g_scriptTickGeneration);
        g_newMessageAccumulatorSeconds -= messageTickStep;
        ++messageSteps;
    }
    if (messageSteps == g_maxCatchUpTicks &&
        g_newMessageAccumulatorSeconds >= messageTickStep) {
        g_newMessageAccumulatorSeconds = std::fmod(
            g_newMessageAccumulatorSeconds, messageTickStep);
    }

    if (g_smoothUiExperimental) {
        // Presentation-safe UI work now receives every rendered frame and its
        // real timestep. Discrete helpers are independently gated by the
        // FFrame dispatcher using g_scriptTickGeneration.
        g_originalScriptTick(tickableObject, static_cast<float>(deltaTime));
        return;
    }

    const double tickStep = 1.0 / static_cast<double>(g_uiTickFps);
    g_uiAccumulatorSeconds += deltaTime;
    int dispatched = 0;
    while (g_uiAccumulatorSeconds + 1.0e-9 >= tickStep && dispatched < g_maxCatchUpTicks) {
        g_originalScriptTick(tickableObject, static_cast<float>(tickStep));
        g_uiAccumulatorSeconds -= tickStep;
        ++dispatched;
    }

    if (dispatched == g_maxCatchUpTicks && g_uiAccumulatorSeconds >= tickStep) {
        g_uiAccumulatorSeconds = std::fmod(g_uiAccumulatorSeconds, tickStep);
        if (InterlockedCompareExchange(&g_uiCatchUpClamped, 1, 0) == 0) {
            Log(L"KidGFxMgr catch-up was clamped to %d ticks after a hitch.", g_maxCatchUpTicks);
        }
    }
}

bool IsCommittedPointer(const void* pointer) {
    MEMORY_BASIC_INFORMATION info = {};
    return pointer != nullptr &&
           VirtualQuery(pointer, &info, sizeof(info)) == sizeof(info) &&
           info.State == MEM_COMMIT && (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

bool IsWritablePointer(const void* pointer) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFF;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool MakeWritablePointer(void* pointer, SIZE_T size) {
    MEMORY_BASIC_INFORMATION info = {};
    if (pointer == nullptr || size == 0 ||
        VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    if (IsWritablePointer(pointer)) {
        return true;
    }

    const DWORD baseProtection = info.Protect & 0xFF;
    const bool executable = baseProtection == PAGE_EXECUTE ||
                            baseProtection == PAGE_EXECUTE_READ ||
                            baseProtection == PAGE_EXECUTE_READWRITE ||
                            baseProtection == PAGE_EXECUTE_WRITECOPY;
    DWORD ignored = 0;
    return VirtualProtect(pointer, size,
                          executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE,
                          &ignored) != FALSE;
}

struct CardiacScanStats {
    SIZE_T anchors = 0;
    SIZE_T completePatterns = 0;
    SIZE_T livePointers = 0;
    SIZE_T validConstants = 0;
};

bool InspectCardiacMassageAt(BYTE* decay, bool activate, CardiacScanStats* stats) {
    constexpr SIZE_T windowBefore = static_cast<SIZE_T>(-kCardiacRecoveryFromDecay);
    constexpr SIZE_T windowSize = windowBefore + kCardiacDeltaThirtyFromDecay +
                                  ArrayCount(kCardiacDeltaThirtyPattern);
    BYTE snapshot[windowSize] = {};
    SIZE_T bytesRead = 0;
    BYTE* windowAddress = decay - windowBefore;
    if (!ReadProcessMemory(GetCurrentProcess(), windowAddress, snapshot, sizeof(snapshot),
                           &bytesRead) || bytesRead != sizeof(snapshot)) {
        return false;
    }

    BYTE* localDecay = snapshot + windowBefore;
    BYTE* recovery = localDecay + kCardiacRecoveryFromDecay;
    BYTE* buttonDown = localDecay + kCardiacButtonDownFromDecay;
    BYTE* downAdd = localDecay + kCardiacDownAddFromDecay;
    BYTE* downFloor = localDecay + kCardiacDownFloorFromDecay;
    BYTE* divisor = localDecay + kCardiacDivisorFromDecay;
    BYTE* deltaThirty = localDecay + kCardiacDeltaThirtyFromDecay;

    if (!MatchesPattern(recovery, kCardiacRecoveryPattern, ArrayCount(kCardiacRecoveryPattern)) ||
        !MatchesPattern(buttonDown, kCardiacButtonDownPattern, ArrayCount(kCardiacButtonDownPattern)) ||
        !MatchesPattern(downAdd, kCardiacDownAddPattern, ArrayCount(kCardiacDownAddPattern)) ||
        !MatchesPattern(downFloor, kCardiacDownFloorPattern, ArrayCount(kCardiacDownFloorPattern)) ||
        !MatchesPattern(divisor, kCardiacDivisorPattern, ArrayCount(kCardiacDivisorPattern)) ||
        !MatchesPattern(deltaThirty, kCardiacDeltaThirtyPattern, ArrayCount(kCardiacDeltaThirtyPattern))) {
        return false;
    }
    ++stats->completePatterns;

    // Loaded UnrealScript stores each UObject operand in an eight-byte VM slot,
    // even in this 32-bit build.  The usable pointer is the low DWORD at +2.
    DWORD deltaProperty = 0;
    memcpy(&deltaProperty, deltaThirty + 2, sizeof(deltaProperty));
    if (!IsCommittedPointer(reinterpret_cast<const void*>(static_cast<uintptr_t>(deltaProperty)))) {
        return false; // This is serialized package data, not executable VM bytecode.
    }
    ++stats->livePointers;

    float downScale = 0.0f;
    float decayScale = 0.0f;
    memcpy(&downScale, downAdd + kCardiacFloatPayloadOffset, sizeof(downScale));
    memcpy(&decayScale, localDecay + kCardiacFloatPayloadOffset, sizeof(decayScale));
    if (downScale < 0.9999f || downScale > 1.0001f ||
        decayScale < 0.8999f || decayScale > 0.9001f) {
        return false;
    }
    ++stats->validConstants;

    if (!activate) {
        return true;
    }

    // Publish the addresses only after the complete function signature and
    // live UObject operands have been validated. No VM opcode is rewritten.
    void* downScaleOperand = decay + kCardiacDownAddFromDecay + kCardiacFloatPayloadOffset;
    void* decayScaleOperand = decay + kCardiacFloatPayloadOffset;
    if (!MakeWritablePointer(downScaleOperand, sizeof(float)) ||
        !MakeWritablePointer(decayScaleOperand, sizeof(float))) {
        return false;
    }
    InterlockedExchangePointer(&g_cardiacDownScaleOperand, downScaleOperand);
    InterlockedExchangePointer(&g_cardiacDecayScaleOperand, decayScaleOperand);
    UpdateCardiacDeltaConstants(g_movieAdvanceDelta);
    return true;
}

struct ScriptArrayHeader {
    BYTE* data;
    int count;
    int capacity;
};

// UnrealScript packages other than the core script packages are loaded and
// unloaded with levels. A UFunction recorded at discovery may therefore be
// freed later and its address reused. Every write into a function's bytecode,
// and every redirect to its return code, first confirms that the function
// still owns the exact Script array recorded at discovery.
struct ScriptBinding {
    void* function = nullptr;
    BYTE* data = nullptr;
    int count = 0;
};

bool ReadScriptHeaderSafe(void* function, ScriptArrayHeader* header) {
    if (function == nullptr) {
        return false;
    }
    __try {
        memcpy(header, static_cast<BYTE*>(function) + 0x54, sizeof(*header));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool BindScript(ScriptBinding* binding, void* function) {
    ScriptArrayHeader header = {};
    if (!ReadScriptHeaderSafe(function, &header) || header.data == nullptr) {
        return false;
    }
    binding->function = function;
    binding->data = header.data;
    binding->count = header.count;
    return true;
}

bool ScriptBindingLive(const ScriptBinding& binding, void* function) {
    ScriptArrayHeader header = {};
    return function != nullptr && function == binding.function &&
           ReadScriptHeaderSafe(function, &header) && header.data == binding.data &&
           header.count == binding.count;
}

// Return-code redirects point at the function's own `return; EndOfScript`
// tail. Confirm it is still the tail of this function's current Script.
bool ReturnCodeLive(void* function, const void* returnCode) {
    ScriptArrayHeader header = {};
    return returnCode != nullptr && ReadScriptHeaderSafe(function, &header) &&
           header.data != nullptr && header.count >= 3 &&
           returnCode == header.data + header.count - 3;
}

ScriptBinding g_cardiacBinding;
ScriptBinding g_uiFadeBinding;
ScriptBinding g_sebastianBinding;

// Every UFunction that any patch acts on is recorded here when it is armed.
// The script hooks see every UnrealScript call; for all other functions they
// do one lookup in this set and return to the engine immediately.
constexpr SIZE_T kInterestSlots = 2048;  // power of two
void* volatile g_interestingFunctions[kInterestSlots] = {};

SIZE_T InterestSlot(const void* function) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(function);
    return ((value >> 4) ^ (value >> 15)) & (kInterestSlots - 1);
}

void MarkInteresting(void* function) {
    if (function == nullptr) {
        return;
    }
    SIZE_T slot = InterestSlot(function);
    for (SIZE_T probe = 0; probe < kInterestSlots; ++probe) {
        void* existing = InterlockedCompareExchangePointer(
            &g_interestingFunctions[slot], function, nullptr);
        if (existing == nullptr || existing == function) {
            return;
        }
        slot = (slot + 1) & (kInterestSlots - 1);
    }
}

bool IsInteresting(const void* function) {
    SIZE_T slot = InterestSlot(function);
    for (SIZE_T probe = 0; probe < kInterestSlots; ++probe) {
        const void* existing = g_interestingFunctions[slot];
        if (existing == function) {
            return true;
        }
        if (existing == nullptr) {
            return false;
        }
        slot = (slot + 1) & (kInterestSlots - 1);
    }
    return false;
}

// First-invocation detection for the FFrame hook. Open addressing without
// eviction, so a function is inspected once, not again every time another
// function shares its slot. Each entry also records the function's Script
// pointer: a function allocated at a recycled address (after its package was
// unloaded and another loaded) has a different Script and is inspected anew.
struct CheckedFunction {
    void* volatile function;
    void* volatile script;
};
constexpr SIZE_T kCheckedFunctionSlots = 131072;  // power of two
CheckedFunction g_checkedFunctions[kCheckedFunctionSlots] = {};

void* FunctionScriptPointer(void* function) {
    return *reinterpret_cast<void**>(static_cast<BYTE*>(function) + 0x54);
}

bool ClaimFunctionForInspection(CheckedFunction* table, void* function) {
    if (function == nullptr) {
        return false;
    }
    void* script = FunctionScriptPointer(function);
    const uintptr_t value = reinterpret_cast<uintptr_t>(function);
    SIZE_T slot = ((value >> 4) ^ (value >> 17)) & (kCheckedFunctionSlots - 1);
    for (SIZE_T probe = 0; probe < 64; ++probe) {
        CheckedFunction& entry = table[slot];
        void* existing = entry.function;
        if (existing == function) {
            if (entry.script == script) {
                return false;
            }
            entry.script = script;  // Same address, new function.
            return true;
        }
        if (existing == nullptr) {
            entry.script = script;
            entry.function = function;
            return true;
        }
        slot = (slot + 1) & (kCheckedFunctionSlots - 1);
    }
    // Pathological clustering: fall back to treating the call as new, which
    // only repeats a discovery check.
    return true;
}

bool IsObjectName(void* object, const char* targetAnsi, const wchar_t* targetWide,
                  volatile LONG* cachedNameIndex) {
    if (object == nullptr || targetAnsi == nullptr || targetWide == nullptr ||
        cachedNameIndex == nullptr || g_executableBase == nullptr) {
        return false;
    }

    HANDLE process = GetCurrentProcess();
    SIZE_T bytesRead = 0;
    int nameIndex = -1;
    if (!ReadProcessMemory(process, static_cast<const BYTE*>(object) + 0x2C,
                           &nameIndex, sizeof(nameIndex), &bytesRead) ||
        bytesRead != sizeof(nameIndex)) {
        return false;
    }
    const LONG knownIndex = InterlockedCompareExchange(
        cachedNameIndex, -1, -1);
    if (knownIndex >= 0) {
        return nameIndex == knownIndex;
    }

    void** names = nullptr;
    int nameCount = 0;
    bytesRead = 0;
    if (!ReadProcessMemory(process, g_executableBase + kGNamesDataRva,
                           &names, sizeof(names), &bytesRead) || bytesRead != sizeof(names)) {
        return false;
    }
    bytesRead = 0;
    if (!ReadProcessMemory(process, g_executableBase + kGNamesCountRva,
                           &nameCount, sizeof(nameCount), &bytesRead) ||
        bytesRead != sizeof(nameCount) || names == nullptr ||
        nameCount <= 0 || nameCount > 4 * 1024 * 1024 ||
        nameIndex < 0 || nameIndex >= nameCount) {
        return false;
    }

    BYTE* entry = nullptr;
    bytesRead = 0;
    if (!ReadProcessMemory(process, names + nameIndex, &entry, sizeof(entry), &bytesRead) ||
        bytesRead != sizeof(entry) || entry == nullptr) {
        return false;
    }

    BYTE flags = 0;
    bytesRead = 0;
    if (!ReadProcessMemory(process, entry + 8, &flags, sizeof(flags), &bytesRead) ||
        bytesRead != sizeof(flags)) {
        return false;
    }

    bool matches = false;
    if ((flags & 1) != 0) {
        wchar_t name[64] = {};
        bytesRead = 0;
        if (ReadProcessMemory(process, entry + 0x10, name, sizeof(name) - sizeof(wchar_t),
                              &bytesRead)) {
            name[ArrayCount(name) - 1] = L'\0';
            matches = wcscmp(name, targetWide) == 0;
        }
    } else {
        char name[64] = {};
        bytesRead = 0;
        if (ReadProcessMemory(process, entry + 0x10, name, sizeof(name) - 1,
                              &bytesRead)) {
            name[ArrayCount(name) - 1] = '\0';
            matches = strcmp(name, targetAnsi) == 0;
        }
    }
    if (matches) {
        InterlockedCompareExchange(cachedNameIndex, nameIndex, -1);
    }
    return matches;
}

bool ReadNameByIndex(int nameIndex, wchar_t* output, SIZE_T outputCount) {
    if (output == nullptr || outputCount == 0 || g_executableBase == nullptr) {
        return false;
    }
    output[0] = L'\0';

    HANDLE process = GetCurrentProcess();
    SIZE_T bytesRead = 0;
    void** names = nullptr;
    int nameCount = 0;
    if (!ReadProcessMemory(process, g_executableBase + kGNamesDataRva,
                           &names, sizeof(names), &bytesRead) ||
        bytesRead != sizeof(names) || names == nullptr ||
        !ReadProcessMemory(process, g_executableBase + kGNamesCountRva,
                           &nameCount, sizeof(nameCount), &bytesRead) ||
        bytesRead != sizeof(nameCount) || nameCount <= 0 ||
        nameCount > 4 * 1024 * 1024 || nameIndex < 0 || nameIndex >= nameCount) {
        return false;
    }

    BYTE* entry = nullptr;
    BYTE flags = 0;
    if (!ReadProcessMemory(process, names + nameIndex, &entry, sizeof(entry), &bytesRead) ||
        bytesRead != sizeof(entry) || entry == nullptr ||
        !ReadProcessMemory(process, entry + 8, &flags, sizeof(flags), &bytesRead) ||
        bytesRead != sizeof(flags)) {
        return false;
    }

    if ((flags & 1) != 0) {
        const SIZE_T byteCapacity = (outputCount - 1) * sizeof(wchar_t);
        if (!ReadProcessMemory(process, entry + 0x10, output, byteCapacity, &bytesRead)) {
            output[0] = L'\0';
            return false;
        }
        output[outputCount - 1] = L'\0';
        return true;
    }

    char ansi[128] = {};
    const SIZE_T ansiCapacity = outputCount < ArrayCount(ansi) ? outputCount : ArrayCount(ansi);
    if (ansiCapacity < 2 ||
        !ReadProcessMemory(process, entry + 0x10, ansi, ansiCapacity - 1, &bytesRead)) {
        return false;
    }
    ansi[ansiCapacity - 1] = '\0';
    return MultiByteToWideChar(CP_ACP, 0, ansi, -1, output,
                               static_cast<int>(outputCount)) > 0;
}

bool ReadObjectName(void* object, wchar_t* output, SIZE_T outputCount) {
    if (object == nullptr || output == nullptr || outputCount == 0) {
        return false;
    }

    int nameParts[2] = {-1, 0};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           static_cast<const BYTE*>(object) + 0x2C,
                           nameParts, sizeof(nameParts), &bytesRead) ||
        bytesRead != sizeof(nameParts) ||
        !ReadNameByIndex(nameParts[0], output, outputCount)) {
        return false;
    }
    if (nameParts[1] > 0) {
        const SIZE_T used = wcslen(output);
        if (used < outputCount) {
            _snwprintf_s(output + used, outputCount - used, _TRUNCATE,
                         L"_%d", nameParts[1] - 1);
        }
    }
    return true;
}

void* ReadObjectPointer(void* object, ptrdiff_t offset) {
    if (object == nullptr) {
        return nullptr;
    }
    void* value = nullptr;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           static_cast<const BYTE*>(object) + offset,
                           &value, sizeof(value), &bytesRead) ||
        bytesRead != sizeof(value)) {
        return nullptr;
    }
    return value;
}

bool IsFunctionOuterName(void* function, const char* targetAnsi,
                         const wchar_t* targetWide, volatile LONG* cachedNameIndex) {
    return IsObjectName(ReadObjectPointer(function, 0x28), targetAnsi,
                        targetWide, cachedNameIndex);
}

bool HasOuterName(void* object, const char* targetAnsi,
                  const wchar_t* targetWide, volatile LONG* cachedNameIndex) {
    void* current = object;
    for (int depth = 0; depth < 12 && current != nullptr; ++depth) {
        if (IsObjectName(current, targetAnsi, targetWide, cachedNameIndex)) {
            return true;
        }
        current = ReadObjectPointer(current, 0x28);
    }
    return false;
}

void RegisterUiFrameGateFunction(void* function, int targetIndex) {
    MarkInteresting(function);
    const uintptr_t key = reinterpret_cast<uintptr_t>(function);
    SIZE_T slot = ((key >> 4) ^ (key >> 11)) & (kUiFrameGateDispatchSlots - 1);
    for (SIZE_T probe = 0; probe < kUiFrameGateDispatchSlots; ++probe) {
        UiFrameGateDispatch& entry = g_uiFrameGateDispatch[slot];
        if (entry.function == nullptr || entry.function == function) {
            entry.targetIndex = targetIndex;
            entry.function = function;
            return;
        }
        slot = (slot + 1) & (kUiFrameGateDispatchSlots - 1);
    }
}

int FindUiFrameGateTargetForFunction(void* function) {
    if (function == nullptr) {
        return -1;
    }
    const uintptr_t key = reinterpret_cast<uintptr_t>(function);
    SIZE_T slot = ((key >> 4) ^ (key >> 11)) & (kUiFrameGateDispatchSlots - 1);
    for (SIZE_T probe = 0; probe < kUiFrameGateDispatchSlots; ++probe) {
        const UiFrameGateDispatch& entry = g_uiFrameGateDispatch[slot];
        if (entry.function == function) {
            return entry.targetIndex;
        }
        if (entry.function == nullptr) {
            break;
        }
        slot = (slot + 1) & (kUiFrameGateDispatchSlots - 1);
    }
    return -1;
}

UiFrameGateState* FindOrCreateUiFrameGateState(void* object, int targetIndex) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(object);
    SIZE_T slot = ((key >> 4) ^ (key >> 12) ^
                   (static_cast<uintptr_t>(targetIndex) * 0x9E3779B1u)) &
                  (kUiFrameGateStateSlots - 1);
    for (SIZE_T probe = 0; probe < kUiFrameGateStateSlots; ++probe) {
        UiFrameGateState& state = g_uiFrameGateStates[slot];
        if (state.object == object && state.targetIndex == targetIndex) {
            return &state;
        }
        if (state.object == nullptr) {
            state.object = object;
            state.targetIndex = targetIndex;
            state.lastGeneration = LONG_MIN;
            return &state;
        }
        slot = (slot + 1) & (kUiFrameGateStateSlots - 1);
    }
    return nullptr;
}

// [Diagnostics] SmoothUIGate: extra "Outer.Function" patterns (Outer may end
// in '*' as a prefix wildcard) that stay on the 30 Hz gate in smooth-UI mode.
// Used to bisect UI behaviour; empty by default.
struct UiExtraGatePattern {
    wchar_t outer[64];
    wchar_t function[64];
    bool prefix;
};
struct UiExtraGate {
    void* function;
    void* returnCode;
};
constexpr SIZE_T kMaxUiExtraPatterns = 16;
constexpr SIZE_T kMaxUiExtraGates = 96;
UiExtraGatePattern g_uiExtraPatterns[kMaxUiExtraPatterns] = {};
SIZE_T g_uiExtraPatternCount = 0;
UiExtraGate g_uiExtraGates[kMaxUiExtraGates] = {};
volatile LONG g_uiExtraGateCount = 0;

void LoadSmoothUiGatePatterns() {
    wchar_t list[1024] = {};
    GetPrivateProfileStringW(L"Diagnostics", L"SmoothUIGate", L"", list,
                             static_cast<DWORD>(ArrayCount(list)), g_iniPath);
    wchar_t* context = nullptr;
    for (wchar_t* token = wcstok_s(list, L", ", &context);
         token != nullptr && g_uiExtraPatternCount < kMaxUiExtraPatterns;
         token = wcstok_s(nullptr, L", ", &context)) {
        const wchar_t* dot = wcschr(token, L'.');
        if (dot == nullptr) {
            continue;
        }
        UiExtraGatePattern& pattern = g_uiExtraPatterns[g_uiExtraPatternCount++];
        wcsncpy_s(pattern.outer, token, static_cast<size_t>(dot - token));
        wcsncpy_s(pattern.function, dot + 1, _TRUNCATE);
        const size_t length = wcslen(pattern.outer);
        pattern.prefix = length > 0 && pattern.outer[length - 1] == L'*';
        if (pattern.prefix) {
            pattern.outer[length - 1] = 0;
        }
    }
    if (g_uiExtraPatternCount != 0) {
        Log(L"Smooth UI: %Iu extra 30 Hz gate pattern(s) configured.", g_uiExtraPatternCount);
    }
}

BYTE* FindVoidReturnCode(void* function) {
    ScriptArrayHeader script = {};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), static_cast<const BYTE*>(function) + 0x54,
                           &script, sizeof(script), &bytesRead) ||
        bytesRead != sizeof(script) || script.data == nullptr || script.count < 3 ||
        script.count > 1024 * 1024 || script.capacity < script.count) {
        return nullptr;
    }
    constexpr BYTE returnVoidAndEnd[] = {0x04, 0x0B, 0x53};
    BYTE* returnCode = script.data + script.count - ArrayCount(returnVoidAndEnd);
    BYTE terminator[ArrayCount(returnVoidAndEnd)] = {};
    if (!ReadProcessMemory(GetCurrentProcess(), returnCode, terminator, sizeof(terminator),
                           &bytesRead) ||
        bytesRead != sizeof(terminator) ||
        memcmp(terminator, returnVoidAndEnd, sizeof(terminator)) != 0) {
        return nullptr;
    }
    return returnCode;
}

void TryDiscoverExtraUiGate(void* function) {
    if (g_uiExtraPatternCount == 0) {
        return;
    }
    wchar_t name[64] = {};
    wchar_t outer[64] = {};
    if (!ReadObjectName(function, name, ArrayCount(name)) ||
        !ReadObjectName(ReadObjectPointer(function, kUObjectOuterOffset), outer,
                        ArrayCount(outer))) {
        return;
    }
    for (SIZE_T index = 0; index < g_uiExtraPatternCount; ++index) {
        const UiExtraGatePattern& pattern = g_uiExtraPatterns[index];
        const bool outerMatches = pattern.prefix
            ? _wcsnicmp(outer, pattern.outer, wcslen(pattern.outer)) == 0
            : _wcsicmp(outer, pattern.outer) == 0;
        if (!outerMatches || _wcsicmp(name, pattern.function) != 0) {
            continue;
        }
        BYTE* returnCode = FindVoidReturnCode(function);
        const LONG slot = g_uiExtraGateCount;
        if (returnCode == nullptr || slot >= static_cast<LONG>(kMaxUiExtraGates)) {
            return;
        }
        g_uiExtraGates[slot] = {function, returnCode};
        InterlockedIncrement(&g_uiExtraGateCount);
        RegisterUiFrameGateFunction(function, static_cast<int>(kUiFrameGateTargetCount + slot));
        Log(L"Smooth UI: extra 30 Hz gate armed for %s.%s.", outer, name);
        return;
    }
}

void* UiGateReturnCode(int targetIndex) {
    if (targetIndex < static_cast<int>(kUiFrameGateTargetCount)) {
        return g_uiFrameGateTargets[targetIndex].returnCode;
    }
    const int extra = targetIndex - static_cast<int>(kUiFrameGateTargetCount);
    return extra < g_uiExtraGateCount ? g_uiExtraGates[extra].returnCode : nullptr;
}

void TryDiscoverUiFrameGateFunction(void* function) {
    if (!g_smoothUiExperimental || function == nullptr ||
        FindUiFrameGateTargetForFunction(function) >= 0) {
        return;
    }
    TryDiscoverExtraUiGate(function);
    if (FindUiFrameGateTargetForFunction(function) >= 0) {
        return;
    }
    for (SIZE_T index = 0; index < kUiFrameGateTargetCount; ++index) {
        UiFrameGateTarget& target = g_uiFrameGateTargets[index];
        if (target.function == function ||
            !IsObjectName(function, target.functionName, target.functionNameWide,
                          &target.functionNameIndex) ||
            !IsFunctionOuterName(function, target.outerName, target.outerNameWide,
                                 &target.outerNameIndex)) {
            continue;
        }

        ScriptArrayHeader script = {};
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                               static_cast<const BYTE*>(function) + 0x54,
                               &script, sizeof(script), &bytesRead) ||
            bytesRead != sizeof(script) || script.data == nullptr ||
            script.count < 3 || script.count > 1024 * 1024 ||
            script.capacity < script.count) {
            return;
        }
        constexpr BYTE returnVoidAndEnd[] = {0x04, 0x0B, 0x53};
        BYTE* returnCode = script.data + script.count - ArrayCount(returnVoidAndEnd);
        BYTE terminator[ArrayCount(returnVoidAndEnd)] = {};
        if (!ReadProcessMemory(GetCurrentProcess(), returnCode, terminator,
                               sizeof(terminator), &bytesRead) ||
            bytesRead != sizeof(terminator) ||
            memcmp(terminator, returnVoidAndEnd, sizeof(terminator)) != 0) {
            return;
        }
        InterlockedExchangePointer(&target.returnCode, returnCode);
        InterlockedExchangePointer(&target.function, function);
        RegisterUiFrameGateFunction(function, static_cast<int>(index));
        Log(L"Smooth UI: discrete 30 Hz gate armed for %S.%S.",
            target.outerName, target.functionName);
        return;
    }
}

bool ShouldSuppressUiFrameGate(void* object, void* function, void** returnCode) {
    if (!g_smoothUiExperimental || object == nullptr || returnCode == nullptr) {
        return false;
    }
    const int targetIndex = FindUiFrameGateTargetForFunction(function);
    if (targetIndex < 0 || g_scriptTickLimiterInstalled == 0) {
        return false;
    }
    UiFrameGateState* state = FindOrCreateUiFrameGateState(object, targetIndex);
    if (state == nullptr) {
        return false;
    }
    const LONG generation = g_scriptTickGeneration;
    if (state->lastGeneration == LONG_MIN || state->lastGeneration != generation) {
        state->lastGeneration = generation;
        return false;
    }
    void* verifiedReturnCode = UiGateReturnCode(targetIndex);
    if (!ReturnCodeLive(function, verifiedReturnCode)) {
        return false;
    }
    *returnCode = verifiedReturnCode;
    return true;
}

void TryActivateSmoothUiManagerPatch(void* function) {
    if (!g_smoothUiExperimental || function == nullptr ||
        function == g_uiFadeBinding.function ||
        !IsObjectName(function, "Tick", L"Tick", &g_uiManagerTickNameIndex) ||
        !IsFunctionOuterName(function, "KidGFxMgr", L"KidGFxMgr",
                             &g_uiManagerClassNameIndex)) {
        return;
    }

    ScriptArrayHeader script = {};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           static_cast<const BYTE*>(function) + 0x54,
                           &script, sizeof(script), &bytesRead) ||
        bytesRead != sizeof(script) || script.data == nullptr ||
        script.count < 64 || script.count > 1024 * 1024 ||
        script.capacity < script.count) {
        Log(L"Smooth UI: KidGFxMgr.Tick Script array was invalid; continuous fade correction inactive.");
        return;
    }

    // KidGFxMgr.Tick has exactly two FloatConst(30.0) operands, both used as
    // the divisor in FadeSpeed/30. Replacing 30 with 1/DeltaTime preserves the
    // original one-second fade rate at arbitrary render cadence.
    constexpr BYTE floatThirty[] = {0x1E, 0x00, 0x00, 0xF0, 0x41};
    BYTE* operands[2] = {};
    SIZE_T found = 0;
    for (int offset = 0;
         offset + static_cast<int>(ArrayCount(floatThirty)) <= script.count;
         ++offset) {
        if (memcmp(script.data + offset, floatThirty, sizeof(floatThirty)) == 0) {
            if (found < ArrayCount(operands)) {
                operands[found] = script.data + offset + 1;
            }
            ++found;
        }
    }
    if (found != ArrayCount(operands) ||
        !MakeWritablePointer(operands[0], sizeof(float)) ||
        !MakeWritablePointer(operands[1], sizeof(float))) {
        Log(L"Smooth UI: KidGFxMgr.Tick fade signature found %Iu operands instead of 2; correction inactive.", found);
        return;
    }
    InterlockedExchangePointer(&g_uiFadeDivisorOperands[0], operands[0]);
    InterlockedExchangePointer(&g_uiFadeDivisorOperands[1], operands[1]);
    BindScript(&g_uiFadeBinding, function);
    MarkInteresting(function);
    InterlockedExchange(&g_uiFadePatchActivated, 1);
    UpdateUiFadeDeltaConstants(g_movieAdvanceDelta);
    Log(L"Smooth UI: KidGFxMgr.Tick now runs at render rate; continuous fades use DeltaTime.");
}

int FindSoundRetriggerTargetForFunction(void* function) {
    // This predicate is reached for every UnrealScript function execution.
    // The function pointers are published once during discovery and never
    // changed afterwards, so ordinary volatile loads are sufficient here and
    // avoid four interlocked operations in the global hot path.
    for (SIZE_T index = 0; index < kSoundRetriggerTargetCount; ++index) {
        PVOID targetFunction = g_soundRetriggerTargets[index].function;
        if (targetFunction != nullptr && function == targetFunction) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

SoundRetriggerState* FindOrCreateSoundRetriggerState(void* object, int targetIndex) {
    SoundRetriggerState* empty = nullptr;
    for (SIZE_T index = 0; index < kSoundRetriggerStateSlots; ++index) {
        SoundRetriggerState& state = g_soundRetriggerStates[index];
        if (state.object == object && state.targetIndex == targetIndex) {
            return &state;
        }
        if (state.object == nullptr && empty == nullptr) {
            empty = &state;
        }
    }
    if (empty != nullptr) {
        empty->object = object;
        empty->targetIndex = targetIndex;
        empty->lastGeneration = LONG_MIN;
        return empty;
    }
    return nullptr;
}

void TryDiscoverSoundRetriggerFunction(void* function) {
    if (!g_soundRetriggerFix || function == nullptr) {
        return;
    }

    if (FindSoundRetriggerTargetForFunction(function) >= 0) {
        return;
    }

    const SIZE_T probeIndex =
        (reinterpret_cast<uintptr_t>(function) >> 4) & (kSoundRetriggerProbeSlots - 1);
    SoundRetriggerProbe& probe = g_soundRetriggerProbes[probeIndex];
    if (probe.function == function) {
        return;
    }
    probe.function = function;

    for (SIZE_T index = 0; index < kSoundRetriggerTargetCount; ++index) {
        SoundRetriggerTarget& target = g_soundRetriggerTargets[index];
        if (target.function == function ||
            !IsObjectName(function, target.functionName, target.functionNameWide,
                          &target.functionNameIndex) ||
            !IsFunctionOuterName(function, target.outerName, target.outerNameWide,
                                 &target.outerNameIndex)) {
            continue;
        }

        ScriptArrayHeader script = {};
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                               static_cast<const BYTE*>(function) + 0x54,
                               &script, sizeof(script), &bytesRead) ||
            bytesRead != sizeof(script) || script.data == nullptr ||
            script.count < 3 || script.count > 1024 * 1024 ||
            script.capacity < script.count) {
            Log(L"Sound retrigger target %S.%S has an invalid Script array; gate inactive.",
                target.outerName, target.functionName);
            return;
        }

        // The known targets are void helpers. Reuse their own tail to skip a
        // duplicate call without inventing bytecode or modifying function data.
        constexpr BYTE returnVoidAndEnd[] = {0x04, 0x0B, 0x53};
        BYTE* returnCode = script.data + script.count - ArrayCount(returnVoidAndEnd);
        BYTE terminator[ArrayCount(returnVoidAndEnd)] = {};
        if (!ReadProcessMemory(GetCurrentProcess(), returnCode, terminator,
                               sizeof(terminator), &bytesRead) ||
            bytesRead != sizeof(terminator) ||
            memcmp(terminator, returnVoidAndEnd, sizeof(terminator)) != 0) {
            Log(L"Sound retrigger target %S.%S lacks the expected void return; gate inactive.",
                target.outerName, target.functionName);
            return;
        }

        InterlockedExchangePointer(&target.returnCode, returnCode);
        InterlockedExchangePointer(&target.function, function);
        MarkInteresting(function);
        Log(L"Sound retrigger gate armed for %S.%S; duplicate calls now share the 30 Hz simulation cadence.",
            target.outerName, target.functionName);
        return;
    }
}

bool ShouldSuppressSoundRetrigger(void* object, void* function, void** returnCode) {
    if (!g_soundRetriggerFix || returnCode == nullptr) {
        return false;
    }

    const int targetIndex = FindSoundRetriggerTargetForFunction(function);
    if (targetIndex < 0 || g_scriptTickLimiterInstalled == 0) {
        return false;
    }
    SoundRetriggerTarget& target = g_soundRetriggerTargets[targetIndex];
    void* verifiedReturnCode = target.returnCode;
    if (!ReturnCodeLive(function, verifiedReturnCode)) {
        return false;
    }

    SoundRetriggerState* state = FindOrCreateSoundRetriggerState(object, targetIndex);
    if (state == nullptr) {
        return false;
    }
    const LONG generation = g_scriptTickGeneration;
    if (state->lastGeneration == LONG_MIN || state->lastGeneration != generation) {
        state->lastGeneration = generation;
        return false;
    }

    if (InterlockedCompareExchange(&target.suppressionLogged, 1, 0) == 0) {
        Log(L"Sound retrigger gate is suppressing duplicate %S.%S calls.",
            target.outerName, target.functionName);
    }
    *returnCode = verifiedReturnCode;
    return true;
}

int FindFixedStepTargetForFunction(void* function) {
    for (SIZE_T index = 0; index < kFixedStepTargetCount; ++index) {
        PVOID targetFunction = g_fixedStepTargets[index].function;
        if (targetFunction != nullptr && function == targetFunction) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

FixedStepState* FindOrCreateFixedStepState(void* object, int targetIndex, ULONGLONG now) {
    FixedStepState* empty = nullptr;
    FixedStepState* stale = nullptr;
    for (SIZE_T index = 0; index < kFixedStepStateSlots; ++index) {
        FixedStepState& state = g_fixedStepStates[index];
        if (state.object == object && state.targetIndex == targetIndex) {
            return &state;
        }
        if (state.object == nullptr && empty == nullptr) {
            empty = &state;
        }
        // Controllers are destroyed between encounters. An entry idle for
        // several seconds belongs to a dead or inactive object and is reused.
        if (state.object != nullptr && now - state.lastCallMs > 5000 &&
            (stale == nullptr || state.lastCallMs < stale->lastCallMs)) {
            stale = &state;
        }
    }
    if (empty == nullptr) {
        empty = stale;
    }
    if (empty != nullptr) {
        *empty = {};
        empty->object = object;
        empty->targetIndex = targetIndex;
        return empty;
    }
    return nullptr;
}

// UStruct::Children is the parameter/local chain; parameters come first.
// The fixed-step fix rewrites the first parameter, so require it to be a
// FloatProperty named DeltaTime at frame offset zero.
bool HasLeadingDeltaTimeParameter(void* function) {
    void* child = ReadObjectPointer(function, kStructChildrenOffset);
    if (child == nullptr) {
        return false;
    }
    wchar_t childName[64] = {};
    wchar_t className[64] = {};
    int propertyOffset = -1;
    SIZE_T bytesRead = 0;
    return ReadObjectName(child, childName, ArrayCount(childName)) &&
           _wcsicmp(childName, L"DeltaTime") == 0 &&
           ReadObjectName(ReadObjectPointer(child, kUObjectClassOffset), className,
                          ArrayCount(className)) &&
           wcscmp(className, L"FloatProperty") == 0 &&
           ReadProcessMemory(GetCurrentProcess(),
                             static_cast<BYTE*>(child) + kPropertyOffsetOffset,
                             &propertyOffset, sizeof(propertyOffset), &bytesRead) &&
           bytesRead == sizeof(propertyOffset) && propertyOffset == 0;
}

void TryDiscoverFixedStepFunction(void* function) {
    if (function == nullptr || FindFixedStepTargetForFunction(function) >= 0) {
        return;
    }

    for (SIZE_T index = 0; index < kFixedStepTargetCount; ++index) {
        FixedStepTarget& target = g_fixedStepTargets[index];
        if (!*target.enabled ||
            target.function == function ||
            !IsObjectName(function, target.functionName, target.functionNameWide,
                          &target.functionNameIndex) ||
            !IsFunctionOuterName(function, target.stateName, target.stateNameWide,
                                 &target.stateNameIndex) ||
            !HasOuterName(function, target.ownerName, target.ownerNameWide,
                          &target.ownerNameIndex)) {
            continue;
        }

        ScriptArrayHeader script = {};
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                               static_cast<const BYTE*>(function) + 0x54,
                               &script, sizeof(script), &bytesRead) ||
            bytesRead != sizeof(script) || script.data == nullptr ||
            script.count < 3 || script.count > 1024 * 1024 ||
            script.capacity < script.count) {
            Log(L"Fixed-step target %S.%S has an invalid Script array; fix inactive.",
                target.stateName, target.functionName);
            return;
        }

        constexpr BYTE returnVoidAndEnd[] = {0x04, 0x0B, 0x53};
        BYTE* returnCode = script.data + script.count - ArrayCount(returnVoidAndEnd);
        BYTE terminator[ArrayCount(returnVoidAndEnd)] = {};
        if (!ReadProcessMemory(GetCurrentProcess(), returnCode, terminator,
                               sizeof(terminator), &bytesRead) ||
            bytesRead != sizeof(terminator) ||
            memcmp(terminator, returnVoidAndEnd, sizeof(terminator)) != 0) {
            Log(L"Fixed-step target %S.%S lacks the expected void return; fix inactive.",
                target.stateName, target.functionName);
            return;
        }
        if (!HasLeadingDeltaTimeParameter(function)) {
            Log(L"Fixed-step target %S.%S does not take DeltaTime as its first parameter; fix inactive.",
                target.stateName, target.functionName);
            return;
        }

        InterlockedExchangePointer(&target.returnCode, returnCode);
        InterlockedExchangePointer(&target.function, function);
        MarkInteresting(function);
        Log(L"Fixed-step timing fix armed for %S.%S.",
            target.stateName, target.functionName);
        return;
    }
}

// ---------------------------------------------------------------------------
// Per-frame script constants
//
// A few UnrealScript bodies apply a literal once per call and are called once
// per rendered frame. Each literal is an operand in the shared UFunction
// bytecode, so it is rescaled once per rendered frame from the real frame
// time. At 30 FPS every operand keeps its retail value.
// ---------------------------------------------------------------------------

enum class DeltaOperandKind {
    FloatPerFrame,   // float literal added per frame:   value * (dt * 30)
    IntPerFrame,     // int32 literal added per frame:   round(value * dt * 30)
    ByteFrameCount,  // IntConstByte frame threshold:    round(value / (dt * 30))
    ExpEasePerFrame, // per-frame ease x += (1-x)*k:     1 - (1-k)^(dt * 30)
};

struct DeltaOperandTarget {
    const char* functionName;
    const wchar_t* functionNameWide;
    const char* outerName;
    const wchar_t* outerNameWide;
    bool* enabled;
    const PatternByte* pattern;
    SIZE_T patternSize;
    SIZE_T operandOffset;
    DeltaOperandKind kind;
    float originalValue;
    const wchar_t* description;
    volatile LONG functionNameIndex;
    volatile LONG outerNameIndex;
    unsigned expectedMatches;
    BYTE* operands[4];
    volatile LONG armed;
    ScriptBinding binding;
};

// KidPickupFactory_Collectable.UpdateAbsorptionPlayer:
//   m_AbsorpSpeed += 0.5           (B8 01 <prop> 1E 0.5f 16)
constexpr PatternByte kCollectableAbsorbPattern[] = {
    {0xB8, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x1E, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x3F, false},
    {0x16, false},
};

// KidDroppedPickup.UpdateAbsorptionPlayer:
//   ... || mMoveFailCount > 10    (97 01 <prop> 2C 0A 16)
constexpr PatternByte kDroppedPickupFailPattern[] = {
    {0x97, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x2C, false}, {0x0A, false}, {0x16, false},
};

// KidGiantHeadThrowItem.Tick:
//   nowRotation.Pitch += 500      (... 1D F4010000 16)
constexpr PatternByte kThrowItemSpinPattern[] = {
    {0x00, false}, {0x1D, false}, {0xF4, false}, {0x01, false}, {0x00, false},
    {0x00, false}, {0x16, false},
};

// KidGFxMgr.EarthControlTick:
//   m_fEarthRotateAlpha += (1 - m_fEarthRotateAlpha) * 0.1
constexpr PatternByte kEarthEasePattern[] = {
    {0x1E, false}, {0xCD, false}, {0xCC, false}, {0xCC, false}, {0x3D, false},
};

// KidGFx_TitleMenuContent.Tick: four ramps of the form
//   x +/-= 1.0 / float(N)          (AC 1E 1.0f 38 3F 2C N 16), N = 6 or 30
// Rescaling the 1.0 numerator by dt*30 keeps each ramp's retail duration.
constexpr PatternByte kTitleRampPattern[] = {
    {0xAC, false}, {0x1E, false}, {0x00, false}, {0x00, false}, {0x80, false}, {0x3F, false},
    {0x38, false}, {0x3F, false}, {0x2C, false}, {0, true}, {0x16, false},
};

DeltaOperandTarget g_deltaOperandTargets[] = {
    {"UpdateAbsorptionPlayer", L"UpdateAbsorptionPlayer",
     "KidPickupFactory_Collectable", L"KidPickupFactory_Collectable",
     &g_collectableAbsorbFix, kCollectableAbsorbPattern,
     ArrayCount(kCollectableAbsorbPattern), 11, DeltaOperandKind::FloatPerFrame, 0.5f,
     L"collectable absorption acceleration", -1, -1, 1, {}, 0},
    {"UpdateAbsorptionPlayer", L"UpdateAbsorptionPlayer",
     "KidDroppedPickup", L"KidDroppedPickup",
     &g_droppedPickupFix, kDroppedPickupFailPattern,
     ArrayCount(kDroppedPickupFailPattern), 11, DeltaOperandKind::ByteFrameCount, 10.0f,
     L"dropped-pickup blocked-move fallback", -1, -1, 1, {}, 0},
    {"Tick", L"Tick", "KidGiantHeadThrowItem", L"KidGiantHeadThrowItem",
     &g_giantHeadThrowSpinFix, kThrowItemSpinPattern,
     ArrayCount(kThrowItemSpinPattern), 2, DeltaOperandKind::IntPerFrame, 500.0f,
     L"Giant Head thrown-item spin", -1, -1, 1, {}, 0},
    // Smooth-UI only (these bodies then run once per rendered frame):
    {"EarthControlTick", L"EarthControlTick", "KidGFxMgr", L"KidGFxMgr",
     &g_smoothUiExperimental, kEarthEasePattern, ArrayCount(kEarthEasePattern), 1,
     DeltaOperandKind::ExpEasePerFrame, 0.1f, L"title globe rotation ease", -1, -1, 1, {}, 0},
    {"Tick", L"Tick", "KidGFx_TitleMenuContent", L"KidGFx_TitleMenuContent",
     &g_smoothUiExperimental, kTitleRampPattern, ArrayCount(kTitleRampPattern), 2,
     DeltaOperandKind::FloatPerFrame, 1.0f, L"title logo fade and reverse ramps", -1, -1, 4, {}, 0},
};

void WriteDeltaOperand(const DeltaOperandTarget& target, BYTE* operand, float deltaTime) {
    const double frames = static_cast<double>(deltaTime) * 30.0;
    switch (target.kind) {
    case DeltaOperandKind::FloatPerFrame: {
        const float value = static_cast<float>(target.originalValue * frames);
        memcpy(operand, &value, sizeof(value));
        break;
    }
    case DeltaOperandKind::IntPerFrame: {
        const LONG value = static_cast<LONG>(std::lround(target.originalValue * frames));
        memcpy(operand, &value, sizeof(value));
        break;
    }
    case DeltaOperandKind::ByteFrameCount: {
        double count = frames > 1.0e-6 ? target.originalValue / frames : 255.0;
        count = count < 1.0 ? 1.0 : (count > 255.0 ? 255.0 : count);
        *operand = static_cast<BYTE>(std::lround(count));
        break;
    }
    case DeltaOperandKind::ExpEasePerFrame: {
        const float value = static_cast<float>(
            1.0 - std::pow(1.0 - static_cast<double>(target.originalValue), frames));
        memcpy(operand, &value, sizeof(value));
        break;
    }
    }
}

// Called as a target body starts executing (so it is alive) from the
// ProcessInternal hook; never from the frame tick.
void UpdateDeltaOperandsFor(void* function, float deltaTime) {
    for (DeltaOperandTarget& target : g_deltaOperandTargets) {
        if (target.armed == 0 || function != target.binding.function ||
            !ScriptBindingLive(target.binding, function)) {
            continue;
        }
        for (unsigned index = 0; index < target.expectedMatches; ++index) {
            WriteDeltaOperand(target, target.operands[index], deltaTime);
        }
    }
}

bool ReadScriptArray(void* function, ScriptArrayHeader* script, int minimumSize) {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             static_cast<const BYTE*>(function) + kStructScriptOffset,
                             script, sizeof(*script), &bytesRead) &&
           bytesRead == sizeof(*script) && script->data != nullptr &&
           script->count >= minimumSize && script->count <= 1024 * 1024 &&
           script->capacity >= script->count;
}

void TryDiscoverDeltaOperand(void* function) {
    for (DeltaOperandTarget& target : g_deltaOperandTargets) {
        if (!*target.enabled || function == target.binding.function ||
            !IsObjectName(function, target.functionName, target.functionNameWide,
                          &target.functionNameIndex) ||
            !IsFunctionOuterName(function, target.outerName, target.outerNameWide,
                                 &target.outerNameIndex)) {
            continue;
        }

        ScriptArrayHeader script = {};
        if (!ReadScriptArray(function, &script, static_cast<int>(target.patternSize))) {
            Log(L"%S.%S has an invalid Script array; %s fix inactive.",
                target.outerName, target.functionName, target.description);
            return;
        }
        constexpr SIZE_T kMaxOperandMatches = 4;
        BYTE* matches[kMaxOperandMatches] = {};
        unsigned matchCount = 0;
        for (int offset = 0; offset + static_cast<int>(target.patternSize) <= script.count;
             ++offset) {
            if (MatchesPattern(script.data + offset, target.pattern, target.patternSize)) {
                if (matchCount < kMaxOperandMatches) {
                    matches[matchCount] = script.data + offset;
                }
                ++matchCount;
            }
        }
        if (matchCount != target.expectedMatches) {
            Log(L"%S.%S operand signature matched %u locations (expected %u); %s fix inactive.",
                target.outerName, target.functionName, matchCount, target.expectedMatches,
                target.description);
            return;
        }

        InterlockedExchange(&target.armed, 0);
        const SIZE_T operandSize =
            target.kind == DeltaOperandKind::ByteFrameCount ? 1 : sizeof(LONG);
        for (unsigned index = 0; index < matchCount; ++index) {
            BYTE* operand = matches[index] + target.operandOffset;
            if (!MakeWritablePointer(operand, operandSize)) {
                Log(L"%S.%S operand could not be made writable; %s fix inactive.",
                    target.outerName, target.functionName, target.description);
                return;
            }
            target.operands[index] = operand;
        }
        for (unsigned index = 0; index < matchCount; ++index) {
            WriteDeltaOperand(target, target.operands[index], g_movieAdvanceDelta);
        }
        BindScript(&target.binding, function);
        MarkInteresting(function);
        InterlockedExchange(&target.armed, 1);
        Log(L"%S.%S located; real frame time now drives the %s.",
            target.outerName, target.functionName, target.description);
        return;
    }
}

// ---------------------------------------------------------------------------
// Engine frame-rate smoothing
//
// The stock KidEngine.ini enables UE3 frame-rate smoothing with a 31 FPS cap.
// Rather than requiring INI edits, locate the live UEngine through
// UObject::GObjObjects and clear bSmoothFrameRate. Property offsets come from
// the engine's own reflection data rather than being hard-coded.
// ---------------------------------------------------------------------------

struct ObjectArray {
    void** data;
    int count;
    int capacity;
};

template <typename T>
bool SafeRead(const void* address, T* value) {
    __try {
        memcpy(value, address, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
T ReadField(const void* object, ptrdiff_t offset) {
    T value = {};
    if (object != nullptr) {
        SafeRead(static_cast<const BYTE*>(object) + offset, &value);
    }
    return value;
}

bool SafeCopy(void* destination, const void* source, SIZE_T size) {
    __try {
        memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Returns the GNames index of an exact (case-insensitive) ANSI name, or -1.
int FindNameIndex(const char* target) {
    void** names = nullptr;
    int count = 0;
    if (!SafeRead(g_executableBase + kGNamesDataRva, &names) ||
        !SafeRead(g_executableBase + kGNamesCountRva, &count) ||
        names == nullptr || count <= 0 || count > 4 * 1024 * 1024) {
        return -1;
    }
    const SIZE_T length = strlen(target);
    char text[64] = {};
    if (length + 1 >= sizeof(text)) {
        return -1;
    }
    for (int index = 0; index < count; ++index) {
        BYTE* entry = nullptr;
        BYTE flags = 0;
        if (!SafeRead(names + index, &entry) || entry == nullptr ||
            !SafeRead(entry + 8, &flags) || (flags & 1) != 0 ||
            !SafeCopy(text, entry + 0x10, length + 1)) {
            continue;
        }
        if (_stricmp(text, target) == 0) {
            return index;
        }
    }
    return -1;
}

int ObjectNameIndex(void* object) {
    return object == nullptr ? -1 : ReadField<int>(object, kUObjectNameOffset);
}

void* FindChildProperty(void* structure, int nameIndex) {
    void* child = ReadField<void*>(structure, kStructChildrenOffset);
    for (int guard = 0; child != nullptr && guard < 4096; ++guard) {
        if (ObjectNameIndex(child) == nameIndex) {
            return child;
        }
        child = ReadField<void*>(child, kFieldNextOffset);
    }
    return nullptr;
}

struct EngineSmoothing {
    BYTE* flagsAddress = nullptr;
    DWORD smoothMask = 0;
    float* maxSmoothedFrameRate = nullptr;
};

EngineSmoothing g_engineSmoothing;
bool g_engineSearchFinished = false;
unsigned g_engineSearchAttempts = 0;

bool LocateEngineSmoothing() {
    const auto* objects = reinterpret_cast<const ObjectArray*>(g_executableBase + kGObjObjectsRva);
    ObjectArray table = {};
    if (!SafeRead(objects, &table) || table.data == nullptr ||
        table.count < 1000 || table.count > 8 * 1024 * 1024 || table.capacity < table.count) {
        return false;
    }
    // Validate the array by its invariant: GObjObjects(i)->Index == i.
    for (int index = 1; index < 32; ++index) {
        void* object = ReadField<void*>(table.data + index, 0);
        if (object == nullptr || ReadField<int>(object, kUObjectIndexOffset) != index) {
            Log(L"Frame-rate unlock: object table validation failed.");
            g_engineSearchFinished = true;
            return false;
        }
    }

    const int engineName = FindNameIndex("Engine");
    const int className = FindNameIndex("Class");
    const int smoothName = FindNameIndex("bSmoothFrameRate");
    const int maxName = FindNameIndex("MaxSmoothedFrameRate");
    if (engineName < 0 || className < 0 || smoothName < 0 || maxName < 0) {
        return false;
    }

    // Engine.Engine: the class named Engine whose outer package is Engine.
    void* engineClass = nullptr;
    for (int index = 0; index < table.count && engineClass == nullptr; ++index) {
        void* object = ReadField<void*>(table.data + index, 0);
        if (object != nullptr && ObjectNameIndex(object) == engineName &&
            ObjectNameIndex(ReadField<void*>(object, kUObjectClassOffset)) == className &&
            ObjectNameIndex(ReadField<void*>(object, kUObjectOuterOffset)) == engineName) {
            engineClass = object;
        }
    }
    if (engineClass == nullptr) {
        return false;
    }
    void* smoothProperty = FindChildProperty(engineClass, smoothName);
    void* maxProperty = FindChildProperty(engineClass, maxName);
    const int smoothOffset = ReadField<int>(smoothProperty, kPropertyOffsetOffset);
    const DWORD smoothMask = ReadField<DWORD>(smoothProperty, kBoolPropertyMaskOffset);
    const int maxOffset = ReadField<int>(maxProperty, kPropertyOffsetOffset);
    if (smoothProperty == nullptr || maxProperty == nullptr ||
        smoothOffset <= 0 || smoothOffset > 0x4000 || maxOffset <= 0 || maxOffset > 0x4000 ||
        smoothMask == 0 || (smoothMask & (smoothMask - 1)) != 0) {
        Log(L"Frame-rate unlock: Engine reflection data did not validate.");
        g_engineSearchFinished = true;
        return false;
    }

    // The live engine is the non-default instance of a subclass of Engine.
    for (int index = 0; index < table.count; ++index) {
        void* object = ReadField<void*>(table.data + index, 0);
        if (object == nullptr || object == engineClass) {
            continue;
        }
        bool isEngine = false;
        void* cls = ReadField<void*>(object, kUObjectClassOffset);
        for (int depth = 0; cls != nullptr && depth < 8 && !isEngine; ++depth) {
            isEngine = cls == engineClass;
            cls = ReadField<void*>(cls, kStructSuperOffset);
        }
        if (!isEngine) {
            continue;
        }
        wchar_t name[128] = {};
        ReadObjectName(object, name, ArrayCount(name));
        if (wcsncmp(name, L"Default__", 9) == 0) {
            continue;
        }
        g_engineSmoothing.flagsAddress = static_cast<BYTE*>(object) + smoothOffset;
        g_engineSmoothing.smoothMask = smoothMask;
        g_engineSmoothing.maxSmoothedFrameRate =
            reinterpret_cast<float*>(static_cast<BYTE*>(object) + maxOffset);
        const DWORD flags = ReadField<DWORD>(g_engineSmoothing.flagsAddress, 0);
        Log(L"Frame-rate unlock: found %s (bSmoothFrameRate=%d, MaxSmoothedFrameRate=%.1f).",
            name, (flags & smoothMask) != 0 ? 1 : 0,
            ReadField<float>(object, maxOffset));
        return true;
    }
    return false;
}

void ApplyFrameRateUnlock() {
    if (!g_unlockFrameRate) {
        return;
    }
    if (g_engineSmoothing.flagsAddress == nullptr) {
        if (g_engineSearchFinished) {
            return;
        }
        // The engine exists before the first UI frame; retry a bounded number
        // of times only in case this is reached during early startup.
        if (g_engineSearchAttempts++ % 120 != 0) {
            return;
        }
        if (!LocateEngineSmoothing()) {
            if (g_engineSearchAttempts > 120 * 30 && !g_engineSearchFinished) {
                g_engineSearchFinished = true;
                Log(L"Frame-rate unlock: live engine object not found; KidEngine.ini smoothing remains in effect.");
            }
            return;
        }
        g_engineSearchFinished = true;
    }
    DWORD flags = 0;
    if (SafeRead(g_engineSmoothing.flagsAddress, &flags) &&
        (flags & g_engineSmoothing.smoothMask) != 0) {
        flags &= ~g_engineSmoothing.smoothMask;
        memcpy(g_engineSmoothing.flagsAddress, &flags, sizeof(flags));
        Log(L"Frame-rate unlock: engine frame-rate smoothing disabled.");
    }
    // UGameEngine::GetMaxTickRate also clamps 1/DeltaTime to
    // [MinSmoothedFrameRate, MaxSmoothedFrameRate] while smoothing is OFF,
    // and the engine loop sleeps to that rate. A zero maximum makes that
    // clamp return 0, which the engine loop treats as "no limit".
    float maxSmoothed = 0.0f;
    if (g_engineSmoothing.maxSmoothedFrameRate != nullptr &&
        SafeRead(g_engineSmoothing.maxSmoothedFrameRate, &maxSmoothed) && maxSmoothed != 0.0f) {
        const float zero = 0.0f;
        memcpy(g_engineSmoothing.maxSmoothedFrameRate, &zero, sizeof(zero));
        Log(L"Frame-rate unlock: engine tick-rate cap of %.0f FPS (MaxSmoothedFrameRate) removed.",
            maxSmoothed);
    }
}

// ---------------------------------------------------------------------------
// Optional frame limiter (FrameRateLimit). Paced once per rendered frame from
// FGFxEngine::Tick on the game thread with a high-resolution waitable timer
// followed by a short spin.
// ---------------------------------------------------------------------------

HANDLE g_limiterTimer = nullptr;
LONGLONG g_limiterFrequency = 0;
LONGLONG g_limiterNext = 0;

void LimitFrameRate() {
    if (g_frameRateLimit <= 0) {
        return;
    }
    if (g_limiterFrequency == 0) {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        g_limiterFrequency = frequency.QuadPart;
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION requires Windows 10 1803+.
        g_limiterTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        if (g_limiterTimer == nullptr) {
            g_limiterTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        Log(L"Frame limiter active at %d FPS.", g_frameRateLimit);
    }
    const LONGLONG period = g_limiterFrequency / g_frameRateLimit;
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (g_limiterNext == 0 || now.QuadPart - g_limiterNext > period) {
        // First frame, or after a hitch: restart pacing instead of rushing.
        g_limiterNext = now.QuadPart + period;
        return;
    }
    const LONGLONG spinWindow = g_limiterFrequency / 1000;  // 1 ms
    const LONGLONG remaining = g_limiterNext - now.QuadPart;
    if (remaining > spinWindow + spinWindow / 2 && g_limiterTimer != nullptr) {
        LARGE_INTEGER due = {};
        due.QuadPart = -((remaining - spinWindow) * 10000000LL / g_limiterFrequency);
        if (SetWaitableTimer(g_limiterTimer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(g_limiterTimer, INFINITE);
        }
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < g_limiterNext);
    g_limiterNext += period;
}

// Runs from UObject::ProcessInternal, after the VM has copied the caller's
// arguments into Locals. (The FFrame constructor runs before argument copy in
// both CallFunction and ProcessEvent, so Locals are not valid there.)
//
// Retail executes these bodies once per rendered frame at 30 FPS. The gate
// therefore follows the shared real-time 30 Hz generation: the first call in
// each generation runs with the game DeltaTime accumulated since the previous
// executed body, and further calls in the same generation return immediately.
bool ProcessFixedStepFunction(void* object, BYTE* stack) {
    void* function = *reinterpret_cast<void**>(stack + kFFrameNodeOffset);
    const int targetIndex = FindFixedStepTargetForFunction(function);
    if (targetIndex < 0 || object == nullptr || g_scriptTickLimiterInstalled == 0) {
        return false;
    }
    FixedStepTarget& target = g_fixedStepTargets[targetIndex];
    void* verifiedReturnCode = target.returnCode;
    auto* locals = *reinterpret_cast<BYTE**>(stack + kFFrameLocalsOffset);
    if (!ReturnCodeLive(function, verifiedReturnCode) || locals == nullptr) {
        return false;
    }

    float* deltaParameter = reinterpret_cast<float*>(locals);
    const float incomingDelta = *deltaParameter;
    if (!std::isfinite(incomingDelta) || incomingDelta < 0.0f || incomingDelta > 1.0f) {
        if (InterlockedCompareExchange(&target.parameterFailureLogged, 1, 0) == 0) {
            Log(L"Fixed-step target %S.%S received DeltaTime %f; that call was left unmodified.",
                target.stateName, target.functionName, incomingDelta);
        }
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    FixedStepState* state = FindOrCreateFixedStepState(object, targetIndex, now);
    if (state == nullptr) {
        return false;
    }
    if (state->hasRun && now - state->lastCallMs > 250) {
        // The state was inactive; do not hand stale time to its next entry.
        state->hasRun = false;
        state->accumulatedDelta = 0.0;
    }
    state->lastCallMs = now;
    state->accumulatedDelta += static_cast<double>(incomingDelta);

    const LONG generation = g_scriptTickGeneration;
    if (!state->hasRun || state->lastGeneration != generation) {
        state->hasRun = true;
        state->lastGeneration = generation;
        double accumulated = state->accumulatedDelta;
        if (accumulated > static_cast<double>(g_maxDeltaSeconds)) {
            accumulated = static_cast<double>(g_maxDeltaSeconds);
        }
        *deltaParameter = static_cast<float>(accumulated);
        state->accumulatedDelta = 0.0;
        return false;
    }

    if (InterlockedCompareExchange(&target.suppressionLogged, 1, 0) == 0) {
        Log(L"Fixed-step timing fix is suppressing excess %S.%S calls.",
            target.stateName, target.functionName);
    }
    *reinterpret_cast<void**>(stack + kFFrameCodeOffset) = verifiedReturnCode;
    return true;
}

using ProcessInternalFunction = void(__thiscall*)(void* object, void* stack, void* result);
ProcessInternalFunction g_originalProcessInternal = nullptr;

// UObject::ProcessInternal(FFrame& Stack, RESULT_DECL) is the entry of every
// interpreted UnrealScript function body, reached from both CallFunction and
// ProcessEvent after parameters have been copied into Stack.Locals.
// ---------------------------------------------------------------------------
// [Diagnostics] ScriptTrace: logs every execution of named UnrealScript
// functions (comma-separated "Function" or "Outer.Function"), with the object,
// its current state and the first four parameter dwords. Diagnostic only.
// ---------------------------------------------------------------------------

constexpr SIZE_T kMaxTracePatterns = 32;
constexpr SIZE_T kMaxTracedFunctions = 128;
constexpr ptrdiff_t kUObjectStateFrameOffset = 0x14;
constexpr ptrdiff_t kStateFrameStateNodeOffset = 0x28;

struct TracePattern {
    wchar_t outer[64];
    wchar_t function[64];
};

TracePattern g_tracePatterns[kMaxTracePatterns] = {};
SIZE_T g_tracePatternCount = 0;
PVOID volatile g_tracedFunctions[kMaxTracedFunctions] = {};
volatile LONG g_tracedFunctionCount = 0;

void LoadScriptTrace() {
    wchar_t list[2048] = {};
    GetPrivateProfileStringW(L"Diagnostics", L"ScriptTrace", L"", list,
                             static_cast<DWORD>(ArrayCount(list)), g_iniPath);
    wchar_t* context = nullptr;
    for (wchar_t* token = wcstok_s(list, L", ", &context);
         token != nullptr && g_tracePatternCount < kMaxTracePatterns;
         token = wcstok_s(nullptr, L", ", &context)) {
        TracePattern& pattern = g_tracePatterns[g_tracePatternCount++];
        const wchar_t* dot = wcschr(token, L'.');
        if (dot != nullptr) {
            wcsncpy_s(pattern.outer, token, static_cast<size_t>(dot - token));
            wcsncpy_s(pattern.function, dot + 1, _TRUNCATE);
        } else {
            wcsncpy_s(pattern.function, token, _TRUNCATE);
        }
    }
    if (g_tracePatternCount != 0) {
        Log(L"Script trace enabled for %Iu function pattern(s).", g_tracePatternCount);
    }
}

void TryDiscoverTracedFunction(void* function) {
    if (g_tracePatternCount == 0 || function == nullptr) {
        return;
    }
    wchar_t name[64] = {};
    if (!ReadObjectName(function, name, ArrayCount(name))) {
        return;
    }
    for (SIZE_T index = 0; index < g_tracePatternCount; ++index) {
        const TracePattern& pattern = g_tracePatterns[index];
        if (_wcsicmp(name, pattern.function) != 0) {
            continue;
        }
        if (pattern.outer[0] != L'\0') {
            wchar_t outer[64] = {};
            if (!ReadObjectName(ReadObjectPointer(function, kUObjectOuterOffset), outer,
                                ArrayCount(outer)) ||
                _wcsicmp(outer, pattern.outer) != 0) {
                continue;
            }
        }
        const LONG slot = InterlockedIncrement(&g_tracedFunctionCount) - 1;
        if (slot < static_cast<LONG>(kMaxTracedFunctions)) {
            g_tracedFunctions[slot] = function;
            MarkInteresting(function);
        }
        return;
    }
}

void TraceScriptCall(void* object, BYTE* stack) {
    const LONG count = g_tracedFunctionCount;
    if (count == 0) {
        return;
    }
    void* function = *reinterpret_cast<void**>(stack + kFFrameNodeOffset);
    bool traced = false;
    for (LONG index = 0; index < count && index < static_cast<LONG>(kMaxTracedFunctions); ++index) {
        if (g_tracedFunctions[index] == function) {
            traced = true;
            break;
        }
    }
    if (!traced) {
        return;
    }
    wchar_t functionName[64] = L"?";
    wchar_t outerName[64] = L"?";
    wchar_t objectName[96] = L"?";
    wchar_t stateName[64] = L"-";
    ReadObjectName(function, functionName, ArrayCount(functionName));
    ReadObjectName(ReadObjectPointer(function, kUObjectOuterOffset), outerName,
                   ArrayCount(outerName));
    ReadObjectName(object, objectName, ArrayCount(objectName));
    void* stateFrame = ReadObjectPointer(object, kUObjectStateFrameOffset);
    if (stateFrame != nullptr) {
        ReadObjectName(ReadObjectPointer(stateFrame, kStateFrameStateNodeOffset), stateName,
                       ArrayCount(stateName));
    }
    DWORD parameters[4] = {};
    auto* locals = *reinterpret_cast<BYTE**>(stack + kFFrameLocalsOffset);
    if (locals != nullptr) {
        SIZE_T bytesRead = 0;
        ReadProcessMemory(GetCurrentProcess(), locals, parameters, sizeof(parameters), &bytesRead);
    }
    Log(L"Trace gen=%ld %s.%s on %s [state %s] args %08X %08X %08X %08X",
        g_scriptTickGeneration, outerName, functionName, objectName, stateName,
        parameters[0], parameters[1], parameters[2], parameters[3]);
}

// ---------------------------------------------------------------------------
// Cinematic freeze vs. locomotion root motion.
//
// Kismet freezes the player by enabling cinematic mode with movement
// disabled. KidPlayerController then ignores move input and zeroes velocity,
// but in its ordinary PlayerWalking state the mesh drops to the standard
// RMM_Accel root-motion mode once input stops. If a locomotion animation is
// still playing (for example right after the Scarlett conversation), its root
// motion keeps driving the pawn despite the freeze. The Alice key mission
// heart platform exposes this: the lift teleports the player to its centre,
// turning him toward the ledge, and the residual walk carries him off.
//
// Every intended scripted root-motion move (special animations, sync
// interactions, QTEs, Matinee control, attacks) switches the mesh to
// RMM_Translate or runs in another controller state. So the player's extracted
// root-motion translation is zeroed only when all of these hold:
//   - the controller is in cinematic mode with KID's move-input lock set;
//   - the controller is in its ordinary PlayerWalking state;
//   - the mesh is in RMM_Accel.
// The edit goes through the supported RootMotionExtracted(out BoneAtom) event.
// Every offset is resolved from the engine's reflection data.
// ---------------------------------------------------------------------------

constexpr ptrdiff_t kFFrameOutParmsOffset = 0x24;
constexpr ptrdiff_t kBoneAtomTranslationOffset = 0x10;
constexpr BYTE kRootMotionModeAccel = 3;

PVOID volatile g_rootMotionExtractedFunction = nullptr;
volatile LONG g_rootMotionExtractedNameIndex = -1;
volatile LONG g_kidPawnClassNameIndex = -1;
volatile LONG g_cinematicRootMotionLogged = 0;

// ---------------------------------------------------------------------------
// Audio device configuration (PCGamingWiki "Crash after boss fights").
//
// The widely used workaround for the post-boss crash is a set of
// [XAudio2.XAudio2Device] values in KidEngine.ini. Rather than edit the INI,
// the mod applies them to the live device as UXAudio2Device::Init starts,
// before it allocates voices or the resident pool. Each value only ever moves
// in the workaround's direction, so a user's own stronger setting is kept:
//   MaxChannels >= 64, CommonAudioPoolSize 320 if 0, MinCompressedDurationGame <= 0.3
// ---------------------------------------------------------------------------

using AudioDeviceInitFunction = BOOL(__thiscall*)(void* device);
AudioDeviceInitFunction g_originalAudioDeviceInit = nullptr;

void* FindClassProperty(void* object, int nameIndex);

void ApplyAudioDeviceSettings(void* device) {
    const int maxChannelsName = FindNameIndex("MaxChannels");
    const int poolName = FindNameIndex("CommonAudioPoolSize");
    const int durationName = FindNameIndex("MinCompressedDurationGame");
    void* maxChannelsProperty = FindClassProperty(device, maxChannelsName);
    void* poolProperty = FindClassProperty(device, poolName);
    void* durationProperty = FindClassProperty(device, durationName);
    const int maxChannelsOffset = ReadField<int>(maxChannelsProperty, kPropertyOffsetOffset);
    const int poolOffset = ReadField<int>(poolProperty, kPropertyOffsetOffset);
    const int durationOffset = ReadField<int>(durationProperty, kPropertyOffsetOffset);
    auto validOffset = [](int offset) { return offset >= 0x3C && offset < 0x4000; };
    // Init itself reads MaxChannels at +0x40 (see the signature above).
    if (maxChannelsProperty == nullptr || poolProperty == nullptr || durationProperty == nullptr ||
        maxChannelsOffset != 0x40 || !validOffset(poolOffset) || !validOffset(durationOffset)) {
        Log(L"Audio device fix: reflection data did not validate; settings unchanged.");
        return;
    }
    auto* base = static_cast<BYTE*>(device);
    int maxChannels = ReadField<int>(base, maxChannelsOffset);
    int pool = ReadField<int>(base, poolOffset);
    float duration = ReadField<float>(base, durationOffset);
    const int oldChannels = maxChannels;
    const int oldPool = pool;
    const float oldDuration = duration;
    if (maxChannels < 64) {
        maxChannels = 64;
    }
    if (pool == 0) {
        pool = 320;
    }
    if (!(duration <= 0.3f)) {
        duration = 0.3f;
    }
    memcpy(base + maxChannelsOffset, &maxChannels, sizeof(maxChannels));
    memcpy(base + poolOffset, &pool, sizeof(pool));
    memcpy(base + durationOffset, &duration, sizeof(duration));
    Log(L"Audio device fix: MaxChannels %d -> %d, CommonAudioPoolSize %d -> %d, MinCompressedDurationGame %.2f -> %.2f.",
        oldChannels, maxChannels, oldPool, pool, oldDuration, duration);
}

BOOL __fastcall AudioDeviceInit(void* device, void*) {
    if (g_audioDeviceFix && device != nullptr) {
        ApplyAudioDeviceSettings(device);
    }
    return g_originalAudioDeviceInit(device);
}

struct CinematicFreezeLayout {
    bool resolved = false;
    bool valid = false;
    void* playerPawnClass = nullptr;
    int pawnControllerOffset = 0;
    int meshRootMotionModeOffset = 0;
    int cinematicModeOffset = 0;
    DWORD cinematicModeMask = 0;
    int ignoreMoveOffset = 0;
    DWORD ignoreMoveMask = 0;
    int playerWalkingName = -1;
    int rootMotionDeltaName = -1;
};

CinematicFreezeLayout g_freezeLayout;

// Finds a property by name on an object's class chain.
void* FindClassProperty(void* object, int nameIndex) {
    void* cls = ReadField<void*>(object, kUObjectClassOffset);
    for (int depth = 0; cls != nullptr && depth < 16; ++depth) {
        if (void* property = FindChildProperty(cls, nameIndex)) {
            return property;
        }
        cls = ReadField<void*>(cls, kStructSuperOffset);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Field of view.
//
// The retail projection keeps the horizontal FOV fixed
// (AspectRatio_MaintainXFOV), so screens wider than 16:9 lose picture at the
// top and bottom (Vert-). The engine's AspectRatio_MaintainYFOV is not a Hor+
// fix: it reads every FOV as vertical, which widens 16:9 as well. Instead the
// mod widens the projection half angle in tangent space after the engine has
// computed it:
//   hor+ : vertical FOV stays at its 16:9 value on wider screens.
//   FOV  : every gameplay FOV is scaled by tan(FOV/2)/tan(30), so aim, lock-on
//          and battle zooms keep their proportions. Views from a CameraActor
//          (authored cutscene shots) keep their framing.
// Letterboxed cameras use a separate branch and are left alone.
// ---------------------------------------------------------------------------

ptrdiff_t g_aspectConstraintOffset = 0;
BYTE* g_projectionFovReturn = nullptr;

struct ProjectionFovLayout {
    bool resolved = false;
    bool valid = false;
    ptrdiff_t actorOffset = 0;
    ptrdiff_t cameraOffset = 0;
    ptrdiff_t viewTargetOffset = 0;
    int cameraActorName = -1;
    void* lastTarget = nullptr;
    bool lastTargetIsCameraActor = false;
};

ProjectionFovLayout g_projectionFov;

bool ResolveProjectionFovLayout(void* localPlayer) {
    ProjectionFovLayout& layout = g_projectionFov;
    layout.resolved = true;
    void* actorProperty = FindClassProperty(localPlayer, FindNameIndex("Actor"));
    layout.actorOffset = ReadField<int>(actorProperty, kPropertyOffsetOffset);
    void* controller = ReadField<void*>(localPlayer, layout.actorOffset);
    if (actorProperty == nullptr || controller == nullptr) {
        layout.resolved = false;  // No player controller yet; try again later.
        return false;
    }
    void* cameraProperty = FindClassProperty(controller, FindNameIndex("PlayerCamera"));
    layout.cameraOffset = ReadField<int>(cameraProperty, kPropertyOffsetOffset);
    void* camera = ReadField<void*>(controller, layout.cameraOffset);
    if (cameraProperty == nullptr || camera == nullptr) {
        layout.resolved = false;
        return false;
    }
    void* viewTargetProperty = FindClassProperty(camera, FindNameIndex("ViewTarget"));
    layout.viewTargetOffset = ReadField<int>(viewTargetProperty, kPropertyOffsetOffset);
    layout.cameraActorName = FindNameIndex("CameraActor");
    layout.valid = viewTargetProperty != nullptr && layout.viewTargetOffset > 0 &&
                   layout.cameraActorName >= 0;
    Log(L"FOV: view target layout %s (Actor 0x%X, PlayerCamera 0x%X, ViewTarget 0x%X).",
        layout.valid ? L"resolved" : L"did not validate; FOV applies to every view",
        static_cast<unsigned>(layout.actorOffset), static_cast<unsigned>(layout.cameraOffset),
        static_cast<unsigned>(layout.viewTargetOffset));
    return layout.valid;
}

// True when the player is looking through an authored CameraActor.
bool ViewingThroughCameraActor(void* localPlayer) {
    ProjectionFovLayout& layout = g_projectionFov;
    if (!layout.resolved) {
        ResolveProjectionFovLayout(localPlayer);
    }
    if (!layout.valid) {
        return false;
    }
    void* controller = ReadField<void*>(localPlayer, layout.actorOffset);
    void* camera = ReadField<void*>(controller, layout.cameraOffset);
    void* target = ReadField<void*>(camera, layout.viewTargetOffset);
    if (target == nullptr) {
        return false;
    }
    if (target != layout.lastTarget) {
        layout.lastTarget = target;
        layout.lastTargetIsCameraActor = false;
        void* cls = ReadField<void*>(target, kUObjectClassOffset);
        for (int depth = 0; cls != nullptr && depth < 32; ++depth) {
            if (ObjectNameIndex(cls) == layout.cameraActorName) {
                layout.lastTargetIsCameraActor = true;
                break;
            }
            cls = ReadField<void*>(cls, kStructSuperOffset);
        }
    }
    return layout.lastTargetIsCameraActor;
}

// frame is the stack pointer at the hooked instruction.
extern "C" void __cdecl AdjustProjectionFov(BYTE* frame, void* localPlayer) {
    if (localPlayer == nullptr ||
        ReadField<BYTE>(localPlayer, g_aspectConstraintOffset) != 1) {
        return;  // Not AspectRatio_MaintainXFOV: the user's KidEngine.ini setting wins.
    }
    float* halfFov = reinterpret_cast<float*>(frame + 0x24);
    const int sizeX = *reinterpret_cast<int*>(frame + 0x44);
    const int sizeY = *reinterpret_cast<int*>(frame + 0x40);
    float scale = 1.0f;
    if (g_fovHorPlus && sizeX > 0 && sizeY > 0) {
        const float widthRatio = (static_cast<float>(sizeX) / static_cast<float>(sizeY)) * (9.0f / 16.0f);
        if (widthRatio > 1.0f) {
            scale *= widthRatio;
        }
    }
    if (g_fovScale != 1.0f && !ViewingThroughCameraActor(localPlayer)) {
        scale *= g_fovScale;
    }
    if (scale == 1.0f || !(*halfFov > 0.0f && *halfFov < 1.57f)) {
        return;
    }
    float widened = atanf(tanf(*halfFov) * scale);
    const float maxHalf = 85.0f * 3.14159265f / 180.0f;
    if (widened > maxHalf) {
        widened = maxHalf;
    }
    *halfFov = widened;
}

__declspec(naked) void ProjectionFovStub() {
    __asm {
        movss dword ptr [esp+24h], xmm0   ; the instruction the jump replaced
        pushad
        pushfd
        sub esp, 80h
        movups xmmword ptr [esp], xmm0
        movups xmmword ptr [esp+10h], xmm1
        movups xmmword ptr [esp+20h], xmm2
        movups xmmword ptr [esp+30h], xmm3
        movups xmmword ptr [esp+40h], xmm4
        movups xmmword ptr [esp+50h], xmm5
        movups xmmword ptr [esp+60h], xmm6
        movups xmmword ptr [esp+70h], xmm7
        lea eax, [esp+0A4h]               ; 80h + pushfd + pushad
        push esi                          ; ULocalPlayer
        push eax
        call AdjustProjectionFov
        add esp, 8
        movups xmm0, xmmword ptr [esp]
        movups xmm1, xmmword ptr [esp+10h]
        movups xmm2, xmmword ptr [esp+20h]
        movups xmm3, xmmword ptr [esp+30h]
        movups xmm4, xmmword ptr [esp+40h]
        movups xmm5, xmmword ptr [esp+50h]
        movups xmm6, xmmword ptr [esp+60h]
        movups xmm7, xmmword ptr [esp+70h]
        add esp, 80h
        popfd
        popad
        jmp dword ptr [g_projectionFovReturn]
    }
}

// ---------------------------------------------------------------------------
// Supersampling.
//
// With Supersampling=N the scene is rendered at N times the viewport size per
// axis and scaled down by the engine's own screen-percentage pass, so the HUD,
// menus and Scaleform movies stay at native resolution. The rectangle is
// scaled from the buffer origin rather than centred, and the buffers are
// allocated at the scaled size. ScreenPercentage in KidEngine.ini is ignored
// while this is on.
// ---------------------------------------------------------------------------

using NeedsUpscaleFunction = BOOL(__thiscall*)(void* settings);
using ScaleScreenCoordsFunction = void(__thiscall*)(void* settings, int* x, int* y,
                                                    unsigned* sizeX, unsigned* sizeY);
using UnScaleScreenCoordsFunction = void(__thiscall*)(void* settings, int* x, int* y,
                                                      unsigned* sizeX, unsigned* sizeY,
                                                      float scaledX, float scaledY,
                                                      float scaledSizeX, float scaledSizeY);
using SceneTargetsAllocateFunction = void(__thiscall*)(void* targets, unsigned minSizeX,
                                                       unsigned minSizeY);
NeedsUpscaleFunction g_originalNeedsUpscale = nullptr;
ScaleScreenCoordsFunction g_originalScaleScreenCoords = nullptr;
UnScaleScreenCoordsFunction g_originalUnScaleScreenCoords = nullptr;
SceneTargetsAllocateFunction g_originalSceneTargetsAllocate = nullptr;
const BYTE* g_finalPassUnScaleReturn = nullptr;
// Largest scene buffer edge the mod will request (the D3D9 texture limit on
// current GPUs is 16384).
constexpr unsigned kMaxSceneBufferEdge = 16384;

unsigned SupersampledSize(unsigned size) {
    const double scaled = static_cast<double>(size) * g_supersampling;
    return scaled >= kMaxSceneBufferEdge ? kMaxSceneBufferEdge : static_cast<unsigned>(scaled);
}

BOOL __fastcall NeedsUpscale(void* settings, void*) {
    return g_supersampling > 1.0f ? TRUE : g_originalNeedsUpscale(settings);
}

void __fastcall ScaleScreenCoords(void* settings, void*, int* x, int* y,
                                  unsigned* sizeX, unsigned* sizeY) {
    if (g_supersampling <= 1.0f) {
        g_originalScaleScreenCoords(settings, x, y, sizeX, sizeY);
        return;
    }

    const unsigned viewX = *sizeX;
    const unsigned viewY = *sizeY;
    *x = static_cast<int>(*x * g_supersampling);
    *y = static_cast<int>(*y * g_supersampling);
    *sizeX = SupersampledSize(*sizeX);
    *sizeY = SupersampledSize(*sizeY);
    // Report the render size once per view size (startup and window resizes).
    static unsigned loggedX = 0;
    static unsigned loggedY = 0;
    if (viewX != loggedX || viewY != loggedY) {
        loggedX = viewX;
        loggedY = viewY;
        Log(L"Supersampling: the %ux%u view renders at %ux%u.", viewX, viewY, *sizeX, *sizeY);
    }
}

void __fastcall UnScaleScreenCoords(void* settings, void*, int* x, int* y,
                                    unsigned* sizeX, unsigned* sizeY,
                                    float scaledX, float scaledY,
                                    float scaledSizeX, float scaledSizeY) {
    if (g_supersampling <= 1.0f) {
        g_originalUnScaleScreenCoords(settings, x, y, sizeX, sizeY,
                                      scaledX, scaledY, scaledSizeX, scaledSizeY);
        return;
    }
    const float scale = _ReturnAddress() == g_finalPassUnScaleReturn ? g_supersampling : 1.0f;
    *x = static_cast<int>(scaledX / scale);
    *y = static_cast<int>(scaledY / scale);
    *sizeX = static_cast<unsigned>(scaledSizeX / scale + 0.5f);
    *sizeY = static_cast<unsigned>(scaledSizeY / scale + 0.5f);
}

void __fastcall SceneTargetsAllocate(void* targets, void*, unsigned minSizeX, unsigned minSizeY) {
    if (g_supersampling > 1.0f) {
        minSizeX = SupersampledSize(minSizeX);
        minSizeY = SupersampledSize(minSizeY);
    }
    g_originalSceneTargetsAllocate(targets, minSizeX, minSizeY);
}

// Replaces the scene-colour clear's two size queries (see
// kSceneColorViewportPattern): esi = SizeX * N, eax = SizeY * N.
BYTE* g_sceneColorViewportReturn = nullptr;
__declspec(naked) void SceneColorViewportStub() {
    __asm {
        mov ecx, [ebx+10h]
        mov edx, [ecx]
        call dword ptr [edx+8]
        cvtsi2ss xmm0, eax
        mulss xmm0, dword ptr [g_supersampling]
        cvttss2si esi, xmm0
        mov ecx, [ebx+10h]
        mov edx, [ecx]
        call dword ptr [edx+0Ch]
        cvtsi2ss xmm0, eax
        mulss xmm0, dword ptr [g_supersampling]
        cvttss2si eax, xmm0
        jmp dword ptr [g_sceneColorViewportReturn]
    }
}

bool InstallSceneColorViewportStub(BYTE* site) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kSceneColorViewportSiteLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    g_sceneColorViewportReturn = site + kSceneColorViewportSiteLength;
    WriteRelativeJump(site, reinterpret_cast<const void*>(&SceneColorViewportStub));
    for (SIZE_T index = 5; index < kSceneColorViewportSiteLength; ++index) {
        site[index] = 0x90;
    }
    FlushInstructionCache(GetCurrentProcess(), site, kSceneColorViewportSiteLength);
    DWORD ignored = 0;
    VirtualProtect(site, kSceneColorViewportSiteLength, oldProtect, &ignored);
    return true;
}

// Replaces a two-byte conditional jump with nops.
bool RemoveShortJump(BYTE* jump, const wchar_t* label) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(jump, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log(L"VirtualProtect for %s failed (error %lu).", label, GetLastError());
        return false;
    }
    jump[0] = 0x90;
    jump[1] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), jump, 2);
    DWORD ignored = 0;
    VirtualProtect(jump, 2, oldProtect, &ignored);
    return true;
}

bool InstallProjectionFovHook(BYTE* site) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    g_projectionFovReturn = site + 6;
    WriteRelativeJump(site, reinterpret_cast<const void*>(&ProjectionFovStub));
    site[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    DWORD ignored = 0;
    VirtualProtect(site, 6, oldProtect, &ignored);
    return true;
}

bool ResolveFreezeLayout(void* pawn, void* mesh) {
    CinematicFreezeLayout& layout = g_freezeLayout;
    layout.resolved = true;
    const int controllerName = FindNameIndex("Controller");
    const int rootMotionName = FindNameIndex("RootMotionMode");
    const int cinematicName = FindNameIndex("bCinematicMode");
    const int ignoreMoveName = FindNameIndex("m_bCinematicIgnoreMoveInput");
    layout.playerWalkingName = FindNameIndex("PlayerWalking");
    layout.rootMotionDeltaName = FindNameIndex("ExtractedRootMotionDelta");
    void* controllerProperty = FindClassProperty(pawn, controllerName);
    void* rootMotionProperty = FindClassProperty(mesh, rootMotionName);
    layout.pawnControllerOffset = ReadField<int>(controllerProperty, kPropertyOffsetOffset);
    layout.meshRootMotionModeOffset = ReadField<int>(rootMotionProperty, kPropertyOffsetOffset);
    void* controller = ReadField<void*>(pawn, layout.pawnControllerOffset);
    if (controller == nullptr && controllerProperty != nullptr) {
        // Not possessed yet; resolve again on a later call.
        layout.resolved = false;
        return false;
    }
    void* cinematicProperty = FindClassProperty(controller, cinematicName);
    void* ignoreMoveProperty = FindClassProperty(controller, ignoreMoveName);
    layout.cinematicModeOffset = ReadField<int>(cinematicProperty, kPropertyOffsetOffset);
    layout.cinematicModeMask = ReadField<DWORD>(cinematicProperty, kBoolPropertyMaskOffset);
    layout.ignoreMoveOffset = ReadField<int>(ignoreMoveProperty, kPropertyOffsetOffset);
    layout.ignoreMoveMask = ReadField<DWORD>(ignoreMoveProperty, kBoolPropertyMaskOffset);
    auto validOffset = [](int offset) { return offset > 0 && offset < 0x4000; };
    auto validMask = [](DWORD mask) { return mask != 0 && (mask & (mask - 1)) == 0; };
    layout.valid = controller != nullptr && layout.playerWalkingName >= 0 &&
                   layout.rootMotionDeltaName >= 0 &&
                   validOffset(layout.pawnControllerOffset) &&
                   validOffset(layout.meshRootMotionModeOffset) &&
                   validOffset(layout.cinematicModeOffset) && validMask(layout.cinematicModeMask) &&
                   validOffset(layout.ignoreMoveOffset) && validMask(layout.ignoreMoveMask);
    if (layout.valid) {
        layout.playerPawnClass = ReadField<void*>(pawn, kUObjectClassOffset);
        Log(L"Cinematic root-motion fix armed (Controller@0x%X, RootMotionMode@0x%X, bCinematicMode@0x%X/%X, ignore-move@0x%X/%X).",
            layout.pawnControllerOffset, layout.meshRootMotionModeOffset,
            layout.cinematicModeOffset, layout.cinematicModeMask,
            layout.ignoreMoveOffset, layout.ignoreMoveMask);
    } else {
        Log(L"Cinematic root-motion fix: reflection data did not validate; fix inactive.");
    }
    return layout.valid;
}

void TryDiscoverRootMotionFunction(void* function) {
    if (!g_cinematicRootMotionFix || function == nullptr ||
        g_rootMotionExtractedFunction != nullptr) {
        return;
    }
    if (IsObjectName(function, "RootMotionExtracted", L"RootMotionExtracted",
                     &g_rootMotionExtractedNameIndex) &&
        IsFunctionOuterName(function, "KidPawn", L"KidPawn", &g_kidPawnClassNameIndex)) {
        InterlockedExchangePointer(&g_rootMotionExtractedFunction, function);
        MarkInteresting(function);
    }
}

void SuppressFrozenRootMotion(void* pawn, BYTE* stack) {
    auto* locals = *reinterpret_cast<BYTE**>(stack + kFFrameLocalsOffset);
    void* mesh = locals != nullptr ? ReadField<void*>(locals, 0) : nullptr;  // SkelComp
    if (pawn == nullptr || mesh == nullptr) {
        return;
    }
    CinematicFreezeLayout& layout = g_freezeLayout;
    if (!layout.resolved) {
        // Resolve against the player's pawn only.
        wchar_t className[64] = {};
        if (!ReadObjectName(ReadField<void*>(pawn, kUObjectClassOffset), className,
                            ArrayCount(className)) ||
            wcscmp(className, L"KidPawn_Mondo") != 0) {
            return;
        }
        ResolveFreezeLayout(pawn, mesh);
    }
    if (!layout.valid || ReadField<void*>(pawn, kUObjectClassOffset) != layout.playerPawnClass ||
        ReadField<BYTE>(mesh, layout.meshRootMotionModeOffset) != kRootMotionModeAccel) {
        return;
    }
    void* controller = ReadField<void*>(pawn, layout.pawnControllerOffset);
    if (controller == nullptr ||
        (ReadField<DWORD>(controller, layout.cinematicModeOffset) & layout.cinematicModeMask) == 0 ||
        (ReadField<DWORD>(controller, layout.ignoreMoveOffset) & layout.ignoreMoveMask) == 0) {
        return;
    }
    void* stateFrame = ReadField<void*>(controller, kUObjectStateFrameOffset);
    void* stateNode = ReadField<void*>(stateFrame, kStateFrameStateNodeOffset);
    if (ObjectNameIndex(stateNode) != layout.playerWalkingName) {
        return;
    }

    // The BoneAtom out parameter is the native caller's memory, reached via
    // FFrame::OutParms {Property, PropAddr, Next}.
    BYTE* record = ReadField<BYTE*>(stack, kFFrameOutParmsOffset);
    const int deltaName = layout.rootMotionDeltaName;
    for (int guard = 0; record != nullptr && guard < 8; ++guard) {
        void* property = ReadField<void*>(record, 0);
        BYTE* address = ReadField<BYTE*>(record, 4);
        if (address != nullptr && ObjectNameIndex(property) == deltaName) {
            float translation[3] = {};
            SafeRead(address + kBoneAtomTranslationOffset, &translation);
            if (translation[0] != 0.0f || translation[1] != 0.0f || translation[2] != 0.0f) {
                const float zero[3] = {};
                SafeCopy(address + kBoneAtomTranslationOffset, zero, sizeof(zero));
                if (InterlockedExchange(&g_cinematicRootMotionLogged, 1) == 0) {
                    Log(L"Cinematic root-motion fix: held residual locomotion root motion while the player is frozen.");
                }
            }
            return;
        }
        record = ReadField<BYTE*>(record, 8);
    }
}

// ---------------------------------------------------------------------------
// Smooth UI: frame-counter statements in render-rate bodies.
//
// KidGFx_TitleMenuContent.Tick runs every rendered frame in smooth-UI mode so
// its logo fade and reverse ramps (rescaled above) animate smoothly, but its
// state machine also waits on integer frame counters. Each `counter++` /
// `counter--` statement (A5/A6 01 <property> 16, 11 bytes) is turned into an
// EX_Jump over itself on calls that are not the first in a 30 Hz generation,
// and restored on the first call. The counters therefore advance at exactly
// the retail rate while the rest of the body runs at render rate.
// ---------------------------------------------------------------------------

constexpr SIZE_T kCounterStatementSize = 11;
constexpr SIZE_T kMaxCounterStatements = 8;
constexpr BYTE kExJump = 0x06;

struct CounterGateTarget {
    const char* functionName;
    const wchar_t* functionNameWide;
    const char* outerName;
    const wchar_t* outerNameWide;
    unsigned expectedStatements;
    volatile LONG functionNameIndex;
    volatile LONG outerNameIndex;
    PVOID volatile function;
    BYTE* statements[kMaxCounterStatements];
    BYTE original[kMaxCounterStatements][3];
    BYTE jump[kMaxCounterStatements][3];
    unsigned statementCount;
    void* lastObject;
    LONG lastGeneration;
    bool jumpsWritten;
    ScriptBinding binding;
};

CounterGateTarget g_counterGateTargets[] = {
    {"Tick", L"Tick", "KidGFx_TitleMenuContent", L"KidGFx_TitleMenuContent", 6,
     -1, -1, nullptr, {}, {}, {}, 0, nullptr, LONG_MIN, false},
    // KidGFxMgr.Tick itself: m_iFadeInDelayTime-- (the frames the screen stays
    // black before a fade-in starts) and four m_iTutorialWindowCounter++ waits.
    {"Tick", L"Tick", "KidGFxMgr", L"KidGFxMgr", 5,
     -1, -1, nullptr, {}, {}, {}, 0, nullptr, LONG_MIN, false},
};

constexpr PatternByte kIntIncrementStatement[] = {
    {0xA5, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x16, false},
};
constexpr PatternByte kIntDecrementStatement[] = {
    {0xA6, false}, {0x01, false},
    {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true}, {0, true},
    {0x16, false},
};

void TryDiscoverCounterGate(void* function) {
    if (!g_smoothUiExperimental || function == nullptr) {
        return;
    }
    for (CounterGateTarget& target : g_counterGateTargets) {
        if (target.function == function ||
            !IsObjectName(function, target.functionName, target.functionNameWide,
                          &target.functionNameIndex) ||
            !IsFunctionOuterName(function, target.outerName, target.outerNameWide,
                                 &target.outerNameIndex)) {
            continue;
        }
        ScriptArrayHeader script = {};
        if (!ReadScriptArray(function, &script, 64) || script.count > 0xFFFF) {
            Log(L"Smooth UI: %S.%S Script array was invalid; it stays on the 30 Hz gate.",
                target.outerName, target.functionName);
            return;
        }
        BYTE* found[kMaxCounterStatements] = {};
        unsigned count = 0;
        for (int offset = 0; offset + static_cast<int>(kCounterStatementSize) <= script.count;
             ++offset) {
            BYTE* candidate = script.data + offset;
            if (MatchesPattern(candidate, kIntIncrementStatement, kCounterStatementSize) ||
                MatchesPattern(candidate, kIntDecrementStatement, kCounterStatementSize)) {
                if (count < kMaxCounterStatements) {
                    found[count] = candidate;
                }
                ++count;
            }
        }
        if (count != target.expectedStatements) {
            Log(L"Smooth UI: %S.%S has %u frame-counter statements (expected %u); counter gate inactive.",
                target.outerName, target.functionName, count, target.expectedStatements);
            return;
        }
        InterlockedExchangePointer(&target.function, nullptr);
        for (unsigned index = 0; index < count; ++index) {
            if (!MakeWritablePointer(found[index], 3)) {
                return;
            }
            const unsigned resume =
                static_cast<unsigned>(found[index] - script.data) + kCounterStatementSize;
            target.statements[index] = found[index];
            memcpy(target.original[index], found[index], 3);
            target.jump[index][0] = kExJump;
            target.jump[index][1] = static_cast<BYTE>(resume & 0xFF);
            target.jump[index][2] = static_cast<BYTE>(resume >> 8);
        }
        target.statementCount = count;
        target.lastObject = nullptr;
        target.lastGeneration = LONG_MIN;
        target.jumpsWritten = false;
        BindScript(&target.binding, function);
        InterlockedExchangePointer(&target.function, function);
        MarkInteresting(function);
        Log(L"Smooth UI: %S.%S runs at render rate; its %u frame counters keep the 30 Hz clock.",
            target.outerName, target.functionName, count);
        return;
    }
}

void ApplyCounterGate(void* object, BYTE* stack) {
    void* function = *reinterpret_cast<void**>(stack + kFFrameNodeOffset);
    for (CounterGateTarget& target : g_counterGateTargets) {
        if (function != target.function || function == nullptr) {
            continue;
        }
        if (!ScriptBindingLive(target.binding, function)) {
            return;
        }
        const LONG generation = g_scriptTickGeneration;
        const bool step = object != target.lastObject || target.lastGeneration != generation;
        target.lastObject = object;
        target.lastGeneration = generation;
        if (step == !target.jumpsWritten) {
            return;  // Bytecode already in the required form.
        }
        for (unsigned index = 0; index < target.statementCount; ++index) {
            memcpy(target.statements[index], step ? target.original[index] : target.jump[index], 3);
        }
        target.jumpsWritten = !step;
        return;
    }
}

// Rewrites a function's frame-time constants as its body starts executing.
// The function is alive while it runs, and its bytecode is confirmed to be the
// array recorded at discovery, so no write can reach freed memory.
void ApplyLiveScriptConstants(void* function, BYTE* stack) {
    if (function == g_cardiacBinding.function && ScriptBindingLive(g_cardiacBinding, function)) {
        UpdateCardiacDeltaConstants(g_movieAdvanceDelta);
    }
    if (function == g_uiFadeBinding.function && ScriptBindingLive(g_uiFadeBinding, function)) {
        // KidGFxMgr.Tick(float DeltaTime): use the delta this call receives.
        float delta = 0.0f;
        auto* locals = *reinterpret_cast<BYTE**>(stack + kFFrameLocalsOffset);
        if (locals == nullptr || !SafeRead(locals, &delta) || !std::isfinite(delta) ||
            delta <= 0.0f || delta > 1.0f) {
            delta = g_movieAdvanceDelta;
        }
        UpdateUiFadeDeltaConstants(delta);
    }
    UpdateDeltaOperandsFor(function, g_movieAdvanceDelta);
}

void __fastcall ScriptProcessInternal(void* object, void*, BYTE* stack, void* result) {
    if (stack == nullptr ||
        !IsInteresting(*reinterpret_cast<void**>(stack + kFFrameNodeOffset))) {
        g_originalProcessInternal(object, stack, result);
        return;
    }
    ApplyCounterGate(object, stack);
    if (stack != nullptr) {
        TraceScriptCall(object, stack);
        void* function = *reinterpret_cast<void**>(stack + kFFrameNodeOffset);
        if (function != nullptr) {
            if (function == g_rootMotionExtractedFunction) {
                SuppressFrozenRootMotion(object, stack);
            }
            ApplyLiveScriptConstants(function, stack);
        }
        ProcessFixedStepFunction(object, stack);
    }
    g_originalProcessInternal(object, stack, result);
}

void TryActivateSebastianStunFallFix(void* object, void* function) {
    if (!g_sebastianStunFallFix || object == nullptr || function == nullptr ||
        function == g_sebastianBinding.function) {
        return;
    }

    void* objectClass = ReadObjectPointer(object, kUObjectClassOffset);
    if (!IsObjectName(function, "Tick", L"Tick", &g_sebastianTickNameIndex) ||
        !IsObjectName(objectClass, "KidAIController_Sebastian",
                      L"KidAIController_Sebastian",
                      &g_sebastianControllerClassNameIndex) ||
        !IsFunctionOuterName(function, "KidAIController_Sebastian",
                             L"KidAIController_Sebastian",
                             &g_sebastianControllerClassNameIndex)) {
        return;
    }

    ScriptArrayHeader script = {};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           static_cast<BYTE*>(function) + 0x54,
                           &script, sizeof(script), &bytesRead) ||
        bytesRead != sizeof(script) || script.data == nullptr ||
        script.count < 500 || script.count > 4096 ||
        script.capacity < script.count) {
        Log(L"Sebastian vertical-motion target was named, but its Script array was not valid.");
        return;
    }

    BYTE snapshot[4096] = {};
    if (!ReadProcessMemory(GetCurrentProcess(), script.data, snapshot,
                           static_cast<SIZE_T>(script.count), &bytesRead) ||
        bytesRead != static_cast<SIZE_T>(script.count)) {
        Log(L"Sebastian vertical-motion target bytecode could not be read.");
        return;
    }

    // Tick contains two NameConst operands for AbsorbState and one for
    // StunState. The second AbsorbState guards the `Velocity.Z = 0` height
    // clamp which otherwise erases falling velocity on every high-FPS
    // StunState tick.
    SIZE_T absorbStateCount = 0;
    SIZE_T absorbStateOffset = 0;
    SIZE_T stunStateCount = 0;
    ULONGLONG stunStateValue = 0;
    for (SIZE_T offset = 0; offset + 1 < static_cast<SIZE_T>(script.count); ++offset) {
        if (snapshot[offset] == 0x21 && offset + 9 <= static_cast<SIZE_T>(script.count)) {
            int nameIndex = -1;
            memcpy(&nameIndex, snapshot + offset + 1, sizeof(nameIndex));
            wchar_t name[64] = {};
            if (!ReadNameByIndex(nameIndex, name, ArrayCount(name))) {
                continue;
            }
            if (wcscmp(name, L"AbsorbState") == 0) {
                ++absorbStateCount;
                absorbStateOffset = offset;
                InterlockedCompareExchange(&g_sebastianAbsorbStateNameIndex,
                                           nameIndex, -1);
            } else if (wcscmp(name, L"StunState") == 0) {
                ++stunStateCount;
                memcpy(&stunStateValue, snapshot + offset + 1,
                       sizeof(stunStateValue));
                InterlockedCompareExchange(&g_sebastianStunStateNameIndex,
                                           nameIndex, -1);
            }
        }
    }
    if (absorbStateCount != 2 || stunStateCount != 1) {
        Log(L"Sebastian stun-fall bytecode validation failed: AbsorbState=%Iu, StunState=%Iu.",
            absorbStateCount, stunStateCount);
        return;
    }

    BYTE* clampStateOperand = script.data + absorbStateOffset + 1;
    if (!MakeWritablePointer(clampStateOperand, sizeof(ULONGLONG))) {
        Log(L"Sebastian stun-fall operand could not be made writable.");
        return;
    }
    memcpy(&g_sebastianAbsorbStateNameValue, snapshot + absorbStateOffset + 1,
           sizeof(g_sebastianAbsorbStateNameValue));
    g_sebastianStunStateNameValue = stunStateValue;
    InterlockedExchangePointer(&g_sebastianVelocityClampStateOperand,
                               clampStateOperand);
    InterlockedExchangePointer(&g_sebastianVerticalTickFunction, function);
    BindScript(&g_sebastianBinding, function);
    MarkInteresting(function);
    Log(L"Sebastian stun-fall fix activated: the flying-height velocity clamp is bypassed only while StunState owns the shared controller tick.");
}

// The shared controller Tick is called from StunState.Tick via global.Tick.
// Recognise StunState.Tick itself so the clamp bypass knows its caller.
PVOID volatile g_sebastianStunTickFunction = nullptr;
volatile LONG g_sebastianStunTickOuterIndex = -1;

void TryDiscoverSebastianStunTick(void* function) {
    if (!g_sebastianStunFallFix || function == nullptr ||
        function == g_sebastianStunTickFunction ||
        !IsObjectName(function, "Tick", L"Tick", &g_sebastianTickNameIndex) ||
        !IsFunctionOuterName(function, "StunState", L"StunState",
                             &g_sebastianStunStateNameIndex) ||
        !HasOuterName(function, "KidAIController_Sebastian", L"KidAIController_Sebastian",
                      &g_sebastianStunTickOuterIndex)) {
        return;
    }
    InterlockedExchangePointer(&g_sebastianStunTickFunction, function);
    MarkInteresting(function);
}

void UpdateSebastianStunFallFix(void* object, void* function) {
    void* targetFunction = g_sebastianVerticalTickFunction;
    void* clampStateOperand = g_sebastianVelocityClampStateOperand;
    if (!g_sebastianStunFallFix || function != targetFunction ||
        clampStateOperand == nullptr || !ScriptBindingLive(g_sebastianBinding, function)) {
        return;
    }

    const bool calledFromStunState =
        object == g_sebastianExpectedStunGlobalTick;
    g_sebastianExpectedStunGlobalTick = nullptr;
    const ULONGLONG clampStateName =
        calledFromStunState
            ? g_sebastianStunStateNameValue
            : g_sebastianAbsorbStateNameValue;
    memcpy(clampStateOperand, &clampStateName, sizeof(clampStateName));
}

void ObserveBossFinisherTiming(void* object, void* function, void* locals) {
    if (!g_bossFinisherDiagnostics || object == nullptr || function == nullptr) {
        return;
    }
    (void)locals;

    const ULONGLONG now = GetTickCount64();
    void* objectClass = ReadObjectPointer(object, kUObjectClassOffset);

    // These two action boundaries surround the actual third-round critical-hit
    // Matinee. The cooked ToggleCinematicMode_5 (runtime FName suffix _4)
    // starts SeqAct_Interp_4 (length 5.333333);
    // only that Interp's Completed output can activate the loading-tip action.
    if (IsObjectName(function, "Activated", L"Activated",
                     &g_bossFinisherActivatedNameIndex)) {
        wchar_t objectName[128] = L"<unreadable>";
        ReadObjectName(object, objectName, ArrayCount(objectName));
        if (IsObjectName(objectClass, "KidSeqAct_ToggleCinematicMode",
                         L"KidSeqAct_ToggleCinematicMode",
                         &g_bossFinisherToggleCinematicClassNameIndex) &&
            wcscmp(objectName, L"KidSeqAct_ToggleCinematicMode_4") == 0 &&
            HasOuterName(object, "CINE_S03_SebBattle_Matinee",
                         L"CINE_S03_SebBattle_Matinee",
                         &g_bossFinisherCinePackageNameIndex)) {
            g_sebastianThirdCutStartMs = now;
            g_sebastianMatineeBoundarySeen = true;
            g_bossFinisherTraceUntilMs = now + 15000;
            Log(L"Sebastian critical-cut trace: third-round Matinee boundary entered (expected duration 5.333333 s).");
            return;
        }
        if (now <= g_bossFinisherTraceUntilMs &&
            IsObjectName(objectClass, "KidSeqAct_LoadingTips", L"KidSeqAct_LoadingTips",
                         &g_bossFinisherLoadingTipsClassNameIndex)) {
            const bool inCastle = HasOuterName(
                object, "PL_S01_Castle", L"PL_S01_Castle",
                &g_bossFinisherCastlePackageNameIndex);
            const ULONGLONG elapsed = g_sebastianThirdCutStartMs == 0
                ? 0 : now - g_sebastianThirdCutStartMs;
            Log(L"Sebastian critical-cut trace: loading candidate %s entered after %llu ms (PL_S01_Castle=%d, baseline=%s).",
                objectName, elapsed, inCastle ? 1 : 0,
                g_sebastianMatineeBoundarySeen ? L"Matinee" : L"critical hit");
            return;
        }
    }

    const bool sebastianController = IsObjectName(
        objectClass, "KidAIController_Sebastian", L"KidAIController_Sebastian",
        &g_sebastianControllerClassNameIndex);
    if (sebastianController &&
        IsFunctionOuterName(function, "StunState", L"StunState",
                            &g_sebastianStunStateNameIndex)) {
        if (IsObjectName(function, "BeginState", L"BeginState",
                         &g_sebastianBeginStateNameIndex)) {
            g_sebastianStunController = object;
            g_sebastianStunStartMs = now;
            g_sebastianFirstLandedMs = 0;
            g_sebastianStunAnimEndMs = 0;
            g_sebastianLandedSinceStun = false;
            g_sebastianMissingLandingLogged = false;
            g_sebastianTryStunEndLogged = false;
            Log(L"Sebastian fall trace: StunState began.");
            return;
        }
        if (IsObjectName(function, "OnAnimEnd", L"OnAnimEnd",
                         &g_bossFinisherOnAnimEndNameIndex)) {
            g_sebastianStunAnimEndMs = now;
            Log(L"Sebastian fall trace: StunState.OnAnimEnd entered %llu ms after StunState began and %llu ms after SebastianHasLanded.",
                g_sebastianStunStartMs == 0 ? 0 : now - g_sebastianStunStartMs,
                g_sebastianFirstLandedMs == 0 ? 0 : now - g_sebastianFirstLandedMs);
            return;
        }
        if (IsObjectName(function, "Tick", L"Tick", &g_sebastianTickNameIndex)) {
            g_sebastianExpectedStunGlobalTick = object;
        }
        if (object == g_sebastianStunController && !g_sebastianLandedSinceStun &&
            !g_sebastianMissingLandingLogged && g_sebastianStunStartMs != 0 &&
            now - g_sebastianStunStartMs >= 3000 &&
            IsObjectName(function, "Tick", L"Tick", &g_sebastianTickNameIndex)) {
            g_sebastianMissingLandingLogged = true;
            Log(L"Sebastian fall trace: StunState has run for %llu ms without SebastianHasLanded.",
                now - g_sebastianStunStartMs);
            return;
        }
    }
    if (sebastianController &&
        IsObjectName(function, "SebastianHasLanded", L"SebastianHasLanded",
                     &g_sebastianHasLandedNameIndex)) {
        g_sebastianLandedSinceStun = true;
        if (g_sebastianFirstLandedMs == 0) {
            g_sebastianFirstLandedMs = now;
        }
        Log(L"Sebastian fall trace: SebastianHasLanded entered %llu ms after StunState began.",
            g_sebastianStunStartMs == 0 ? 0 : now - g_sebastianStunStartMs);
        return;
    }
    if (IsObjectName(objectClass, "KidAIPawn_Sebastian_Content",
                     L"KidAIPawn_Sebastian_Content",
                     &g_bossFinisherSebastianClassNameIndex) &&
        IsObjectName(function, "Landed", L"Landed", &g_sebastianLandedNameIndex) &&
        IsFunctionOuterName(function, "KidAIPawn_Sebastian_Content",
                            L"KidAIPawn_Sebastian_Content",
                            &g_bossFinisherSebastianClassNameIndex)) {
        Log(L"Sebastian fall trace: pawn Landed event entered %llu ms after StunState began.",
            g_sebastianStunStartMs == 0 ? 0 : now - g_sebastianStunStartMs);
        return;
    }
    if (sebastianController &&
        !g_sebastianTryStunEndLogged &&
        IsObjectName(function, "TryStunEnd", L"TryStunEnd",
                     &g_sebastianTryStunEndNameIndex) &&
        IsFunctionOuterName(function, "KidAIController_Sebastian",
                            L"KidAIController_Sebastian",
                            &g_sebastianControllerClassNameIndex)) {
        g_sebastianTryStunEndLogged = true;
        Log(L"Sebastian fall trace: controller TryStunEnd entered %llu ms after SebastianHasLanded.",
            g_sebastianFirstLandedMs == 0 ? 0 : now - g_sebastianFirstLandedMs);
        return;
    }
    if (sebastianController &&
        IsFunctionOuterName(function, "RecoveryState", L"RecoveryState",
                            &g_sebastianRecoveryStateNameIndex) &&
        IsObjectName(function, "OnAnimEnd", L"OnAnimEnd",
                     &g_bossFinisherOnAnimEndNameIndex)) {
        Log(L"Sebastian fall trace: RecoveryState.OnAnimEnd entered; floor state is being cleared.");
        return;
    }

    const bool adrenalineFunction = IsFunctionOuterName(
        function, "PlayerAdrenalineBurst", L"PlayerAdrenalineBurst",
        &g_bossFinisherAdrenalineStateNameIndex);
    if (adrenalineFunction && g_sebastianStunStartMs != 0 &&
        now - g_sebastianStunStartMs <= 12000 &&
        IsObjectName(function, "BeginState", L"BeginState",
                     &g_sebastianBeginStateNameIndex)) {
        if (g_sebastianStunAnimEndMs == 0) {
            Log(L"Sebastian critical-input trace: PlayerAdrenalineBurst began %llu ms after landing, BEFORE StunState.OnAnimEnd armed the weak point.",
                g_sebastianFirstLandedMs == 0 ? 0 : now - g_sebastianFirstLandedMs);
        } else {
            Log(L"Sebastian critical-input trace: PlayerAdrenalineBurst began %llu ms after landing and %llu ms after StunState.OnAnimEnd armed the weak point.",
                g_sebastianFirstLandedMs == 0 ? 0 : now - g_sebastianFirstLandedMs,
                now - g_sebastianStunAnimEndMs);
        }
        return;
    }

    if (IsObjectName(function, "OnAdjustSwordAimBySocket", L"OnAdjustSwordAimBySocket",
                     &g_bossFinisherOnAdjustNameIndex) &&
        IsObjectName(objectClass, "KidAIPawn_Sebastian_Content",
                     L"KidAIPawn_Sebastian_Content",
                     &g_bossFinisherSebastianClassNameIndex)) {
        if (object != g_sebastianAdjustPawn) {
            g_sebastianAdjustPawn = object;
            g_sebastianAdjustCount = 0;
        }
        ++g_sebastianAdjustCount;
        g_bossFinisherLastAdjustMs = now;
        g_sebastianThirdCutStartMs = now;
        g_bossFinisherTraceUntilMs = now + 15000;
        g_sebastianMatineeBoundarySeen = false;
        Log(L"Sebastian critical-input trace: critical hit #%d succeeded; watching the following 15 seconds for the final-round Matinee and loading transition.",
            g_sebastianAdjustCount);
        return;
    }
}

bool IsCardiacFunctionName(void* function) {
    return IsObjectName(function, "TickMikaCardiacMassage", L"TickMikaCardiacMassage",
                        &g_cardiacTargetNameIndex);
}

volatile LONG g_kismetUpdateNameIndex = -1;
volatile LONG g_kismetActivatedNameIndex = -1;

int FindKismetLatentTargetForObject(void* object) {
    if (object == nullptr) {
        return -1;
    }

    void* objectClass = nullptr;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           static_cast<const BYTE*>(object) + kUObjectClassOffset,
                           &objectClass, sizeof(objectClass), &bytesRead) ||
        bytesRead != sizeof(objectClass) || objectClass == nullptr) {
        return -1;
    }

    for (SIZE_T index = 0; index < kKismetLatentActionCount; ++index) {
        KismetLatentActionTarget& target = g_kismetLatentTargets[index];
        if (IsObjectName(objectClass, target.className, target.classNameWide,
                         &target.classNameIndex)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int FindKismetLatentTargetForUpdateFunction(void* function) {
    for (SIZE_T index = 0; index < kKismetLatentActionCount; ++index) {
        PVOID updateFunction = g_kismetLatentTargets[index].updateFunction;
        if (updateFunction != nullptr && function == updateFunction) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

KismetLatentState* FindOrCreateKismetLatentState(void* object, int targetIndex) {
    KismetLatentState* empty = nullptr;
    for (SIZE_T index = 0; index < kKismetLatentStateSlots; ++index) {
        KismetLatentState& state = g_kismetLatentStates[index];
        if (state.object == object && state.targetIndex == targetIndex) {
            return &state;
        }
        if (state.object == nullptr && empty == nullptr) {
            empty = &state;
        }
    }
    if (empty != nullptr) {
        empty->object = object;
        empty->targetIndex = targetIndex;
        empty->lastGeneration = LONG_MIN;
        return empty;
    }
    if (InterlockedCompareExchange(&g_kismetLatentCapacityWarning, 1, 0) == 0) {
        Log(L"Kismet latent timing state table is full; untracked actions will remain unmodified.");
    }
    return nullptr;
}

PVOID volatile g_kismetActivatedFunctions[8] = {};

bool IsKismetActivatedFunction(const void* function) {
    for (const PVOID volatile& slot : g_kismetActivatedFunctions) {
        if (slot == function) {
            return true;
        }
    }
    return false;
}

void ObserveKismetLatentActivation(void* object, void* function) {
    if (!g_kismetLatentTimingFix || object == nullptr || function == nullptr ||
        !IsObjectName(function, "Activated", L"Activated", &g_kismetActivatedNameIndex)) {
        return;
    }

    const int targetIndex = FindKismetLatentTargetForObject(object);
    if (targetIndex < 0) {
        return;
    }
    MarkInteresting(function);
    for (PVOID volatile& slot : g_kismetActivatedFunctions) {
        if (slot == function) {
            break;
        }
        if (InterlockedCompareExchangePointer(&slot, function, nullptr) == nullptr) {
            break;
        }
    }

    KismetLatentState* state = FindOrCreateKismetLatentState(object, targetIndex);
    if (state != nullptr) {
        state->lastGeneration = InterlockedCompareExchange(&g_scriptTickGeneration, 0, 0);
    }
}

bool TryDiscoverKismetLatentUpdate(void* object, void* function) {
    if (!g_kismetLatentTimingFix || object == nullptr || function == nullptr ||
        FindKismetLatentTargetForUpdateFunction(function) >= 0 ||
        !IsObjectName(function, "Update", L"Update", &g_kismetUpdateNameIndex)) {
        return false;
    }

    const int targetIndex = FindKismetLatentTargetForObject(object);
    if (targetIndex < 0) {
        return false;
    }

    ScriptArrayHeader script = {};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           static_cast<const BYTE*>(function) + 0x54,
                           &script, sizeof(script), &bytesRead) ||
        bytesRead != sizeof(script) || script.data == nullptr ||
        script.count < 3 || script.count > 1024 * 1024 ||
        script.capacity < script.count) {
        Log(L"%S.Update was identified, but its Script array was invalid.",
            g_kismetLatentTargets[targetIndex].className);
        return false;
    }

    // A suppressed latent Update must return true. Returning false would
    // deactivate the SequenceOp rather than merely defer its frame counter.
    // Use the function's own compiler-emitted tail, so the VM keeps its normal
    // return-value handling and no synthetic bytecode is introduced.
    constexpr BYTE returnTrueAndEnd[] = {0x04, 0x27, 0x53};
    BYTE* returnCode = script.data + script.count - ArrayCount(returnTrueAndEnd);
    BYTE terminator[ArrayCount(returnTrueAndEnd)] = {};
    if (!ReadProcessMemory(GetCurrentProcess(), returnCode, terminator,
                           sizeof(terminator), &bytesRead) ||
        bytesRead != sizeof(terminator) ||
        memcmp(terminator, returnTrueAndEnd, sizeof(terminator)) != 0) {
        Log(L"%S.Update did not end with the expected true return; timing fix inactive.",
            g_kismetLatentTargets[targetIndex].className);
        return false;
    }

    KismetLatentActionTarget& target = g_kismetLatentTargets[targetIndex];
    InterlockedExchangePointer(&target.returnTrueCode, returnCode);
    InterlockedExchangePointer(&target.updateFunction, function);
    MarkInteresting(function);
    Log(L"%S.Update located on first live execution; 30 Hz latent timing armed.",
        target.className);
    return true;
}

bool ShouldSuppressKismetLatentUpdate(void* object, void* function, void** returnCode) {
    if (returnCode == nullptr || !g_kismetLatentTimingFix) {
        return false;
    }

    const int targetIndex = FindKismetLatentTargetForUpdateFunction(function);
    if (targetIndex < 0 || g_scriptTickLimiterInstalled == 0) {
        return false;
    }

    void* verifiedReturnCode = g_kismetLatentTargets[targetIndex].returnTrueCode;
    if (!ReturnCodeLive(function, verifiedReturnCode)) {
        return false;
    }

    KismetLatentState* state = FindOrCreateKismetLatentState(object, targetIndex);
    if (state == nullptr) {
        return false;
    }

    const LONG generation = g_scriptTickGeneration;
    if (state->lastGeneration == LONG_MIN) {
        // The action was already live when first discovered. Preserve that
        // first call, then synchronize subsequent calls to the fixed clock.
        state->lastGeneration = generation;
        return false;
    }
    if (state->lastGeneration != generation) {
        state->lastGeneration = generation;
        return false;
    }

    *returnCode = verifiedReturnCode;
    return true;
}

bool TryDiscoverNewMessageTick(void* object, void* function) {
    if (!g_newMessageTimingFix || function == nullptr || object == nullptr ||
        function == g_newMessageTickFunction ||
        !IsObjectName(function, "Tick", L"Tick", &g_newMessageTickNameIndex)) {
        return false;
    }

    void* objectClass = nullptr;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           static_cast<const BYTE*>(object) + kUObjectClassOffset,
                           &objectClass, sizeof(objectClass), &bytesRead) ||
        bytesRead != sizeof(objectClass) ||
        !IsObjectName(objectClass, "KidNewMessage", L"KidNewMessage",
                      &g_newMessageClassNameIndex)) {
        return false;
    }

    ScriptArrayHeader script = {};
    if (!ReadProcessMemory(GetCurrentProcess(),
                           static_cast<const BYTE*>(function) + 0x54,
                           &script, sizeof(script), &bytesRead) ||
        bytesRead != sizeof(script) || script.data == nullptr ||
        script.count < 2 || script.count > 1024 * 1024 ||
        script.capacity < script.count) {
        Log(L"KidNewMessage.Tick was identified, but its Script array was invalid.");
        return false;
    }

    // Use the function's own compiler-emitted terminator instead of synthetic
    // bytecode. EX_Return, EX_Nothing, EX_EndOfScript is 04 0B 53 in this UE3
    // build. Keeping the cursor inside the live Script array also preserves the
    // VM's normal bounds and ownership assumptions.
    constexpr BYTE returnVoidAndEnd[] = {0x04, 0x0B, 0x53};
    if (script.count < static_cast<int>(ArrayCount(returnVoidAndEnd))) {
        Log(L"KidNewMessage.Tick was identified, but its Script was too short.");
        return false;
    }
    BYTE* returnCode = script.data + script.count - ArrayCount(returnVoidAndEnd);
    BYTE terminator[ArrayCount(returnVoidAndEnd)] = {};
    bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), returnCode, terminator,
                           sizeof(terminator), &bytesRead) ||
        bytesRead != sizeof(terminator) ||
        memcmp(terminator, returnVoidAndEnd, sizeof(terminator)) != 0) {
        Log(L"KidNewMessage.Tick did not end with the expected void return; timing fix inactive.");
        return false;
    }

    InterlockedExchangePointer(&g_newMessageReturnCode, returnCode);
    InterlockedExchangePointer(&g_newMessageTickFunction, function);
    MarkInteresting(function);
    Log(L"KidNewMessage.Tick located on first live execution; fixed-step message timing armed.");
    return true;
}

bool ShouldSuppressNewMessageTick(void* object, void* function) {
    static void* messageObject = nullptr;
    static LONG lastGeneration = 0;

    void* targetFunction = g_newMessageTickFunction;
    if (!g_newMessageTimingFix || function != targetFunction ||
        targetFunction == nullptr || g_scriptTickLimiterInstalled == 0) {
        return false;
    }

    const LONG generation = g_scriptTickGeneration;
    if (object != messageObject) {
        messageObject = object;
        lastGeneration = generation;
        return false;
    }
    if (generation == lastGeneration) {
        if (InterlockedCompareExchange(&g_newMessageFixActivated, 1, 0) == 0) {
            Log(L"KidNewMessage.Tick fixed-step gate reached; excess rendered-frame ticks are suppressed.");
        }
        return true;
    }

    // Consume the current generation as one logical message tick. If the UI
    // limiter performed several bounded catch-up steps, deliberately collapse
    // them here: replaying one cached input edge through six message ticks in a
    // single rendered frame would recreate the dismissal bug after a hitch.
    lastGeneration = generation;
    return false;
}

void DumpCardiacScript(const BYTE* bytes, DWORD byteCount) {
    wchar_t dumpPath[MAX_PATH] = {};
    BuildSiblingPath(L".cardiac.bin", dumpPath, ArrayCount(dumpPath));
    HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log(L"Could not create cardiac bytecode dump (error %lu).", GetLastError());
        return;
    }
    DWORD written = 0;
    const BOOL okay = WriteFile(file, bytes, byteCount, &written, nullptr);
    CloseHandle(file);
    if (!okay || written != byteCount) {
        Log(L"Cardiac bytecode dump was incomplete (%lu/%lu bytes, error %lu).",
            written, byteCount, GetLastError());
    }
}

void TryActivateCardiacFromFunction(void* function) {
    if (!g_cardiacMassageFix ||
        function == g_cardiacBinding.function ||
        !IsCardiacFunctionName(function) ||
        InterlockedCompareExchange(&g_cardiacTargetSeen, 1, 0) != 0) {
        return;
    }

    // UStruct::Script is TArray<BYTE> at +0x54 in this executable. Read the
    // header defensively before claiming the UFunction so a partially loaded
    // object can be reconsidered on its next invocation.
    ScriptArrayHeader script = {};
    SIZE_T bytesRead = 0;
    BYTE* scriptMember = static_cast<BYTE*>(function) + 0x54;
    if (!ReadProcessMemory(GetCurrentProcess(), scriptMember, &script, sizeof(script),
                           &bytesRead) || bytesRead != sizeof(script) ||
        script.data == nullptr || script.count < 0x120 || script.count > 1024 * 1024 ||
        script.capacity < script.count) {
        InterlockedExchange(&g_cardiacTargetSeen, 0);
        Log(L"TickMikaCardiacMassage was named by the VM, but its Script array was not ready.");
        return;
    }

    BYTE* snapshot = static_cast<BYTE*>(VirtualAlloc(
        nullptr, static_cast<SIZE_T>(script.count), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (snapshot == nullptr) {
        InterlockedExchange(&g_cardiacTargetSeen, 0);
        return;
    }
    bytesRead = 0;
    const bool copied = ReadProcessMemory(
        GetCurrentProcess(), script.data, snapshot, static_cast<SIZE_T>(script.count),
        &bytesRead) != FALSE && bytesRead == static_cast<SIZE_T>(script.count);
    if (!copied) {
        VirtualFree(snapshot, 0, MEM_RELEASE);
        InterlockedExchange(&g_cardiacTargetSeen, 0);
        return;
    }

    InterlockedExchange(&g_cardiacActivated, 0);
    CardiacScanStats stats = {};
    const SIZE_T windowBefore = static_cast<SIZE_T>(-kCardiacRecoveryFromDecay);
    const SIZE_T windowAfter = static_cast<SIZE_T>(kCardiacDeltaThirtyFromDecay) +
                               ArrayCount(kCardiacDeltaThirtyPattern);
    for (SIZE_T offset = windowBefore;
         offset + windowAfter <= static_cast<SIZE_T>(script.count); ++offset) {
        if (!MatchesPattern(snapshot + offset, kCardiacDecayPattern,
                            ArrayCount(kCardiacDecayPattern))) {
            continue;
        }
        ++stats.anchors;
        if (InspectCardiacMassageAt(script.data + offset, true, &stats)) {
            BindScript(&g_cardiacBinding, function);
            MarkInteresting(function);
            InterlockedExchange(&g_cardiacActivated, 1);
            Log(L"TickMikaCardiacMassage located on first live execution; DeltaTime constants activated.");
            break;
        }
    }
    if (InterlockedCompareExchange(&g_cardiacActivated, 0, 0) == 0) {
        DumpCardiacScript(snapshot, static_cast<DWORD>(script.count));
        Log(L"TickMikaCardiacMassage live UFunction found (%d-byte Script), but the operand signature did not validate: anchors=%Iu, complete=%Iu, live=%Iu, constants=%Iu. Live bytecode dumped to KIDFrameRateFix.cardiac.bin.",
            script.count, stats.anchors, stats.completePatterns,
            stats.livePointers, stats.validConstants);
    }
    VirtualFree(snapshot, 0, MEM_RELEASE);
}

void* __fastcall ScriptAwareFrameConstructor(
    void* frame, void*, void* object, void* function, int codeOffset,
    void* locals, void* previousFrame) {
    // FFrame construction is the hottest hook in the plugin. Reflective
    // discovery (GNames reads, outer-chain walks, and bytecode validation)
    // is performed once per UFunction; all subsequent calls use only the
    // matched-function fast paths below.
    const bool firstInvocationOfFunction = ClaimFunctionForInspection(
        g_checkedFunctions, function);
    if (g_bossFinisherDiagnostics &&
        (firstInvocationOfFunction ||
         (g_bossFinisherTraceUntilMs != 0 &&
          GetTickCount64() <= g_bossFinisherTraceUntilMs))) {
        ObserveBossFinisherTiming(object, function, locals);
    }
    if (firstInvocationOfFunction) {
        TryActivateSmoothUiManagerPatch(function);
        TryDiscoverUiFrameGateFunction(function);
        TryActivateSebastianStunFallFix(object, function);
        TryDiscoverSoundRetriggerFunction(function);
        TryDiscoverFixedStepFunction(function);
        TryDiscoverNewMessageTick(object, function);
        ObserveKismetLatentActivation(object, function);
        TryDiscoverKismetLatentUpdate(object, function);
        TryActivateCardiacFromFunction(function);
        TryDiscoverDeltaOperand(function);
        TryDiscoverTracedFunction(function);
        TryDiscoverRootMotionFunction(function);
        TryDiscoverCounterGate(function);
        TryDiscoverSebastianStunTick(function);
    }
    if (!IsInteresting(function)) {
        return g_originalFrameConstructor(
            frame, object, function, codeOffset, locals, previousFrame);
    }
    if (function == g_sebastianStunTickFunction) {
        g_sebastianExpectedStunGlobalTick = object;
    }
    if (IsKismetActivatedFunction(function)) {
        ObserveKismetLatentActivation(object, function);
    }
    UpdateSebastianStunFallFix(object, function);
    void* result = g_originalFrameConstructor(
        frame, object, function, codeOffset, locals, previousFrame);
    void* uiReturnCode = nullptr;
    if (ShouldSuppressUiFrameGate(object, function, &uiReturnCode) &&
        uiReturnCode != nullptr) {
        memcpy(static_cast<BYTE*>(frame) + kFFrameCodeOffset,
               &uiReturnCode, sizeof(uiReturnCode));
    }
    void* soundReturnCode = nullptr;
    if (ShouldSuppressSoundRetrigger(object, function, &soundReturnCode) &&
        soundReturnCode != nullptr) {
        memcpy(static_cast<BYTE*>(frame) + kFFrameCodeOffset,
               &soundReturnCode, sizeof(soundReturnCode));
    }
    if (ShouldSuppressNewMessageTick(object, function)) {
        void* returnCode = InterlockedCompareExchangePointer(
            &g_newMessageReturnCode, nullptr, nullptr);
        if (ReturnCodeLive(function, returnCode)) {
            memcpy(static_cast<BYTE*>(frame) + kFFrameCodeOffset,
                   &returnCode, sizeof(returnCode));
        }
    }
    void* kismetReturnCode = nullptr;
    if (ShouldSuppressKismetLatentUpdate(object, function, &kismetReturnCode) &&
        kismetReturnCode != nullptr) {
        memcpy(static_cast<BYTE*>(frame) + kFFrameCodeOffset,
               &kismetReturnCode, sizeof(kismetReturnCode));
    }
    return result;
}

bool RedirectFloatOperand(BYTE* operand, const float* replacement, const wchar_t* label) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(operand, sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log(L"VirtualProtect for %s operand failed (error %lu).", label, GetLastError());
        return false;
    }

    *reinterpret_cast<const float**>(operand) = replacement;
    FlushInstructionCache(GetCurrentProcess(), operand, sizeof(DWORD));

    DWORD ignored = 0;
    VirtualProtect(operand, sizeof(DWORD), oldProtect, &ignored);
    return true;
}

template <typename FunctionType>
bool InstallHook(BYTE* target, SIZE_T hookLength, const void* replacement,
                 FunctionType* original, const wchar_t* label) {
    BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, hookLength + 5,
                                                       MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
        Log(L"VirtualAlloc for %s trampoline failed (error %lu).", label, GetLastError());
        return false;
    }

    memcpy(trampoline, target, hookLength);
    WriteRelativeJump(trampoline + hookLength, target + hookLength);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, hookLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log(L"VirtualProtect for %s failed (error %lu).", label, GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    *original = reinterpret_cast<FunctionType>(trampoline);

    WriteRelativeJump(target, replacement);
    for (SIZE_T index = 5; index < hookLength; ++index) {
        target[index] = 0x90;
    }
    FlushInstructionCache(GetCurrentProcess(), target, hookLength);
    DWORD ignored = 0;
    VirtualProtect(target, hookLength, oldProtect, &ignored);

    return true;
}

void Initialize() {
    BuildSiblingPath(L".ini", g_iniPath, ArrayCount(g_iniPath));
    BuildSiblingPath(L".log", g_logPath, ArrayCount(g_logPath));
    // Start a fresh log for each game session.
    HANDLE logFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(logFile);
    }
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(g_module, modulePath, ArrayCount(modulePath));
    const wchar_t* moduleName = wcsrchr(modulePath, L'\\');
    Log(L"KIDFrameRateFix %S loading as %s.", KIDFRAMERATEFIX_VERSION,
        moduleName != nullptr ? moduleName + 1 : modulePath);
    LoadConfiguration();
    LoadScriptTrace();
    LoadSmoothUiGatePatterns();
    if (!g_enabled) {
        Log(L"All patches disabled (Enabled=0 in KIDFrameRateFix.ini).");
        return;
    }

    HMODULE executable = GetModuleHandleW(nullptr);
    g_executableBase = reinterpret_cast<BYTE*>(executable);
    BYTE* textBegin = nullptr;
    SIZE_T textSize = 0;
    if (!ValidateExecutable(executable, &textBegin, &textSize)) {
        return;
    }

    BYTE* movieAdvance = FindUniquePattern(textBegin, textSize,
        kMovieAdvancePattern, ArrayCount(kMovieAdvancePattern), L"FGFxEngine movie advance");
    BYTE* scriptTick = FindUniquePattern(textBegin, textSize,
        kScriptTickPattern, ArrayCount(kScriptTickPattern), L"UKidGFxMgr::Tick");
    if (movieAdvance == nullptr || scriptTick == nullptr) {
        return;
    }

    BYTE* movieTick = movieAdvance + kMovieTickFromAdvancePattern;
    if (memcmp(movieTick, kMovieTickPrologue, sizeof(kMovieTickPrologue)) != 0) {
        Log(L"FGFxEngine::Tick prologue did not match; dynamic UI patch not installed.");
        return;
    }

    const float* originalAdvanceDelta =
        *reinterpret_cast<const float**>(movieAdvance + kAdvanceOperandOffset);
    const float* originalPostAdvanceDelta =
        *reinterpret_cast<const float**>(movieAdvance + kPostAdvanceOperandOffset);
    if (originalAdvanceDelta != originalPostAdvanceDelta ||
        *originalAdvanceDelta < 0.0333f || *originalAdvanceDelta > 0.0334f) {
        Log(L"FGFxEngine operands did not reference the expected 1/30 constant; patch not installed.");
        return;
    }

    if (!RedirectFloatOperand(movieAdvance + kAdvanceOperandOffset,
                              &g_movieAdvanceDelta, L"Movie::Advance") ||
        !RedirectFloatOperand(movieAdvance + kPostAdvanceOperandOffset,
                              &g_movieAdvanceDelta, L"PostAdvance")) {
        return;
    }

    const bool movieHookInstalled = InstallHook(movieTick, kMovieTickHookLength,
        reinterpret_cast<const void*>(&DynamicMovieTick),
        &g_originalMovieTick, L"FGFxEngine::Tick");
    if (!movieHookInstalled) {
        Log(L"Dynamic FGFxEngine::Tick hook failed; patch not installed.");
        return;
    }

    const bool scriptHookInstalled = InstallHook(scriptTick, kScriptHookLength,
        reinterpret_cast<const void*>(&RateLimitedScriptTick),
        &g_originalScriptTick, L"UKidGFxMgr::Tick");
    if (!scriptHookInstalled) {
        Log(L"Warning: Scaleform movie timestep patch is active, but the script tick limiter failed.");
    } else {
        InterlockedExchange(&g_scriptTickLimiterInstalled, 1);
    }

    auto* base = reinterpret_cast<BYTE*>(executable);
    // Before WinMain reads the command line (see Resolution.cpp).
    ApplyStartupResolution(g_resolutionMode, g_resolutionWidth, g_resolutionHeight);
    if (g_timingProbe) {
        g_timingProbeInstalled = TimingProbeInstall(base);
    }
    if (g_crashDump) {
        wchar_t dumpPath[MAX_PATH] = {};
        BuildSiblingPath(L"-crash.dmp", dumpPath, ArrayCount(dumpPath));
        CrashDumpInstall(textBegin, textSize, dumpPath);
    }
    Log(L"Scaleform Movie::Advance timestep patched at KidGame.exe+0x%08X; it now follows incoming DeltaTime.",
        static_cast<unsigned>(movieAdvance - base));
    if (scriptHookInstalled) {
        if (g_smoothUiExperimental) {
            Log(L"Script tick hook installed at KidGame.exe+0x%08X. Full-rate UI enabled; frame-counted helpers keep a 30 Hz clock.",
                static_cast<unsigned>(scriptTick - base));
        } else {
            Log(L"Script tick hook installed at KidGame.exe+0x%08X. UI rate: %d Hz using elapsed-time accumulation.",
                static_cast<unsigned>(scriptTick - base), g_uiTickFps);
        }
    }

    if (g_fullscreenFadeFix) {
        BYTE* fadeTile = FindUniquePattern(textBegin, textSize, kFadeTilePattern,
            ArrayCount(kFadeTilePattern), L"Cinematic fade DrawTile");
        const float* heightConstant = fadeTile == nullptr ? nullptr :
            *reinterpret_cast<const float**>(fadeTile + kFadeHeightOperandOffset);
        const float* widthConstant = fadeTile == nullptr ? nullptr :
            *reinterpret_cast<const float**>(fadeTile + kFadeWidthOperandOffset);
        // A value other than 1080/1920 means another fix (for example an
        // SUWSF fade patch) already rewrote the shared constants; that is
        // harmless because the operands are redirected away from them.
        if (fadeTile != nullptr && widthConstant + 1 == heightConstant &&
            RedirectFloatOperand(fadeTile + kFadeHeightOperandOffset, &g_fadeHeight,
                                 L"fade height") &&
            RedirectFloatOperand(fadeTile + kFadeWidthOperandOffset, &g_fadeWidth,
                                 L"fade width")) {
            Log(L"Cinematic fade size patched at KidGame.exe+0x%08X; it follows the window size.",
                static_cast<unsigned>(fadeTile - base));
        } else {
            g_fullscreenFadeFix = false;
            Log(L"Cinematic fade fix was not installed.");
        }
    }

    if (g_supersampling > 1.0f) {
        BYTE* needsUpscale = FindUniquePattern(textBegin, textSize, kNeedsUpscalePattern,
            ArrayCount(kNeedsUpscalePattern), L"FSystemSettings::NeedsUpscale");
        BYTE* scale = FindUniquePattern(textBegin, textSize, kScaleScreenCoordsPattern,
            ArrayCount(kScaleScreenCoordsPattern), L"FSystemSettings::ScaleScreenCoords");
        BYTE* unscale = FindUniquePattern(textBegin, textSize, kUnScaleScreenCoordsPattern,
            ArrayCount(kUnScaleScreenCoordsPattern), L"FSystemSettings::UnScaleScreenCoords");
        BYTE* allocate = FindUniquePattern(textBegin, textSize, kSceneTargetsAllocatePattern,
            ArrayCount(kSceneTargetsAllocatePattern), L"FSceneRenderTargets::Allocate");
        BYTE* clampX = FindUniquePattern(textBegin, textSize, kViewTargetClampXPattern,
            ArrayCount(kViewTargetClampXPattern), L"view region clamp (X)");
        BYTE* clampY = FindUniquePattern(textBegin, textSize, kViewTargetClampYPattern,
            ArrayCount(kViewTargetClampYPattern), L"view region clamp (Y)");
        BYTE* finalPassCall = FindUniquePattern(textBegin, textSize, kFinalPassUnScaleCallPattern,
            ArrayCount(kFinalPassUnScaleCallPattern), L"final pass UnScaleScreenCoords call");
        if (finalPassCall != nullptr && unscale != nullptr) {
            BYTE* call = finalPassCall + kFinalPassUnScaleCallOffset;
            if (call + 5 + *reinterpret_cast<const INT32*>(call + 1) == unscale) {
                g_finalPassUnScaleReturn = call + 5;
            }
        }
        BYTE* sceneColorViewport = FindUniquePattern(textBegin, textSize, kSceneColorViewportPattern,
            ArrayCount(kSceneColorViewportPattern), L"scene colour viewport");
        // The two clamps are consecutive statements of one function.
        if (clampX != nullptr && clampY != nullptr && (clampY <= clampX || clampY - clampX > 0x40)) {
            clampY = nullptr;
        }

        // All or none: a partial set would render outside the buffers.
        if (needsUpscale != nullptr && scale != nullptr && unscale != nullptr && allocate != nullptr &&
            clampX != nullptr && clampY != nullptr && g_finalPassUnScaleReturn != nullptr &&
            sceneColorViewport != nullptr &&
            InstallHook(allocate, kSceneTargetsAllocateHookLength,
                reinterpret_cast<const void*>(&SceneTargetsAllocate),
                &g_originalSceneTargetsAllocate, L"FSceneRenderTargets::Allocate") &&
            InstallHook(unscale, kUnScaleScreenCoordsHookLength,
                reinterpret_cast<const void*>(&UnScaleScreenCoords),
                &g_originalUnScaleScreenCoords, L"FSystemSettings::UnScaleScreenCoords") &&
            InstallHook(needsUpscale, kNeedsUpscaleHookLength,
                reinterpret_cast<const void*>(&NeedsUpscale),
                &g_originalNeedsUpscale, L"FSystemSettings::NeedsUpscale") &&
            InstallHook(scale, kScaleScreenCoordsHookLength,
                reinterpret_cast<const void*>(&ScaleScreenCoords),
                &g_originalScaleScreenCoords, L"FSystemSettings::ScaleScreenCoords")) {
            InstallSceneColorViewportStub(sceneColorViewport + kSceneColorViewportSiteOffset);
            RemoveShortJump(clampX + kViewTargetClampJumpOffset, L"view region clamp (X)");
            RemoveShortJump(clampY + kViewTargetClampJumpOffset, L"view region clamp (Y)");
            Log(L"Supersampling hooks installed: %gx%g samples per pixel (%gx SSAA).",
                g_supersampling, g_supersampling, g_supersampling * g_supersampling);
        } else {
            g_supersampling = 1.0f;
            Log(L"Supersampling hooks were not installed; rendering at native resolution.");
        }
    }

    if (g_fovHorPlus || g_fovScale != 1.0f) {
        BYTE* projection = FindUniquePattern(textBegin, textSize, kProjectionFovPattern,
            ArrayCount(kProjectionFovPattern), L"CalcSceneView FOV");
        const DWORD constraintOffset = projection == nullptr ? 0 :
            *reinterpret_cast<const DWORD*>(projection + kProjectionFovConstraintOffset);
        if (projection != nullptr && constraintOffset > 0x3C && constraintOffset < 0x1000 &&
            constraintOffset == *reinterpret_cast<const DWORD*>(projection + kProjectionFovConstraintOffset + 8) &&
            InstallProjectionFovHook(projection + kProjectionFovHookOffset)) {
            g_aspectConstraintOffset = static_cast<ptrdiff_t>(constraintOffset);
            Log(L"FOV hook installed at KidGame.exe+0x%08X: %s, FOV %.1f.",
                static_cast<unsigned>(projection + kProjectionFovHookOffset - base),
                g_fovHorPlus ? L"hor+" : L"vert-", g_fov);
        } else {
            Log(L"FOV hook was not installed; the game's FOV applies unchanged.");
        }
    }

    if (g_particleCollisionFix) {
        BYTE* collisionUpdate = FindUniquePattern(
            textBegin, textSize, kCollisionUpdatePattern,
            ArrayCount(kCollisionUpdatePattern), L"UParticleModuleCollision::Update");
        if (collisionUpdate != nullptr &&
            InstallHook(collisionUpdate, kCollisionUpdateHookLength,
                reinterpret_cast<const void*>(&DeltaTimedRibbonCollisionUpdate),
                &g_originalCollisionUpdate, L"UParticleModuleCollision::Update")) {
            Log(L"Ribbon collision integrity and 30 Hz timing hook installed at KidGame.exe+0x%08X.",
                static_cast<unsigned>(collisionUpdate - base));
        } else {
            Log(L"Ribbon collision fix was not installed; original particle behavior remains active.");
        }
    }

    if (g_smoothUiExperimental || g_newMessageTimingFix || g_kismetLatentTimingFix ||
        g_sebastianStunFallFix || g_soundRetriggerFix ||
        g_giantHeadStunTimingFix || g_thomasMotionTimingFix ||
        g_giantHeadBeamTimingFix || g_sebastianAbsorbTimingFix ||
        g_bossFinisherDiagnostics || g_collectableAbsorbFix || g_tracePatternCount != 0 ||
        g_cinematicRootMotionFix ||
        g_droppedPickupFix || g_giantHeadThrowSpinFix ||
        g_cardiacMassageFix) {
            BYTE* frameConstructor = FindUniquePattern(
                textBegin, textSize, kFrameConstructorPattern,
                ArrayCount(kFrameConstructorPattern), L"FFrame constructor");
            if (frameConstructor != nullptr &&
                InstallHook(frameConstructor, kFrameConstructorHookLength,
                    reinterpret_cast<const void*>(&ScriptAwareFrameConstructor),
                    &g_originalFrameConstructor, L"FFrame constructor")) {
                Log(L"Script on-demand hook installed at KidGame.exe+0x%08X; waiting for enabled live functions.",
                    static_cast<unsigned>(frameConstructor - base));
            } else {
                Log(L"Script on-demand hook was not installed; script timing fixes remain inactive.");
            }
    }

    if (g_audioDeviceFix) {
        BYTE* audioInit = FindUniquePattern(textBegin, textSize, kAudioDeviceInitPattern,
            ArrayCount(kAudioDeviceInitPattern), L"UXAudio2Device::Init");
        if (audioInit != nullptr &&
            InstallHook(audioInit, kAudioDeviceInitHookLength,
                reinterpret_cast<const void*>(&AudioDeviceInit),
                &g_originalAudioDeviceInit, L"UXAudio2Device::Init")) {
            Log(L"Audio device hook installed at KidGame.exe+0x%08X.",
                static_cast<unsigned>(audioInit - base));
        } else {
            Log(L"Audio device fix was not installed; KidEngine.ini audio settings apply unchanged.");
        }
    }

    if (g_tracePatternCount != 0 || g_cinematicRootMotionFix || g_smoothUiExperimental ||
        g_giantHeadStunTimingFix || g_thomasMotionTimingFix ||
        g_giantHeadBeamTimingFix || g_sebastianAbsorbTimingFix) {
        BYTE* processInternal = FindUniquePattern(
            textBegin, textSize, kProcessInternalPattern,
            ArrayCount(kProcessInternalPattern), L"UObject::ProcessInternal");
        if (processInternal != nullptr &&
            InstallHook(processInternal, kProcessInternalHookLength,
                reinterpret_cast<const void*>(&ScriptProcessInternal),
                &g_originalProcessInternal, L"UObject::ProcessInternal")) {
            Log(L"Script body hook installed at KidGame.exe+0x%08X.",
                static_cast<unsigned>(processInternal - base));
        } else {
            for (FixedStepTarget& target : g_fixedStepTargets) {
                *target.enabled = false;
            }
            Log(L"Script body hook was not installed; fixed-step boss timing fixes disabled.");
        }
    }
    return;
}

} // namespace

// Logging entry point for the other translation units.
void KidLog(const wchar_t* format, ...) {
    wchar_t line[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, ArrayCount(line), _TRUNCATE, format, args);
    va_end(args);
    Log(L"%s", line);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);

        // Guard against two copies in one process (for example the dinput8
        // proxy plus a leftover KIDFrameRateFix.asi). Only the first patches.
        wchar_t mutexName[64] = {};
        _snwprintf_s(mutexName, ArrayCount(mutexName), _TRUNCATE,
                     L"Local\\KIDFrameRateFix.%lu", GetCurrentProcessId());
        HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, mutexName);
        if (instanceMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
            return TRUE;
        }

        // Patch synchronously. As a dinput8 proxy this DLL is a static import
        // and runs before the game's entry point, so no game thread can be
        // executing the code being hooked. The work uses only kernel32.
        Initialize();
    }
    return TRUE;
}
