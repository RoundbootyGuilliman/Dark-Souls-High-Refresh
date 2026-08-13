#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

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
static UINT g_target_fps = 140;
static volatile LONG64 g_pacer_anchor;
static volatile LONG g_activation_logged;
static volatile LONG g_simulation_logged;
static volatile LONG g_frpg_fx_logged;
static volatile LONG g_remo_logged;
static volatile LONG g_remo_tick_phase;
static ULONGLONG g_remo_last_input_ms;
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

    LogLine("DSHR render, simulation, FX, and Remo hooks installed: targetFPS=%u timestepScale=%.9f",
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
