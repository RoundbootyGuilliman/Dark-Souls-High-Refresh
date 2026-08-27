#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

// This source is built as dinput8.dll. Dark Souls Remastered imports
// DirectInput8Create, so the game loads this proxy normally; the export below
// forwards that call to the real system DLL while DSHR runs in-process.

#define DSHR_VERSION "1.4"

// Both supported executables report game patch 1.03, but the November 2022
// Steam security update was separately linked and moved internal functions,
// callback tables, imports and scheduler vtables. Keep a complete address set
// for each build and select it from immutable PE metadata before touching any
// process memory.
typedef struct BuildProfile
{
    const char *name;
    DWORD timestamp;
    DWORD image_size;
    DWORD checksum;
    uintptr_t qpc_iat_rva;
    uintptr_t pacer_first_check_return_rva;
    uintptr_t pacer_loop_check_return_rva;
    uintptr_t simulation_step_vtable_entry_rva;
    uintptr_t simulation_step_rva;
    uintptr_t frpg_fx_update_vtable_entry_rva;
    uintptr_t frpg_fx_update_rva;
    uintptr_t remo_active_state_entry_rva;
    uintptr_t remo_active_state_rva;
    uintptr_t menu_action_query_rva;
    uintptr_t input_press_update_vtable_entry_rva;
    uintptr_t input_press_update_rva;
    uintptr_t input_press_alt_update_vtable_entry_rva;
    uintptr_t input_press_alt_update_rva;
    uintptr_t input_press_settings_lookup_rva;
    uintptr_t input_press_alt_settings_lookup_rva;
    uintptr_t input_repeat_update_vtable_entry_rva;
    uintptr_t input_repeat_update_rva;
    uintptr_t input_repeat_alt_update_vtable_entry_rva;
    uintptr_t input_repeat_alt_update_rva;
    uintptr_t input_repeat_settings_lookup_rva;
    uintptr_t input_repeat_alt_settings_lookup_rva;
    uintptr_t havok_step_call_rva;
    uintptr_t havok_step_rva;
    uintptr_t flipper_60_vtable_rva;
    uintptr_t flipper_60_destructor_rva;
    uintptr_t flipper_140_vtable_rva;
    uintptr_t flipper_140_destructor_rva;
} BuildProfile;

static const BuildProfile g_build_profiles[] = {
    {
        "2018 1.03 (F0ECFBE20D780751248DFB2A9759D6215E246676)",
        0x5B3E2C36, 0x03817800, 0x00000000,
        0x20B5120, 0xCDF898, 0xCDF8CE,
        0x1AC54A8, 0x24DDD0,
        0x13678F8, 0x4F6380,
        0x1D0DB08, 0x28EAE0,
        0xED2B70,
        0x14D73D8, 0xE154C0,
        0x14D73E0, 0xE15500,
        0x19CC80, 0xE14B60,
        0x14D70D0, 0xE14790,
        0x14D70D8, 0xE147D0,
        0x19CDC0, 0xE13FA0,
        0x15BD5E, 0x2A18B0,
        0x12A7248, 0x0BB8A0,
        0x12A72A0, 0x0BB9E0,
    },
    {
        "2022 Steam 1.03 (9150CC63C617332ED3C2C66E7566ED67E3292DA0)",
        0x6344CA56, 0x0319B000, 0x02FF5493,
        0x2017344, 0xCE3478, 0xCE34AE,
        0x1A294B8, 0x24F6B0,
        0x136B618, 0x4F7450,
        0x1C71AF8, 0x290400,
        0xED6750,
        0x14DB0B0, 0xE190A0,
        0x14DB0B8, 0xE190E0,
        0x19E570, 0xE18740,
        0x14DADA8, 0xE18370,
        0x14DADB0, 0xE183B0,
        0x19E6B0, 0xE17B80,
        0x15D58E, 0x2A31D0,
        0x12AB268, 0x0BB480,
        0x12AB2C0, 0x0BB5C0,
    },
};


typedef BOOL (WINAPI *QueryPerformanceCounterFn)(LARGE_INTEGER *);
typedef void (*SimulationStepFn)(void *step, float frame_time);
typedef void (*FrpgFxUpdateFn)(void *manager, float frame_time);
typedef void (*RemoActiveStateFn)(void *step, float frame_time, void *task_item);
typedef BYTE (*MenuActionQueryFn)(void *menu, int action);
typedef void (*InputRepeatUpdateFn)(void *detector, void *input_state,
                                    void *entries_begin, void *settings,
                                    float frame_time);
typedef void *(*InputRepeatSettingsLookupFn)(void *settings, int index);
typedef InputRepeatUpdateFn InputPressUpdateFn;
typedef InputRepeatSettingsLookupFn InputPressSettingsLookupFn;
typedef HRESULT (WINAPI *DirectInput8CreateFn)(HINSTANCE instance, DWORD version,
                                               const GUID *interface_id,
                                               void **output, void *outer_unknown);
typedef HRESULT (WINAPI *DirectInputCreateDeviceFn)(void *direct_input,
                                                    const GUID *device_guid,
                                                    void **output,
                                                    void *outer_unknown);
typedef HRESULT (WINAPI *DirectInputGetDeviceStateFn)(void *device, DWORD size,
                                                      void *state);
typedef HRESULT (WINAPI *DirectInputGetDeviceDataFn)(void *device, DWORD object_size,
                                                     void *objects, DWORD *count,
                                                     DWORD flags);

#define MAX_DIRECT_INPUT_INTERFACES 4
#define MAX_DIRECT_INPUT_MOUSE_VTABLES 4
#define MAX_DIRECT_INPUT_MOUSE_DEVICES 32
typedef struct DirectInputInterfaceHook
{
    void **vtable;
    DirectInputCreateDeviceFn create_device;
} DirectInputInterfaceHook;

typedef struct DirectInputMouseVtableHook
{
    void **vtable;
    DirectInputGetDeviceStateFn get_device_state;
    DirectInputGetDeviceDataFn get_device_data;
} DirectInputMouseVtableHook;

typedef struct DirectInputMouseDevice
{
    void *device;
    LONG64 x_remainder;
    LONG64 y_remainder;
} DirectInputMouseDevice;

// Every action used by the supported builds is below 256. Keeping the tracking
// table to that verified range also prevents malformed settings from indexing
// beyond the game's action bitset.
#define MAX_INPUT_ACTIONS 256
#define MAX_INPUT_PULSE_SOURCES 128
#define MAX_INPUT_QUERY_CONSUMERS 4096

typedef struct InputPulseSource
{
    void *detector;
    BYTE kind;
    DWORD active_bits[MAX_INPUT_ACTIONS / 32];
} InputPulseSource;

typedef struct InputQueryConsumer
{
    void *menu;
    LONG64 generation;
} InputQueryConsumer;

static HMODULE g_module;
static BYTE *g_image_base;
static const BuildProfile *g_build_profile;
static QueryPerformanceCounterFn g_query_performance_counter;
static SimulationStepFn g_simulation_step;
static FrpgFxUpdateFn g_frpg_fx_update;
static RemoActiveStateFn g_remo_active_state;
static MenuActionQueryFn g_menu_action_query;
static InputPressUpdateFn g_input_press_update;
static InputPressUpdateFn g_input_press_alt_update;
static InputPressSettingsLookupFn g_input_press_settings_lookup;
static InputPressSettingsLookupFn g_input_press_alt_settings_lookup;
static InputRepeatUpdateFn g_input_repeat_update;
static InputRepeatUpdateFn g_input_repeat_alt_update;
static InputRepeatSettingsLookupFn g_input_repeat_settings_lookup;
static InputRepeatSettingsLookupFn g_input_repeat_alt_settings_lookup;
static DirectInput8CreateFn g_direct_input8_create;
static DirectInputInterfaceHook g_direct_input_interfaces[MAX_DIRECT_INPUT_INTERFACES];
static DirectInputMouseVtableHook
    g_direct_input_mouse_vtables[MAX_DIRECT_INPUT_MOUSE_VTABLES];
static DirectInputMouseDevice
    g_direct_input_mouse_devices[MAX_DIRECT_INPUT_MOUSE_DEVICES];
static InputPulseSource g_input_pulse_sources[MAX_INPUT_PULSE_SOURCES];
static InputQueryConsumer g_input_query_consumers[MAX_INPUT_QUERY_CONSUMERS];
static volatile LONG64 g_input_action_generation[MAX_INPUT_ACTIONS];
static volatile LONG64 g_input_action_pulse_ms[MAX_INPUT_ACTIONS];
static HMODULE g_system_dinput8;
static INIT_ONCE g_dinput8_once = INIT_ONCE_STATIC_INIT;
static void *g_havok_call_relay;
static void *g_menu_action_query_trampoline;
static UINT g_target_fps = 140;
static volatile LONG64 g_pacer_anchor;
static volatile LONG g_activation_logged;
static volatile LONG g_simulation_logged;
static volatile LONG g_frpg_fx_logged;
static volatile LONG g_remo_logged;
static volatile LONG g_input_repeat_logged;
static volatile LONG g_mouse_logged;
static volatile LONG g_havok_patch_state;
static volatile LONG g_menu_action_query_patch_state;
static volatile LONG64 g_input_pulse_generation;
static volatile LONG64 g_input_pulse_batch_ms;
static volatile LONG g_input_pulse_lock;
static volatile LONG g_input_query_consumer_lock;
static volatile LONG g_scheduler_active;
static volatile LONG g_is_game_process;
static volatile LONG g_direct_input_interface_lock;
static volatile LONG g_direct_input_mouse_lock;
static ULONGLONG g_install_time_ms;
static char g_log_path[MAX_PATH];


static BOOL IsDarkSoulsRemasteredProcess(void)
{
    char path[MAX_PATH];
    if (!GetModuleFileNameA(NULL, path, MAX_PATH))
        return FALSE;

    const char *name = path;
    for (const char *scan = path; *scan; ++scan)
        if (*scan == '\\' || *scan == '/') name = scan + 1;
    if (lstrcmpiA(name, "DarkSoulsRemastered.exe") != 0)
        return FALSE;

    BYTE *image_base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos_header = (IMAGE_DOS_HEADER *)image_base;
    if (!image_base || dos_header->e_magic != IMAGE_DOS_SIGNATURE ||
        dos_header->e_lfanew <= 0 || dos_header->e_lfanew >= 0x100000)
        return FALSE;

    IMAGE_NT_HEADERS64 *nt_headers =
        (IMAGE_NT_HEADERS64 *)(image_base + dos_header->e_lfanew);
    return nt_headers->Signature == IMAGE_NT_SIGNATURE &&
           nt_headers->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           nt_headers->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
           nt_headers->OptionalHeader.SizeOfImage >= 0x01000000;
}

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

static IMAGE_NT_HEADERS64 *GetImageHeaders(void)
{
    if (!g_image_base)
        return NULL;
    IMAGE_DOS_HEADER *dos_header = (IMAGE_DOS_HEADER *)g_image_base;
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE ||
        dos_header->e_lfanew <= 0 || dos_header->e_lfanew >= 0x100000)
        return NULL;
    IMAGE_NT_HEADERS64 *nt_headers =
        (IMAGE_NT_HEADERS64 *)(g_image_base + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE ||
        nt_headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return NULL;
    return nt_headers;
}

static const BuildProfile *SelectBuildProfile(IMAGE_NT_HEADERS64 *nt_headers)
{
    for (size_t index = 0; index < sizeof(g_build_profiles) / sizeof(g_build_profiles[0]);
         ++index)
    {
        const BuildProfile *profile = &g_build_profiles[index];
        if (nt_headers->FileHeader.TimeDateStamp == profile->timestamp &&
            nt_headers->OptionalHeader.SizeOfImage == profile->image_size &&
            nt_headers->OptionalHeader.CheckSum == profile->checksum)
            return profile;
    }
    return NULL;
}

static BOOL IsProfileRvaValid(uintptr_t rva, size_t size)
{
    return g_build_profile && rva <= g_build_profile->image_size &&
           size <= g_build_profile->image_size - rva;
}

static BOOL IsExecutableAddress(const void *address)
{
    MEMORY_BASIC_INFORMATION information;
    if (!VirtualQuery(address, &information, sizeof(information)) ||
        information.State != MEM_COMMIT || (information.Protect & PAGE_GUARD) != 0)
        return FALSE;
    DWORD protection = information.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

static BOOL ValidatePacerCall(uintptr_t return_rva, void **qpc_iat)
{
    BYTE *return_address = g_image_base + return_rva;
    BYTE *call = return_address - 6;
    if (call[0] != 0xFF || call[1] != 0x15)
    {
        LogLine("ERROR: frame-pacer call before RVA %p has unexpected bytes %02X %02X",
                (void *)return_rva, call[0], call[1]);
        return FALSE;
    }

    LONG displacement;
    CopyMemory(&displacement, call + 2, sizeof(displacement));
    void **actual_iat = (void **)(return_address + displacement);
    if (actual_iat != qpc_iat)
    {
        LogLine("ERROR: frame-pacer call before RVA %p uses IAT %p (expected %p)",
                (void *)return_rva, actual_iat, qpc_iat);
        return FALSE;
    }
    return TRUE;
}

static BOOL ValidateBuildAddresses(void)
{
    const BuildProfile *profile = g_build_profile;
    const uintptr_t rvas[] = {
        profile->qpc_iat_rva,
        profile->pacer_first_check_return_rva,
        profile->pacer_loop_check_return_rva,
        profile->simulation_step_vtable_entry_rva,
        profile->simulation_step_rva,
        profile->frpg_fx_update_vtable_entry_rva,
        profile->frpg_fx_update_rva,
        profile->remo_active_state_entry_rva,
        profile->remo_active_state_rva,
        profile->menu_action_query_rva,
        profile->input_press_update_vtable_entry_rva,
        profile->input_press_update_rva,
        profile->input_press_alt_update_vtable_entry_rva,
        profile->input_press_alt_update_rva,
        profile->input_press_settings_lookup_rva,
        profile->input_press_alt_settings_lookup_rva,
        profile->input_repeat_update_vtable_entry_rva,
        profile->input_repeat_update_rva,
        profile->input_repeat_alt_update_vtable_entry_rva,
        profile->input_repeat_alt_update_rva,
        profile->input_repeat_settings_lookup_rva,
        profile->input_repeat_alt_settings_lookup_rva,
        profile->havok_step_call_rva,
        profile->havok_step_rva,
        profile->flipper_60_vtable_rva,
        profile->flipper_60_destructor_rva,
        profile->flipper_140_vtable_rva,
        profile->flipper_140_destructor_rva,
    };
    for (size_t index = 0; index < sizeof(rvas) / sizeof(rvas[0]); ++index)
    {
        if (!IsProfileRvaValid(rvas[index], sizeof(void *)))
        {
            LogLine("ERROR: build profile contains out-of-range RVA %p", (void *)rvas[index]);
            return FALSE;
        }
    }

    void **qpc_iat = (void **)(g_image_base + profile->qpc_iat_rva);
    if (!*qpc_iat || !IsExecutableAddress(*qpc_iat) ||
        !ValidatePacerCall(profile->pacer_first_check_return_rva, qpc_iat) ||
        !ValidatePacerCall(profile->pacer_loop_check_return_rva, qpc_iat))
    {
        LogLine("ERROR: frame-pacer import validation failed");
        return FALSE;
    }

    void *simulation_step = *(void **)(g_image_base +
                                        profile->simulation_step_vtable_entry_rva);
    if (simulation_step != g_image_base + profile->simulation_step_rva)
    {
        LogLine("ERROR: simulation vtable entry has unexpected value %p (expected %p)",
                simulation_step, g_image_base + profile->simulation_step_rva);
        return FALSE;
    }

    void *fx_update = *(void **)(g_image_base + profile->frpg_fx_update_vtable_entry_rva);
    if (fx_update != g_image_base + profile->frpg_fx_update_rva)
    {
        LogLine("ERROR: FX update vtable entry has unexpected value %p (expected %p)",
                fx_update, g_image_base + profile->frpg_fx_update_rva);
        return FALSE;
    }

    void *input_press_update =
        *(void **)(g_image_base + profile->input_press_update_vtable_entry_rva);
    void *input_press_alt_update =
        *(void **)(g_image_base + profile->input_press_alt_update_vtable_entry_rva);
    if (input_press_update != g_image_base + profile->input_press_update_rva ||
        input_press_alt_update != g_image_base + profile->input_press_alt_update_rva)
    {
        LogLine("ERROR: input press vtable validation failed: primary=%p (expected %p), alt=%p (expected %p)",
                input_press_update,
                g_image_base + profile->input_press_update_rva,
                input_press_alt_update,
                g_image_base + profile->input_press_alt_update_rva);
        return FALSE;
    }

    void *input_repeat_update =
        *(void **)(g_image_base + profile->input_repeat_update_vtable_entry_rva);
    void *input_repeat_alt_update =
        *(void **)(g_image_base + profile->input_repeat_alt_update_vtable_entry_rva);
    if (input_repeat_update != g_image_base + profile->input_repeat_update_rva ||
        input_repeat_alt_update != g_image_base + profile->input_repeat_alt_update_rva)
    {
        LogLine("ERROR: input repeat vtable validation failed: primary=%p (expected %p), alt=%p (expected %p)",
                input_repeat_update,
                g_image_base + profile->input_repeat_update_rva,
                input_repeat_alt_update,
                g_image_base + profile->input_repeat_alt_update_rva);
        return FALSE;
    }

    static const BYTE action_query_prologue[15] = {
        0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7,
        0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF
    };
    BYTE *action_query = g_image_base + profile->menu_action_query_rva;
    if (memcmp(action_query, action_query_prologue,
               sizeof(action_query_prologue)) != 0)
    {
        LogLine("ERROR: menu action query has an unexpected prologue");
        return FALSE;
    }

    void *flipper_60_destructor = *(void **)(g_image_base + profile->flipper_60_vtable_rva);
    void *flipper_140_destructor = *(void **)(g_image_base + profile->flipper_140_vtable_rva);
    if (flipper_60_destructor != g_image_base + profile->flipper_60_destructor_rva ||
        flipper_140_destructor != g_image_base + profile->flipper_140_destructor_rva)
    {
        LogLine("ERROR: scheduler vtable validation failed: 60=%p (expected %p), 140=%p (expected %p)",
                flipper_60_destructor,
                g_image_base + profile->flipper_60_destructor_rva,
                flipper_140_destructor,
                g_image_base + profile->flipper_140_destructor_rva);
        return FALSE;
    }

    BYTE *havok_call = g_image_base + profile->havok_step_call_rva;
    if (havok_call[0] != 0xE8)
    {
        LogLine("ERROR: Havok call site has unexpected opcode %02X", havok_call[0]);
        return FALSE;
    }
    LONG havok_displacement;
    CopyMemory(&havok_displacement, havok_call + 1, sizeof(havok_displacement));
    if (havok_call + 5 + havok_displacement != g_image_base + profile->havok_step_rva)
    {
        LogLine("ERROR: Havok call target validation failed");
        return FALSE;
    }

    return TRUE;
}

static BOOL PatchPointer(void **address, void *replacement)
{
    DWORD old_protection;
    if (!VirtualProtect(address, sizeof(void *), PAGE_READWRITE, &old_protection))
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
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (relay)
            return relay;
    }
    return NULL;
}

static void AcquireSpinLock(volatile LONG *lock)
{
    while (InterlockedCompareExchange(lock, 1, 0) != 0)
        Sleep(0);
}

static void ReleaseSpinLock(volatile LONG *lock)
{
    InterlockedExchange(lock, 0);
}

static BYTE HookMenuActionQuery(void *menu, int action)
{
    BYTE active = g_menu_action_query(menu, action);
    if (!active || !menu || action < 0 || action >= MAX_INPUT_ACTIONS ||
        InterlockedCompareExchange(&g_scheduler_active, 0, 0) == 0)
        return active;

    LONG64 generation = InterlockedCompareExchange64(
        &g_input_action_generation[action], 0, 0);
    ULONGLONG pulse_ms = (ULONGLONG)InterlockedCompareExchange64(
        &g_input_action_pulse_ms[action], 0, 0);
    ULONGLONG now = GetTickCount64();
    if (generation == 0 || now - pulse_ms > 250)
        return active;

    uintptr_t hash = (((uintptr_t)menu >> 4) * 0x9E3779B1u) &
                     (MAX_INPUT_QUERY_CONSUMERS - 1);
    InputQueryConsumer *consumer = NULL;
    AcquireSpinLock(&g_input_query_consumer_lock);
    for (size_t probe = 0; probe < MAX_INPUT_QUERY_CONSUMERS; ++probe)
    {
        InputQueryConsumer *candidate =
            &g_input_query_consumers[(hash + probe) &
                                     (MAX_INPUT_QUERY_CONSUMERS - 1)];
        if (!candidate->menu || candidate->menu == menu)
        {
            consumer = candidate;
            if (!candidate->menu)
                candidate->menu = menu;
            break;
        }
    }
    if (!consumer)
    {
        consumer = &g_input_query_consumers[hash];
        consumer->menu = menu;
        consumer->generation = 0;
    }

    BOOL suppress = consumer->generation == generation;
    if (!suppress)
        consumer->generation = generation;
    ReleaseSpinLock(&g_input_query_consumer_lock);

    return suppress ? FALSE : active;
}

static BOOL PatchMenuActionQuery(void)
{
    BYTE *target = g_image_base + g_build_profile->menu_action_query_rva;
    static const BYTE expected_prologue[15] = {
        0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7,
        0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF
    };
    if (memcmp(target, expected_prologue, sizeof(expected_prologue)) != 0)
    {
        LogLine("ERROR: menu action query changed before its hook was installed");
        return FALSE;
    }

    g_menu_action_query_trampoline = VirtualAlloc(
        NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_menu_action_query_trampoline)
    {
        LogLine("ERROR: could not allocate menu action query trampoline (Win32=%lu)",
                GetLastError());
        return FALSE;
    }

    BYTE *trampoline = (BYTE *)g_menu_action_query_trampoline;
    CopyMemory(trampoline, target, sizeof(expected_prologue));
    BYTE jump_back[14] = {
        0xFF, 0x25, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    };
    void *resume = target + sizeof(expected_prologue);
    CopyMemory(jump_back + 6, &resume, sizeof(resume));
    CopyMemory(trampoline + sizeof(expected_prologue), jump_back,
               sizeof(jump_back));

    DWORD trampoline_protection;
    if (!VirtualProtect(trampoline, 0x1000, PAGE_EXECUTE_READ,
                        &trampoline_protection))
    {
        LogLine("ERROR: could not make menu action query trampoline executable (Win32=%lu)",
                GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        g_menu_action_query_trampoline = NULL;
        return FALSE;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline,
                          sizeof(expected_prologue) + sizeof(jump_back));
    g_menu_action_query = (MenuActionQueryFn)trampoline;

    BYTE detour[15] = {
        0xFF, 0x25, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0x90
    };
    void *hook = (void *)HookMenuActionQuery;
    CopyMemory(detour + 6, &hook, sizeof(hook));
    DWORD old_protection;
    if (!VirtualProtect(target, sizeof(detour), PAGE_EXECUTE_READWRITE,
                        &old_protection))
    {
        LogLine("ERROR: could not make menu action query writable (Win32=%lu)",
                GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        g_menu_action_query_trampoline = NULL;
        g_menu_action_query = (MenuActionQueryFn)target;
        return FALSE;
    }
    CopyMemory(target, detour, sizeof(detour));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(detour));
    DWORD ignored;
    VirtualProtect(target, sizeof(detour), old_protection, &ignored);
    LogLine("DSHR all-menu input pulse filter active: query=%p trampoline=%p",
            target, trampoline);
    return TRUE;
}

static void TryInstallMenuInputPatch(void)
{
    if (!g_build_profile || GetTickCount64() - g_install_time_ms < 5000 ||
        InterlockedCompareExchange(&g_menu_action_query_patch_state, 1, 0) != 0)
        return;

    BOOL installed = PatchMenuActionQuery();
    InterlockedExchange(&g_menu_action_query_patch_state,
                        installed ? 2 : -1);
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
    DWORD relay_protection;
    if (!VirtualProtect(g_havok_call_relay, 0x1000, PAGE_EXECUTE_READ, &relay_protection))
    {
        LogLine("ERROR: could not make the Havok timestep relay executable (Win32=%lu)",
                GetLastError());
        goto release_relay;
    }
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
        InterlockedCompareExchange(&g_scheduler_active, 0, 0) == 0 ||
        (caller != g_image_base + g_build_profile->pacer_first_check_return_rva &&
         caller != g_image_base + g_build_profile->pacer_loop_check_return_rva))
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
    if (InterlockedCompareExchange(&g_scheduler_active, 0, 0) == 0)
    {
        g_simulation_step(step, frame_time);
        return;
    }

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
        BOOL installed = PatchHavokTimestepCall(
            g_image_base + g_build_profile->havok_step_call_rva,
            g_image_base + g_build_profile->havok_step_rva,
            60.0f / (float)g_target_fps);
        InterlockedExchange(&g_havok_patch_state, installed ? 2 : -1);
    }
    TryInstallMenuInputPatch();
    g_simulation_step(step, corrected_frame_time);
}

static void HookFrpgFxUpdate(void *manager, float frame_time)
{
    // The simulation callback is dormant on the title screen, but FX already
    // runs there. Install the delayed all-menu patch from this live UI path
    // after the protected startup window has elapsed.
    TryInstallMenuInputPatch();

    if (InterlockedCompareExchange(&g_scheduler_active, 0, 0) == 0)
    {
        g_frpg_fx_update(manager, frame_time);
        return;
    }

    // The FRPG FX manager advances the core particle engine once per rendered
    // frame but the dormant high-refresh scheduler still supplies 1/60 second.
    // Scale that real FX delta before it reaches particles, emitters and their
    // lifetime counters.
    float corrected_frame_time = frame_time * 60.0f / (float)g_target_fps;
    if (InterlockedCompareExchange(&g_frpg_fx_logged, 1, 0) == 0)
        LogLine("DSHR FX timestep active: incoming=%.9f corrected=%.9f",
                frame_time, corrected_frame_time);
    g_frpg_fx_update(manager, corrected_frame_time);
}

static void HookRemoActiveState(void *step, float frame_time, void *task_item)
{
    if (InterlockedCompareExchange(&g_scheduler_active, 0, 0) == 0)
    {
        g_remo_active_state(step, frame_time, task_item);
        return;
    }

    // Remo forwards this value to its timeline player. Scale the delta while
    // still running the complete active-state callback every displayed frame.
    // Skipping callbacks kept playback speed correct, but also skipped camera,
    // transform, fade and resource servicing, which could cause flickering or
    // crashes in more complex cinematics.
    float corrected_frame_time = frame_time * 60.0f / (float)g_target_fps;
    if (InterlockedCompareExchange(&g_remo_logged, 1, 0) == 0)
        LogLine("DSHR Remo timestep active: incoming=%.9f corrected=%.9f targetFPS=%u",
                frame_time, corrected_frame_time, g_target_fps);
    g_remo_active_state(step, corrected_frame_time, task_item);
}

static float CorrectInputRepeatFrameTime(float frame_time)
{
    if (InterlockedCompareExchange(&g_scheduler_active, 0, 0) == 0)
        return frame_time;

    // Some callers already pass the scheduler's 1/targetFPS delta, while the
    // UI path keeps passing the native 1/60 delta on every displayed frame.
    // Correct only the latter so input paths that are already time-based are
    // not scaled twice.
    float native_frame_time = 1.0f / 60.0f;
    float target_frame_time = 1.0f / (float)g_target_fps;
    float native_difference = frame_time - native_frame_time;
    float target_difference = frame_time - target_frame_time;
    if (native_difference < 0.0f) native_difference = -native_difference;
    if (target_difference < 0.0f) target_difference = -target_difference;
    float corrected_frame_time = native_difference < target_difference
                                     ? frame_time * 60.0f / (float)g_target_fps
                                     : frame_time;
    if (InterlockedCompareExchange(&g_input_repeat_logged, 1, 0) == 0)
        LogLine("DSHR input repeat timestep active: incoming=%.9f corrected=%.9f targetFPS=%u",
                frame_time, corrected_frame_time, g_target_fps);
    return corrected_frame_time;
}

static void UpdateInputActionPulses(
    void *detector, void *input_state, void *resolved_settings,
    size_t stride, size_t action_offset, size_t bits_offset, BYTE kind)
{
    if (InterlockedCompareExchange(&g_scheduler_active, 0, 0) == 0 ||
        !detector || !input_state || !resolved_settings)
        return;

    BYTE *entry = *(BYTE **)((BYTE *)resolved_settings + 0x10);
    BYTE *end = *(BYTE **)((BYTE *)resolved_settings + 0x18);
    DWORD *bits = *(DWORD **)((BYTE *)input_state + bits_offset);
    if (!entry || !end || end < entry || (size_t)(end - entry) > 0x1000 ||
        !bits)
        return;

    AcquireSpinLock(&g_input_pulse_lock);
    InputPulseSource *pulse_source = NULL;
    for (size_t index = 0; index < MAX_INPUT_PULSE_SOURCES; ++index)
    {
        InputPulseSource *candidate = &g_input_pulse_sources[index];
        if ((candidate->detector == detector && candidate->kind == kind) ||
            !candidate->detector)
        {
            pulse_source = candidate;
            if (!candidate->detector)
            {
                candidate->detector = detector;
                candidate->kind = kind;
            }
            break;
        }
    }
    if (!pulse_source)
    {
        ReleaseSpinLock(&g_input_pulse_lock);
        return;
    }

    for (; entry + stride <= end; entry += stride)
    {
        int action = *(int *)(entry + action_offset);
        if (action < 0 || action >= MAX_INPUT_ACTIONS)
            continue;
        DWORD mask = (DWORD)1 << (action & 31);
        DWORD *tracked_word = &pulse_source->active_bits[(unsigned)action >> 5];
        BOOL active = (bits[(unsigned)action >> 5] & mask) != 0;
        BOOL previously_active = (*tracked_word & mask) != 0;
        if (active)
            *tracked_word |= mask;
        else
            *tracked_word &= ~mask;
        if (!active || previously_active)
            continue;

        ULONGLONG now = GetTickCount64();
        ULONGLONG batch_ms = (ULONGLONG)InterlockedCompareExchange64(
            &g_input_pulse_batch_ms, 0, 0);
        LONG64 generation = InterlockedCompareExchange64(
            &g_input_pulse_generation, 0, 0);
        if (generation == 0 || now - batch_ms > 40)
        {
            generation = InterlockedIncrement64(&g_input_pulse_generation);
            InterlockedExchange64(&g_input_pulse_batch_ms, (LONG64)now);
        }
        InterlockedExchange64(&g_input_action_generation[action], generation);
        InterlockedExchange64(&g_input_action_pulse_ms[action], (LONG64)now);
    }
    ReleaseSpinLock(&g_input_pulse_lock);
}

static void HookInputPressUpdate(void *detector, void *input_state,
                                 void *entries_begin, void *settings,
                                 float frame_time)
{
    void *resolved_settings = g_input_press_settings_lookup(settings, 0);
    g_input_press_update(detector, input_state, entries_begin, settings, frame_time);
    UpdateInputActionPulses(detector, input_state, resolved_settings,
                            8, 4, 0x198, 0);
}

static void HookInputPressAltUpdate(void *detector, void *input_state,
                                    void *entries_begin, void *settings,
                                    float frame_time)
{
    void *resolved_settings = g_input_press_alt_settings_lookup(settings, 0);
    g_input_press_alt_update(detector, input_state, entries_begin, settings,
                             frame_time);
    UpdateInputActionPulses(detector, input_state, resolved_settings,
                            8, 4, 0x50, 1);
}

static void HookInputRepeatUpdate(void *detector, void *input_state,
                                  void *entries_begin, void *settings,
                                  float frame_time)
{
    void *resolved_settings = g_input_repeat_settings_lookup(settings, 0);
    g_input_repeat_update(detector, input_state, entries_begin, settings,
                          CorrectInputRepeatFrameTime(frame_time));
    UpdateInputActionPulses(detector, input_state, resolved_settings,
                            12, 8, 0x198, 2);
}

static void HookInputRepeatAltUpdate(void *detector, void *input_state,
                                     void *entries_begin, void *settings,
                                     float frame_time)
{
    void *resolved_settings = g_input_repeat_alt_settings_lookup(settings, 0);
    g_input_repeat_alt_update(detector, input_state, entries_begin, settings,
                              CorrectInputRepeatFrameTime(frame_time));
    UpdateInputActionPulses(detector, input_state, resolved_settings,
                            12, 8, 0x50, 3);
}

static BOOL IsWritablePrivateRegion(const MEMORY_BASIC_INFORMATION *information)
{
    DWORD basic_protection = information->Protect & 0xFF;
    return information->State == MEM_COMMIT &&
           information->Type == MEM_PRIVATE &&
           basic_protection == PAGE_READWRITE &&
           (information->Protect & PAGE_GUARD) == 0;
}

static BYTE *SearchSchedulerRegion(const MEMORY_BASIC_INFORMATION *information,
                                   uintptr_t expected_vtable)
{
    uintptr_t base = (uintptr_t)information->BaseAddress;
    uintptr_t end = base + information->RegionSize;
    uintptr_t cursor = (base + 7) & ~(uintptr_t)7;
    if (end < base || end - base < 16)
        return NULL;

    for (; cursor <= end - 16; cursor += 8)
    {
        uintptr_t *candidate = (uintptr_t *)cursor;
        if (candidate[0] == expected_vtable && candidate[1] == 0)
            return (BYTE *)candidate;
    }
    return NULL;
}

static BYTE *FindSchedulerObject(uintptr_t expected_vtable, BOOL preferred_heap_only)
{
    uintptr_t address = 0x10000;
    const uintptr_t limit = 0x100000000ull;
    while (address < limit)
    {
        MEMORY_BASIC_INFORMATION information;
        if (!VirtualQuery((void *)address, &information, sizeof(information)) ||
            !information.RegionSize)
            break;

        uintptr_t next = (uintptr_t)information.BaseAddress + information.RegionSize;
        if (next <= address)
            break;

        if (IsWritablePrivateRegion(&information) &&
            ((preferred_heap_only && information.RegionSize == 0x02001000) ||
             (!preferred_heap_only && information.RegionSize != 0x02001000 &&
              information.RegionSize <= 0x08000000)))
        {
            BYTE *scheduler = SearchSchedulerRegion(&information, expected_vtable);
            if (scheduler)
                return scheduler;
        }
        address = next;
    }
    return NULL;
}

static LONG *FindSchedulerModeField(BYTE *scheduler)
{
    uintptr_t address = 0x10000;
    const uintptr_t limit = 0x100000000ull;
    while (address < limit)
    {
        MEMORY_BASIC_INFORMATION information;
        if (!VirtualQuery((void *)address, &information, sizeof(information)) ||
            !information.RegionSize)
            break;

        uintptr_t base = (uintptr_t)information.BaseAddress;
        uintptr_t end = base + information.RegionSize;
        uintptr_t next = end;
        if (next <= address)
            break;

        if (IsWritablePrivateRegion(&information) &&
            information.RegionSize <= 0x08000000 && end >= base && end - base >= 16)
        {
            uintptr_t cursor = (base + 15) & ~(uintptr_t)7;
            for (; cursor <= end - 8; cursor += 8)
            {
                if (*(BYTE **)cursor == scheduler && *(LONG *)(cursor - 8) == 4)
                    return (LONG *)(cursor - 8);
            }
        }
        address = next;
    }
    return NULL;
}

typedef struct ProcessWindowSearch
{
    DWORD process_id;
    BOOL found;
} ProcessWindowSearch;

static BOOL CALLBACK FindProcessWindow(HWND window, LPARAM parameter)
{
    ProcessWindowSearch *search = (ProcessWindowSearch *)parameter;
    DWORD process_id;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == search->process_id && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == NULL)
    {
        search->found = TRUE;
        return FALSE;
    }
    return TRUE;
}

static BOOL HasVisibleProcessWindow(void)
{
    ProcessWindowSearch search = {GetCurrentProcessId(), FALSE};
    EnumWindows(FindProcessWindow, (LPARAM)&search);
    return search.found;
}

static BOOL IsSystemMouseGuid(const GUID *guid)
{
    static const GUID system_mouse = {
        0x6F1D2B60, 0xD5A0, 0x11CF,
        {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}
    };
    if (!guid)
        return FALSE;
    return guid->Data1 == system_mouse.Data1 &&
           guid->Data2 == system_mouse.Data2 &&
           guid->Data3 == system_mouse.Data3 &&
           guid->Data4[0] == system_mouse.Data4[0] &&
           guid->Data4[1] == system_mouse.Data4[1] &&
           guid->Data4[2] == system_mouse.Data4[2] &&
           guid->Data4[3] == system_mouse.Data4[3] &&
           guid->Data4[4] == system_mouse.Data4[4] &&
           guid->Data4[5] == system_mouse.Data4[5] &&
           guid->Data4[6] == system_mouse.Data4[6] &&
           guid->Data4[7] == system_mouse.Data4[7];
}

static LONG ScaleMouseAxis(LONG value, LONG64 *remainder)
{
    LONG64 numerator = (LONG64)value * (LONG64)g_target_fps + *remainder;
    LONG64 scaled = numerator / 60;
    *remainder = numerator - scaled * 60;
    if (scaled > INT32_MAX)
    {
        *remainder = 0;
        return INT32_MAX;
    }
    if (scaled < INT32_MIN)
    {
        *remainder = 0;
        return INT32_MIN;
    }
    return (LONG)scaled;
}

static HRESULT WINAPI HookDirectInputGetDeviceState(void *device, DWORD size, void *state)
{
    void **vtable = device ? *(void ***)device : NULL;
    DirectInputGetDeviceStateFn get_device_state = NULL;
    DirectInputMouseDevice *mouse = NULL;
    while (InterlockedCompareExchange(&g_direct_input_mouse_lock, 1, 0) != 0)
        Sleep(0);
    for (size_t index = 0; index < MAX_DIRECT_INPUT_MOUSE_VTABLES; ++index)
        if (g_direct_input_mouse_vtables[index].vtable == vtable)
            get_device_state = g_direct_input_mouse_vtables[index].get_device_state;
    for (size_t index = 0; index < MAX_DIRECT_INPUT_MOUSE_DEVICES; ++index)
        if (g_direct_input_mouse_devices[index].device == device)
            mouse = &g_direct_input_mouse_devices[index];
    InterlockedExchange(&g_direct_input_mouse_lock, 0);
    if (!get_device_state)
        return E_FAIL;

    HRESULT result = get_device_state(device, size, state);
    if (SUCCEEDED(result) && state && (size == 16 || size == 20) && mouse &&
        InterlockedCompareExchange(&g_scheduler_active, 0, 0) != 0)
    {
        LONG *axes = (LONG *)state;
        axes[0] = ScaleMouseAxis(axes[0], &mouse->x_remainder);
        axes[1] = ScaleMouseAxis(axes[1], &mouse->y_remainder);
        if (InterlockedCompareExchange(&g_mouse_logged, 1, 0) == 0)
            LogLine("DSHR DirectInput mouse correction active: mode=state scale=%u/60",
                    g_target_fps);
    }
    return result;
}

static HRESULT WINAPI HookDirectInputGetDeviceData(void *device, DWORD object_size,
                                                    void *objects, DWORD *count,
                                                    DWORD flags)
{
    void **vtable = device ? *(void ***)device : NULL;
    DirectInputGetDeviceDataFn get_device_data = NULL;
    DirectInputMouseDevice *mouse = NULL;
    while (InterlockedCompareExchange(&g_direct_input_mouse_lock, 1, 0) != 0)
        Sleep(0);
    for (size_t index = 0; index < MAX_DIRECT_INPUT_MOUSE_VTABLES; ++index)
        if (g_direct_input_mouse_vtables[index].vtable == vtable)
            get_device_data = g_direct_input_mouse_vtables[index].get_device_data;
    for (size_t index = 0; index < MAX_DIRECT_INPUT_MOUSE_DEVICES; ++index)
        if (g_direct_input_mouse_devices[index].device == device)
            mouse = &g_direct_input_mouse_devices[index];
    InterlockedExchange(&g_direct_input_mouse_lock, 0);
    if (!get_device_data)
        return E_FAIL;

    HRESULT result = get_device_data(device, object_size, objects, count, flags);
    if (SUCCEEDED(result) && objects && count && object_size >= 8 &&
        mouse &&
        InterlockedCompareExchange(&g_scheduler_active, 0, 0) != 0)
    {
        BYTE *object = (BYTE *)objects;
        for (DWORD index = 0; index < *count; ++index, object += object_size)
        {
            DWORD offset = *(DWORD *)object;
            LONG *value = (LONG *)(object + sizeof(DWORD));
            if (offset == 0)
                *value = ScaleMouseAxis(*value, &mouse->x_remainder);
            else if (offset == 4)
                *value = ScaleMouseAxis(*value, &mouse->y_remainder);
        }
        if (InterlockedCompareExchange(&g_mouse_logged, 1, 0) == 0)
            LogLine("DSHR DirectInput mouse correction active: mode=buffered scale=%u/60",
                    g_target_fps);
    }
    return result;
}

static BOOL HookMouseDevice(void *device)
{
    if (!device)
        return FALSE;
    void **vtable = *(void ***)device;
    if (!vtable || !IsExecutableAddress(vtable[9]) || !IsExecutableAddress(vtable[10]))
        return FALSE;

    while (InterlockedCompareExchange(&g_direct_input_mouse_lock, 1, 0) != 0)
        Sleep(0);

    DirectInputMouseVtableHook *vtable_hook = NULL;
    DirectInputMouseVtableHook *free_vtable_hook = NULL;
    for (size_t index = 0; index < MAX_DIRECT_INPUT_MOUSE_VTABLES; ++index)
    {
        DirectInputMouseVtableHook *candidate = &g_direct_input_mouse_vtables[index];
        if (candidate->vtable == vtable)
            vtable_hook = candidate;
        else if (!candidate->vtable && !free_vtable_hook)
            free_vtable_hook = candidate;
    }

    if (!vtable_hook)
    {
        if (!free_vtable_hook || vtable[9] == (void *)HookDirectInputGetDeviceState ||
            vtable[10] == (void *)HookDirectInputGetDeviceData)
        {
            InterlockedExchange(&g_direct_input_mouse_lock, 0);
            return FALSE;
        }
        vtable_hook = free_vtable_hook;
        vtable_hook->vtable = vtable;
        vtable_hook->get_device_state = (DirectInputGetDeviceStateFn)vtable[9];
        vtable_hook->get_device_data = (DirectInputGetDeviceDataFn)vtable[10];
        if (!PatchPointer(vtable + 9, HookDirectInputGetDeviceState) ||
            !PatchPointer(vtable + 10, HookDirectInputGetDeviceData))
        {
            if (vtable[9] == (void *)HookDirectInputGetDeviceState)
                PatchPointer(vtable + 9, vtable_hook->get_device_state);
            vtable_hook->vtable = NULL;
            vtable_hook->get_device_state = NULL;
            vtable_hook->get_device_data = NULL;
            InterlockedExchange(&g_direct_input_mouse_lock, 0);
            return FALSE;
        }
    }

    DirectInputMouseDevice *free_device = NULL;
    for (size_t index = 0; index < MAX_DIRECT_INPUT_MOUSE_DEVICES; ++index)
    {
        DirectInputMouseDevice *candidate = &g_direct_input_mouse_devices[index];
        if (candidate->device == device)
        {
            InterlockedExchange(&g_direct_input_mouse_lock, 0);
            return TRUE;
        }
        if (!candidate->device && !free_device)
            free_device = candidate;
    }
    if (!free_device)
    {
        InterlockedExchange(&g_direct_input_mouse_lock, 0);
        return FALSE;
    }
    free_device->device = device;
    free_device->x_remainder = 0;
    free_device->y_remainder = 0;
    InterlockedExchange(&g_direct_input_mouse_lock, 0);
    return TRUE;
}

static HRESULT WINAPI HookDirectInputCreateDevice(void *direct_input,
                                                  const GUID *device_guid,
                                                  void **output,
                                                  void *outer_unknown)
{
    void **vtable = direct_input ? *(void ***)direct_input : NULL;
    DirectInputCreateDeviceFn create_device = NULL;
    while (InterlockedCompareExchange(&g_direct_input_interface_lock, 1, 0) != 0)
        Sleep(0);
    for (size_t index = 0; index < MAX_DIRECT_INPUT_INTERFACES; ++index)
    {
        if (g_direct_input_interfaces[index].vtable == vtable)
        {
            create_device = g_direct_input_interfaces[index].create_device;
            break;
        }
    }
    InterlockedExchange(&g_direct_input_interface_lock, 0);
    if (!create_device)
        return E_FAIL;

    HRESULT result = create_device(direct_input, device_guid, output, outer_unknown);
    if (SUCCEEDED(result) && output && IsSystemMouseGuid(device_guid) &&
        !HookMouseDevice(*output))
        LogLine("ERROR: could not install DirectInput mouse hooks (Win32=%lu)",
                GetLastError());
    return result;
}

static BOOL HookDirectInputInterface(void *direct_input)
{
    if (!direct_input)
        return FALSE;
    void **vtable = *(void ***)direct_input;
    if (!vtable || !IsExecutableAddress(vtable[3]))
        return FALSE;
    while (InterlockedCompareExchange(&g_direct_input_interface_lock, 1, 0) != 0)
        Sleep(0);

    DirectInputInterfaceHook *free_hook = NULL;
    for (size_t index = 0; index < MAX_DIRECT_INPUT_INTERFACES; ++index)
    {
        DirectInputInterfaceHook *hook = &g_direct_input_interfaces[index];
        if (hook->vtable == vtable)
        {
            BOOL installed = vtable[3] == (void *)HookDirectInputCreateDevice;
            InterlockedExchange(&g_direct_input_interface_lock, 0);
            return installed;
        }
        if (!hook->vtable && !free_hook)
            free_hook = hook;
    }

    if (!free_hook || vtable[3] == (void *)HookDirectInputCreateDevice)
    {
        InterlockedExchange(&g_direct_input_interface_lock, 0);
        return FALSE;
    }

    free_hook->vtable = vtable;
    free_hook->create_device = (DirectInputCreateDeviceFn)vtable[3];
    if (!PatchPointer(vtable + 3, HookDirectInputCreateDevice))
    {
        free_hook->vtable = NULL;
        free_hook->create_device = NULL;
        InterlockedExchange(&g_direct_input_interface_lock, 0);
        return FALSE;
    }
    InterlockedExchange(&g_direct_input_interface_lock, 0);
    return TRUE;
}

static BOOL ActivateHighRefreshScheduler(void)
{
    BOOL window_seen = FALSE;
    uintptr_t flipper_60_vtable =
        (uintptr_t)(g_image_base + g_build_profile->flipper_60_vtable_rva);
    void *flipper_140_vtable =
        g_image_base + g_build_profile->flipper_140_vtable_rva;

    for (DWORD waited_ms = 0; waited_ms < 45000; waited_ms += 250)
    {
        if (!window_seen && HasVisibleProcessWindow())
        {
            window_seen = TRUE;
            LogLine("DSHR game window initialized; locating the live 60-FPS scheduler");
        }

        if (window_seen)
        {
            BYTE *scheduler = FindSchedulerObject(flipper_60_vtable, TRUE);
            if (!scheduler)
                scheduler = FindSchedulerObject(flipper_60_vtable, FALSE);
            if (scheduler)
            {
                LONG *mode_field = FindSchedulerModeField(scheduler);
                if (mode_field)
                {
                    InterlockedExchangePointer((void **)scheduler, flipper_140_vtable);
                    InterlockedExchange(mode_field, 5);
                    if (*(void **)scheduler != flipper_140_vtable || *mode_field != 5)
                    {
                        LogLine("ERROR: high-refresh scheduler switch did not verify");
                        return FALSE;
                    }

                    InterlockedExchange(&g_scheduler_active, 1);
                    LogLine("DSHR live scheduler switched at %p; owner mode at %p changed from 4 to 5",
                            scheduler, mode_field);
                    return TRUE;
                }
            }
        }
        Sleep(250);
    }

    LogLine(window_seen
            ? "ERROR: live 60-FPS scheduler was not found within 45 seconds"
            : "ERROR: the game did not create a visible window within 45 seconds");
    return FALSE;
}

static BOOL CALLBACK LoadSystemDInput8(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;

    char path[MAX_PATH];
    UINT length = GetSystemDirectoryA(path, MAX_PATH);
    if (!length || length + sizeof("\\dinput8.dll") > MAX_PATH)
        return FALSE;
    lstrcatA(path, "\\dinput8.dll");

    g_system_dinput8 = LoadLibraryA(path);
    if (!g_system_dinput8)
        return FALSE;
    g_direct_input8_create =
        (DirectInput8CreateFn)GetProcAddress(g_system_dinput8, "DirectInput8Create");
    return g_direct_input8_create != NULL;
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version,
                                                        const GUID *interface_id,
                                                        void **output, void *outer_unknown)
{
    if (!InitOnceExecuteOnce(&g_dinput8_once, LoadSystemDInput8, NULL, NULL) ||
        !g_direct_input8_create)
        return E_FAIL;
    HRESULT result = g_direct_input8_create(instance, version, interface_id,
                                             output, outer_unknown);
    if (SUCCEEDED(result) && output &&
        InterlockedCompareExchange(&g_is_game_process, 0, 0) != 0 &&
        !HookDirectInputInterface(*output))
        LogLine("ERROR: could not install DirectInput interface hook (Win32=%lu)",
                GetLastError());
    return result;
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
    IMAGE_NT_HEADERS64 *nt_headers = GetImageHeaders();
    if (!nt_headers)
    {
        LogLine("ERROR: could not read the DarkSoulsRemastered.exe PE headers");
        return 2;
    }

    g_build_profile = SelectBuildProfile(nt_headers);
    if (!g_build_profile)
    {
        LogLine("ERROR: unsupported DarkSoulsRemastered.exe build: timestamp=%08lX imageSize=%08lX checksum=%08lX",
                nt_headers->FileHeader.TimeDateStamp,
                nt_headers->OptionalHeader.SizeOfImage,
                nt_headers->OptionalHeader.CheckSum);
        return 3;
    }
    LogLine("DSHR v%s recognized %s", DSHR_VERSION, g_build_profile->name);

    if (!ValidateBuildAddresses())
        return 4;

    void **qpc_iat = (void **)(g_image_base + g_build_profile->qpc_iat_rva);
    void **simulation_vtable_entry =
        (void **)(g_image_base + g_build_profile->simulation_step_vtable_entry_rva);
    void **frpg_fx_update_vtable_entry =
        (void **)(g_image_base + g_build_profile->frpg_fx_update_vtable_entry_rva);
    void **remo_active_state_entry =
        (void **)(g_image_base + g_build_profile->remo_active_state_entry_rva);
    void **input_press_update_vtable_entry =
        (void **)(g_image_base +
                  g_build_profile->input_press_update_vtable_entry_rva);
    void **input_press_alt_update_vtable_entry =
        (void **)(g_image_base +
                  g_build_profile->input_press_alt_update_vtable_entry_rva);
    void **input_repeat_update_vtable_entry =
        (void **)(g_image_base +
                  g_build_profile->input_repeat_update_vtable_entry_rva);
    void **input_repeat_alt_update_vtable_entry =
        (void **)(g_image_base +
                  g_build_profile->input_repeat_alt_update_vtable_entry_rva);

    g_query_performance_counter = (QueryPerformanceCounterFn)*qpc_iat;
    g_simulation_step = (SimulationStepFn)*simulation_vtable_entry;
    g_frpg_fx_update = (FrpgFxUpdateFn)*frpg_fx_update_vtable_entry;
    g_menu_action_query =
        (MenuActionQueryFn)(g_image_base +
                            g_build_profile->menu_action_query_rva);
    g_input_press_update = (InputPressUpdateFn)*input_press_update_vtable_entry;
    g_input_press_alt_update =
        (InputPressUpdateFn)*input_press_alt_update_vtable_entry;
    g_input_press_settings_lookup =
        (InputPressSettingsLookupFn)(g_image_base +
                                     g_build_profile->input_press_settings_lookup_rva);
    g_input_press_alt_settings_lookup =
        (InputPressSettingsLookupFn)(g_image_base +
                                     g_build_profile->input_press_alt_settings_lookup_rva);
    g_input_repeat_update = (InputRepeatUpdateFn)*input_repeat_update_vtable_entry;
    g_input_repeat_alt_update =
        (InputRepeatUpdateFn)*input_repeat_alt_update_vtable_entry;
    g_input_repeat_settings_lookup =
        (InputRepeatSettingsLookupFn)(g_image_base +
                                      g_build_profile->input_repeat_settings_lookup_rva);
    g_input_repeat_alt_settings_lookup =
        (InputRepeatSettingsLookupFn)(g_image_base +
                                      g_build_profile->input_repeat_alt_settings_lookup_rva);

    // This callback table is populated by a game-side static initializer after
    // our proxy is loaded. Wait on the hook worker thread until that initializer
    // publishes the supported function instead of racing it or treating its
    // initial null slot as an unsupported executable.
    for (DWORD waited_ms = 0; waited_ms < 30000; waited_ms += 10)
    {
        g_remo_active_state = (RemoActiveStateFn)*remo_active_state_entry;
        if (g_remo_active_state)
            break;
        Sleep(10);
    }
    if (g_remo_active_state !=
        (RemoActiveStateFn)(g_image_base + g_build_profile->remo_active_state_rva))
    {
        LogLine("ERROR: Remo active-state entry has unexpected value %p (expected %p)",
                g_remo_active_state,
                g_image_base + g_build_profile->remo_active_state_rva);
        return 5;
    }

    g_install_time_ms = GetTickCount64();
    if (!PatchPointer(qpc_iat, HookQueryPerformanceCounter))
    {
        LogLine("ERROR: could not install the frame-pacer clock hook (Win32=%lu)",
                GetLastError());
        return 6;
    }
    if (!PatchPointer(simulation_vtable_entry, HookSimulationStep))
    {
        LogLine("ERROR: could not install the simulation timestep hook (Win32=%lu)",
                GetLastError());
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 7;
    }
    if (!PatchPointer(frpg_fx_update_vtable_entry, HookFrpgFxUpdate))
    {
        LogLine("ERROR: could not install the FX timestep hook (Win32=%lu)", GetLastError());
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 8;
    }
    if (!PatchPointer(input_press_update_vtable_entry, HookInputPressUpdate))
    {
        LogLine("ERROR: could not install the input press hook (Win32=%lu)",
                GetLastError());
        PatchPointer(frpg_fx_update_vtable_entry, g_frpg_fx_update);
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 9;
    }
    if (!PatchPointer(input_press_alt_update_vtable_entry, HookInputPressAltUpdate))
    {
        LogLine("ERROR: could not install the alternate input press hook (Win32=%lu)",
                GetLastError());
        PatchPointer(input_press_update_vtable_entry, g_input_press_update);
        PatchPointer(frpg_fx_update_vtable_entry, g_frpg_fx_update);
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 10;
    }
    if (!PatchPointer(input_repeat_update_vtable_entry, HookInputRepeatUpdate))
    {
        LogLine("ERROR: could not install the input repeat timestep hook (Win32=%lu)",
                GetLastError());
        PatchPointer(input_press_alt_update_vtable_entry, g_input_press_alt_update);
        PatchPointer(input_press_update_vtable_entry, g_input_press_update);
        PatchPointer(frpg_fx_update_vtable_entry, g_frpg_fx_update);
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 11;
    }
    if (!PatchPointer(input_repeat_alt_update_vtable_entry, HookInputRepeatAltUpdate))
    {
        LogLine("ERROR: could not install the alternate input repeat timestep hook (Win32=%lu)",
                GetLastError());
        PatchPointer(input_repeat_update_vtable_entry, g_input_repeat_update);
        PatchPointer(input_press_alt_update_vtable_entry, g_input_press_alt_update);
        PatchPointer(input_press_update_vtable_entry, g_input_press_update);
        PatchPointer(frpg_fx_update_vtable_entry, g_frpg_fx_update);
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 12;
    }
    if (!PatchPointer(remo_active_state_entry, HookRemoActiveState))
    {
        LogLine("ERROR: could not install the Remo timeline hook (Win32=%lu)", GetLastError());
        PatchPointer(input_repeat_alt_update_vtable_entry, g_input_repeat_alt_update);
        PatchPointer(input_repeat_update_vtable_entry, g_input_repeat_update);
        PatchPointer(input_press_alt_update_vtable_entry, g_input_press_alt_update);
        PatchPointer(input_press_update_vtable_entry, g_input_press_update);
        PatchPointer(frpg_fx_update_vtable_entry, g_frpg_fx_update);
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 13;
    }
    if (!ActivateHighRefreshScheduler())
    {
        PatchPointer(remo_active_state_entry, g_remo_active_state);
        PatchPointer(input_repeat_alt_update_vtable_entry, g_input_repeat_alt_update);
        PatchPointer(input_repeat_update_vtable_entry, g_input_repeat_update);
        PatchPointer(input_press_alt_update_vtable_entry, g_input_press_alt_update);
        PatchPointer(input_press_update_vtable_entry, g_input_press_update);
        PatchPointer(frpg_fx_update_vtable_entry, g_frpg_fx_update);
        PatchPointer(simulation_vtable_entry, g_simulation_step);
        PatchPointer(qpc_iat, g_query_performance_counter);
        return 14;
    }

    LogLine("DSHR v%s proxy hooks and high-refresh scheduler active; Havok timing correction armed: targetFPS=%u timestepScale=%.9f",
            DSHR_VERSION, g_target_fps, 60.0f / (float)g_target_fps);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (IsDarkSoulsRemasteredProcess())
        {
            InterlockedExchange(&g_is_game_process, 1);
            BuildSiblingPath(g_log_path, "DSHRHook.log");
            HANDLE thread = CreateThread(NULL, 0, InstallHook, NULL, 0, NULL);
            if (thread) CloseHandle(thread);
        }
    }
    return TRUE;
}
