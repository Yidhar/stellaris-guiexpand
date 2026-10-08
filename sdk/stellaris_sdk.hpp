// SUBSET of the generated Stellaris SDK header, written by tools/extract_sdk.py. DO NOT EDIT.
// The full header is produced from the installed stellaris.exe by tools/sdk_dumper (Stellaris MCP
// repository); this file keeps only what src/ uses. Values are RVAs / offsets for the stellaris.exe
// whose PE TimeDateStamp is sdk::kExeTimestamp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sdk {
inline constexpr uint32_t kExeTimestamp = 0x6ABEAA3F;  // PE TimeDateStamp this SDK was dumped from
inline constexpr uintptr_t kRvaEngineAlloc = 0x2021868;

struct CmdSpec {
    const char* name;
    uint32_t token;        // returned by vtable slot 10 (`mov eax, token; ret`)
    uintptr_t vtable_rva;
    uintptr_t factory_rva; // allocates kSize bytes and writes engine defaults
    std::size_t size;
};

namespace glob {
    inline constexpr uintptr_t CConsole_pInstance = 0x3159310;  // CConsole::_pInstance  score=anchor live=anchor
    inline constexpr uintptr_t CStrategicResourceDatabase_pInstance = 0x3152B68;  // CStrategicResourceDatabase::_pInstance  score=anchor live=anchor
    inline constexpr uintptr_t GImAllocatorAllocFunc = 0x27FD320;  // GImAllocatorAllocFunc  score=anchor live=anchor
    inline constexpr uintptr_t GImAllocatorFreeFunc = 0x27FD328;  // GImAllocatorFreeFunc  score=anchor live=anchor
    inline constexpr uintptr_t GImAllocatorUserData = 0x28E2D68;  // GImAllocatorUserData  score=anchor live=anchor
    inline constexpr uintptr_t GImGui = 0x28E2D58;  // GImGui  score=anchor live=anchor
    inline constexpr uintptr_t TGameDatabase_CButtonEffectDatabase_pInstance = 0x31125C0;  // TGameDatabase<CButtonEffectDatabase>::_pInstance  score=anchor live=anchor
    inline constexpr uintptr_t g_CurrentGameState = 0x3114700;  // g_CurrentGameState  score=0.771 live=static
    inline constexpr uintptr_t g_CurrentInGameIdler = 0x3114E68;  // g_CurrentInGameIdler  score=0.773 live=static
}  // namespace glob

namespace db {
    inline constexpr uintptr_t CCountry = 0x3114C38;
}  // namespace db

namespace rt {
    inline constexpr std::ptrdiff_t CCountry_id = 0x20;
    inline constexpr std::ptrdiff_t CGameState_date_hours = 0xC0;
    inline constexpr std::ptrdiff_t CInGameIdler_paused = 0x594;
    inline constexpr std::ptrdiff_t CInGameIdler_speed = 0x590;
    inline constexpr std::ptrdiff_t ImGuiContext_io_MetricsActiveAllocations = 0x3B0;
    inline constexpr std::ptrdiff_t ImGuiContext_sizeof = 0x3F70;
    inline constexpr std::ptrdiff_t ImGuiIO_BackendPlatformUserData = 0xE0;
    inline constexpr std::ptrdiff_t ImGuiIO_ImeWindowHandle = 0x118;
}  // namespace rt

namespace fn {
    inline constexpr uintptr_t CConsole_RunCommandNow = 0x1B13120;
    inline constexpr uintptr_t CGameState_HandleTurnTick = 0x251800;
    inline constexpr uintptr_t CInGameIdler_SetGameSpeed = 0x934C50;
    inline constexpr uintptr_t CInGameIdler_SetPaused = 0x935340;
    inline constexpr uintptr_t CPersistentName_BuildString = 0x33B960;
    inline constexpr uintptr_t CStrategicResource_GetMaximumForCountry = 0x3AE1A0;
    inline constexpr uintptr_t CString_Free = 0x15BE90;
    inline constexpr uintptr_t GetPlayerCountry = 0x14AAD10;
    inline constexpr uintptr_t ImGui_NewFrame = 0x1E9D240;
    inline constexpr uintptr_t NImGuiWrapper_ImGuiInit = 0x1B11090;
    inline constexpr uintptr_t PdxLocalize = 0x16D580;
    inline constexpr uintptr_t PostCommand = 0x5F88A0;
}  // namespace fn

namespace cmd {
namespace execute_button_effect {  // CExecuteButtonEffectCommand (token-name)
    inline constexpr uint32_t kToken = 0x36C4;
    inline constexpr uintptr_t kVtableRva = 0x23BE9E0;
    inline constexpr uintptr_t kFactoryRva = 0xA9B7C0;
    inline constexpr std::size_t kSize = 0x198;
    inline constexpr CmdSpec kSpec{"execute_button_effect", kToken, kVtableRva, kFactoryRva, kSize};
    inline constexpr std::ptrdiff_t scope = 0x20;  // tok 0x2c8d persistent
    inline constexpr std::ptrdiff_t effect = 0x190;  // tok 0x59 ptr (object pointer; serialized as its key string) (ptr; value at +0x20)
}
}  // namespace cmd

namespace ent {
namespace CCountry {
    inline constexpr std::ptrdiff_t name = 0x1460;  // tok 0x1b persistent
    inline constexpr std::ptrdiff_t budget = 0x1C80;  // tok 0x30e0 persistent
    inline constexpr std::ptrdiff_t owned_planets = 0x26B8;  // tok 0x315a ref_array<TPdxRef<CColony>>
    inline constexpr std::ptrdiff_t type = 0x2A68;  // tok 0xe1 ptr (object pointer; serialized as its key string) (ptr; value at +0x20)
    inline constexpr std::ptrdiff_t military_power = 0x2F30;  // tok 0x3669 ptr:fixed_point
    inline constexpr std::ptrdiff_t tech_power = 0x2F48;  // tok 0x2a9f ptr:fixed_point
    inline constexpr std::ptrdiff_t economy_power = 0x2F50;  // tok 0x2a9e ptr:fixed_point
    inline constexpr std::ptrdiff_t empire_size = 0x2F78;  // tok 0x2b89 i32
    inline constexpr std::ptrdiff_t num_sapient_pops = 0x2F7C;  // tok 0x3d43 i32
}
}  // namespace ent

}  // namespace sdk
