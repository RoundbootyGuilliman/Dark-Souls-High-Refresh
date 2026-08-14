#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

// Dark Souls Remastered 1.03 (SHA-1 F0ECFBE20D780751248DFB2A9759D6215E246676)
// imports QueryPerformanceCounter through this IAT slot. Only the two reads in
// its render-frame busy wait receive a scaled clock; all simulation and system
// timing reads remain untouched.
#define QPC_IAT_RVA 0x20B5120
#define PACER_FIRST_CHECK_RETURN_RVA 0xCDF898
#define PACER_LOOP_CHECK_RETURN_RVA  0xCDF8CE
#define SIMULATION_STEP_VTABLE_ENTRY_RVA 0x1AC54A8
#define SIMULATION_STEP_RVA 0x24DDD0
#define FRPG_FX_UPDATE_VTABLE_ENTRY_RVA 0x13678F8
#define FRPG_FX_UPDATE_RVA 0x4F6380
#define REMO_ACTIVE_STATE_ENTRY_RVA 0x1D0DB08
#define REMO_ACTIVE_STATE_RVA 0x28EAE0
#define HAVOK_STEP_CALL_RVA 0x15BD5E
#define HAVOK_STEP_RVA 0x2A18B0


typedef BOOL (WINAPI *QueryPerformanceCounterFn)(LARGE_INTEGER *);
typedef void (*SimulationStepFn)(void *step, float frame_time);
typedef void (*FrpgFxUpdateFn)(void *manager, float frame_time);
typedef void (*RemoActiveStateFn)(void *step, float frame_time, void *task_item);

static HMODULE g_module;
static BYTE *g_image_base;
static QueryPerformanceCounterFn g_query_performance_counter;
static SimulationStepFn g_simulation_step;
static FrpgFxUpdateFn g_frpg_fx_update;
static RemoActiveStateFn g_remo_active_state;
static void *g_havok_call_relay;
static UINT g_target_fps = 140;
static volatile LONG64 g_pacer_anchor;
static volatile LONG g_activation_logged;
static volatile LONG g_simulation_logged;
static volatile LONG g_frpg_fx_logged;
static volatile LONG g_remo_logged;
static volatile LONG g_havok_patch_state;
static volatile LONG g_remo_tick_phase;
static ULONGLONG g_remo_last_input_ms;
static ULONGLONG g_install_time_ms;
static char g_log_path[MAX_PATH];


static void BuildSiblingPath(char *output, const char *name)
{
    GetModuleFileNameA(g_module, output, MAX_PATH);
    char *slash = output;
    for (char *scan = output; *scan; ++scan)
        if (*scan == '\\' || *scan == '/') slash = scan + 1;
    lstrcpynA(slash, name, (int)(MAX_PATH - (slash - output)));
}

static void LogLine(const char *format, ...)
{
    char text[768];
    va_list args;
    va_start(args, format);
    _vsnprintf(text, sizeof(text) - 3, format, args);
    va_end(args);
    text[sizeof(text) - 3] = 0;
    lstrcatA(text, "\r\n");

    HANDLE file = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(file, text, (DWORD)lstrlenA(text), &written, NULL);
        CloseHandle(file);
    }
}

static BOOL PatchPointer(void **address, void *replacement)
{
    DWORD old_protection;
    if (!VirtualProtect(address, sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protection))
        return FALSE;
    InterlockedExchangePointer(address, replacement);
    FlushInstructionCache(GetCurrentProcess(), address, sizeof(void *));
    DWORD ignored;
    VirtualProtect(address, sizeof(void *), old_protection, &ignored);
    return TRUE;
}

static void *AllocateRelayNear(BYTE *call_site)
{
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    uintptr_t granularity = (uintptr_t)system_info.dwAllocationGranularity;

    // Start after the mapped executable image so allocation cannot collide
    // with any of the game's sections. Fall back to a conservative image size
    // if the in-memory PE headers are unexpectedly unavailable.
    uintptr_t image_size = 0x04000000u;
    IMAGE_DOS_HEADER *dos_header = (IMAGE_DOS_HEADER *)g_image_base;
    if (dos_header->e_magic == IMAGE_DOS_SIGNATURE &&
        dos_header->e_lfanew > 0 && dos_header->e_lfanew < 0x100000)
    {
        IMAGE_NT_HEADERS64 *nt_headers =
            (IMAGE_NT_HEADERS64 *)(g_image_base + dos_header->e_lfanew);
        if (nt_headers->Signature == IMAGE_NT_SIGNATURE &&
            nt_headers->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            nt_headers->OptionalHeader.SizeOfImage)
            image_size = nt_headers->OptionalHeader.SizeOfImage;
    }

    uintptr_t start = ((uintptr_t)g_image_base + image_size + granularity - 1) &
                      ~(granularity - 1);
    uintptr_t limit = (uintptr_t)call_site + 0x70000000u;
    for (uintptr_t candidate = start; candidate < limit; candidate += granularity)
    {
        void *relay = VirtualAlloc((void *)candidate, 0x1000,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (relay)
            return relay;
    }
    return NULL;
}

static BOOL PatchHavokTimestepCall(BYTE *call_site, void *expected_target,
                                   float timestep_scale)
{
    if (call_site[0] != 0xE8)
    {
        LogLine("ERROR: Havok call site has unexpected opcode %02X", call_site[0]);
        return FALSE;
    }

    LONG original_displacement;
    CopyMemory(&original_displacement, call_site + 1, sizeof(original_displacement));
    BYTE *actual_target = call_site + 5 + original_displacement;
    if (actual_target != (BYTE *)expected_target)
    {
        LogLine("ERROR: Havok call site targets %p (expected %p)",
                actual_target, expected_target);
        return FALSE;
    }

    g_havok_call_relay = AllocateRelayNear(call_site);
    if (!g_havok_call_relay)
    {
        LogLine("ERROR: could not allocate a nearby Havok timestep relay (Win32=%lu)",
                GetLastError());
        return FALSE;
    }

    // The high-refresh scheduler also advances this separate Havok path once
    // per displayed frame with a fixed 1/60-second value. Scale XMM1 before
    // forwarding to the original function so cloth and rigid bodies retain
    // their native timing while preserving any existing timestep multiplier.
    BYTE relay_code[17] = {
        0xF3, 0x0F, 0x59, 0x0D, 0x05, 0x00, 0x00, 0x00, // mulss xmm1,[rip+5]
        0xE9, 0x00, 0x00, 0x00, 0x00                    // jmp original target
    };
    intptr_t original_jump = (BYTE *)expected_target -
                             ((BYTE *)g_havok_call_relay + 13);
    if (original_jump < INT32_MIN || original_jump > INT32_MAX)
    {
        LogLine("ERROR: original Havok step at %p is out of range from relay %p",
                expected_target, g_havok_call_relay);
        goto release_relay;
    }

    LONG original_relative_jump = (LONG)original_jump;
    CopyMemory(relay_code + 9, &original_relative_jump, sizeof(original_relative_jump));
    CopyMemory(relay_code + 13, &timestep_scale, sizeof(timestep_scale));
    CopyMemory(g_havok_call_relay, relay_code, sizeof(relay_code));
    FlushInstructionCache(GetCurrentProcess(), g_havok_call_relay, sizeof(relay_code));

    intptr_t displacement = (BYTE *)g_havok_call_relay - (call_site + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX)
    {
        LogLine("ERROR: Havok timestep relay at %p is out of range from %p",
                g_havok_call_relay, call_site);
        goto release_relay;
    }

    LONG relative_displacement = (LONG)displacement;
    DWORD old_protection;
    if (!VirtualProtect(call_site, 5, PAGE_EXECUTE_READWRITE, &old_protection))
    {
        LogLine("ERROR: could not make the Havok call site writable (Win32=%lu)",
                GetLastError());
        goto release_relay;
    }
    CopyMemory(call_site + 1, &relative_displacement, sizeof(relative_displacement));
    FlushInstructionCache(GetCurrentProcess(), call_site, 5);
    DWORD ignored;
    VirtualProtect(call_site, 5, old_protection, &ignored);
    LogLine("DSHR Havok timestep correction active: call=%p relay=%p original=%p scale=%.9f",
            call_site, g_havok_call_relay, expected_target, timestep_scale);
    return TRUE;

release_relay:
    VirtualFree(g_havok_call_relay, 0, MEM_RELEASE);
    g_havok_call_relay = NULL;
    return FALSE;
}


static BOOL WINAPI HookQueryPerformanceCounter(LARGE_INTEGER *counter)
{
    BOOL result = g_query_performance_counter(counter);
    void *caller = __builtin_return_address(0);
    if (!result || g_target_fps <= 60 ||
        (caller != g_image_base + PACER_FIRST_CHECK_RETURN_RVA &&
         caller != g_image_base + PACER_LOOP_CHECK_RETURN_RVA))
        return result;

    LONG64 real_counter = counter->QuadPart;
    LONG64 anchor = InterlockedCompareExchange64(&g_pacer_anchor, real_counter, 0);
    if (!anchor)
        anchor = real_counter;

    // The shipping loop evaluates elapsed_time * 60. Scaling only its elapsed
    // clock by target/60 is equivalent to changing that private pacing rate.
    counter->QuadPart = anchor + (real_counter - anchor) * (LONG64)g_target_fps / 60;

    if (InterlockedCompareExchange(&g_activation_logged, 1, 0) == 0)
        LogLine("DSHR frame pacer active: targetFPS=%u", g_target_fps);
    return result;
}

static void HookSimulationStep(void *step, float frame_time)
{
    // The render scheduler calls this once per displayed frame. Retail always
    // supplies 1/60 second, including in the dormant 140-FPS scheduler, which
    // makes every time-based subsystem run at 140/60 speed. Preserve any
    // pause/slowdown multiplier in the supplied value while changing its base
    // duration to one target-refresh interval.
    float corrected_frame_time = frame_time * 60.0f / (float)g_target_fps;
    if (InterlockedCompareExchange(&g_simulation_logged, 1, 0) == 0)
        LogLine("DSHR simulation timestep active: incoming=%.9f corrected=%.9f targetFPS=%u",
                frame_time, corrected_frame_time, g_target_fps);

    // This executable rejects .text changes during its protected startup path.
    // Apply the validated direct-call patch from the live simulation thread
    // after startup has settled, rather than racing that protection here.
    if (GetTickCount64() - g_install_time_ms >= 5000 &&
        InterlockedCompareExchange(&g_havok_patch_state, 1, 0) == 0)
    {
        BOOL installed = PatchHavokTimestepCall(g_image_base + HAVOK_STEP_CALL_RVA,
                                                g_image_base + HAVOK_STEP_RVA,
                                                60.0f / (float)g_target_fps);
        InterlockedExchange(&g_havok_patch_state, installed ? 2 : -1);
    }
    g_simulation_step(step, corrected_frame_time);
}

static void HookFrpgFxUpdate(void *manager, float frame_time)
{
    // The FRPG FX manager advances the core particle engine once per rendered
    // frame but the dormant high-refresh scheduler still supplies 1/60 second.
    // Scale that real FX delta before it reaches particles, emitters and their
    // lifetime counters.
    float corrected_frame_time = frame_time * 60.0f / (float)g_target_fps;
    if (InterlockedCompareExchange(&g_frpg_fx_logged, 1, 0) == 0)
        LogLine("DSHR FX timestep active: incoming=%.9f corrected=%.9f caller=%p",
                frame_time, corrected_frame_time, __builtin_return_address(0));
    g_frpg_fx_update(manager, corrected_frame_time);
}

static BOOL TakeRemoTimelineTick(void)
{
    for (;;)
    {
        LONG previous = g_remo_tick_phase;
        LONG accumulated = previous + 60;
        BOOL run_tick = accumulated >= (LONG)g_target_fps;
        LONG next = run_tick ? accumulated - (LONG)g_target_fps : accumulated;
        if (InterlockedCompareExchange(&g_remo_tick_phase, next, previous) == previous)
            return run_tick;
    }
}

static void HookRemoActiveState(void *step, float frame_time, void *task_item)
{
    // Remo (the in-engine cinematic system) ignores the scheduler's float
    // timestep and advances its authored timeline once per callback. The
    // dormant high-refresh scheduler therefore makes cinematics run at
    // targetFPS/60 speed. Keep the renderer at the configured refresh rate,
    // but phase-distribute exactly 60 native Remo updates across each target
    // second. A gap marks a new cinematic and primes its first update.
    ULONGLONG now = GetTickCount64();
    if (!g_remo_last_input_ms || now - g_remo_last_input_ms > 250)
        InterlockedExchange(&g_remo_tick_phase, (LONG)g_target_fps - 60);
    g_remo_last_input_ms = now;

    if (!TakeRemoTimelineTick())
        return;

    if (InterlockedCompareExchange(&g_remo_logged, 1, 0) == 0)
        LogLine("DSHR Remo timeline active: nativeRate=60 targetFPS=%u incoming=%.9f",
                g_target_fps, frame_time);
    g_remo_active_state(step, frame_time, task_item);
}

static DWORD WINAPI InstallHook(void *unused)
{
    (void)unused;
    BuildSiblingPath(g_log_path, "DSHRHook.log");
    char ini_path[MAX_PATH];
    BuildSiblingPath(ini_path, "DSHRHook.ini");
    g_target_fps = (UINT)GetPrivateProfileIntA("DSHR", "TargetFPS", 140, ini_path);
    if (g_target_fps < 61 || g_target_fps > 1000)
    {
        LogLine("ERROR: TargetFPS must be between 61 and 1000; found %u", g_target_fps);
        return 1;
    }

    g_image_base = (BYTE *)GetModuleHandleA(NULL);
    g_install_time_ms = GetTickCount64();
    void **qpc_iat = (void **)(g_image_base + QPC_IAT_RVA);
    g_query_performance_counter = (QueryPerformanceCounterFn)*qpc_iat;
    if (!g_query_performance_counter || !PatchPointer(qpc_iat, HookQueryPerformanceCounter))
    {
        LogLine("ERROR: could not install the frame-pacer clock hook (Win32=%lu)", GetLastError());
        return 2;
    }

    void **simulation_vtable_entry = (void **)(g_image_base + SIMULATION_STEP_VTABLE_ENTRY_RVA);
    g_simulation_step = (SimulationStepFn)*simulation_vtable_entry;
    if (g_simulation_step != (SimulationStepFn)(g_image_base + SIMULATION_STEP_RVA))
    {
        LogLine("ERROR: simulation vtable entry has unexpected value %p (expected %p)",
                g_simulation_step, g_image_base + SIMULATION_STEP_RVA);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 3;
    }
    if (!PatchPointer(simulation_vtable_entry, HookSimulationStep))
    {
        LogLine("ERROR: could not install the simulation timestep hook (Win32=%lu)", GetLastError());
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 4;
    }

    void **frpg_fx_update_vtable_entry =
        (void **)(g_image_base + FRPG_FX_UPDATE_VTABLE_ENTRY_RVA);
    g_frpg_fx_update = (FrpgFxUpdateFn)*frpg_fx_update_vtable_entry;
    if (g_frpg_fx_update != (FrpgFxUpdateFn)(g_image_base + FRPG_FX_UPDATE_RVA))
    {
        LogLine("ERROR: FX update vtable entry has unexpected value %p (expected %p)",
                g_frpg_fx_update, g_image_base + FRPG_FX_UPDATE_RVA);
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 5;
    }
    if (!PatchPointer(frpg_fx_update_vtable_entry, HookFrpgFxUpdate))
    {
        LogLine("ERROR: could not install the FX timestep hook (Win32=%lu)", GetLastError());
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 6;
    }

    void **remo_active_state_entry = (void **)(g_image_base + REMO_ACTIVE_STATE_ENTRY_RVA);
    // This callback table is populated by a game-side static initializer after
    // our DLL is injected. Wait on the hook worker thread until that initializer
    // publishes the supported function instead of racing it or treating its
    // initial null slot as an unsupported executable.
    for (DWORD waited_ms = 0; waited_ms < 30000; waited_ms += 10)
    {
        g_remo_active_state = (RemoActiveStateFn)*remo_active_state_entry;
        if (g_remo_active_state)
            break;
        Sleep(10);
    }
    if (g_remo_active_state != (RemoActiveStateFn)(g_image_base + REMO_ACTIVE_STATE_RVA))
    {
        LogLine("ERROR: Remo active-state entry has unexpected value %p (expected %p)",
                g_remo_active_state, g_image_base + REMO_ACTIVE_STATE_RVA);
        PatchPointer(frpg_fx_update_vtable_entry, g_frpg_fx_update);
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 7;
    }
    if (!PatchPointer(remo_active_state_entry, HookRemoActiveState))
    {
        LogLine("ERROR: could not install the Remo timeline hook (Win32=%lu)", GetLastError());
        PatchPointer(frpg_fx_update_vtable_entry, g_frpg_fx_update);
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 8;
    }

    LogLine("DSHR render, simulation, FX, and Remo hooks installed; Havok timing correction armed: targetFPS=%u timestepScale=%.9f",
            g_target_fps, 60.0f / (float)g_target_fps);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(NULL, 0, InstallHook, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
