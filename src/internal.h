// Declarations shared by the translation units of guidll.dll. Nothing in here is part of the public interface (that is include/guidll/).
//
//   core.cpp         engine access (guarded reads and calls), game snapshot, script effect channel, deferred actions, runtime state, log, config
//   loc.cpp          the game's localisation
//   host_api.cpp     what other plugins see: panel registry, the C drawing table, dispatch with fault isolation, StlGui_GetApi
//   decl_panels.cpp  panels declared by mods in script files (interface/stl_gui/*.txt)
//   deck.cpp         the "Command Deck": status capsule and five-page deck, the reference skin
//   imgui_host.cpp   hooks, fonts, the per-frame entry, start of the engine's ImGui, command file (development)
//   dllmain.cpp      DllMain
#pragma once

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "sdk/stellaris_sdk.hpp"
#include "guidll/stellaris_gui_api.h"

#ifndef IM_PI
#define IM_PI 3.14159265358979323846f
#endif

namespace guidll {

// ------------------------------------------------------------------------------------------ the engine's ImGui (located by tools/sdk_dumper)
inline constexpr uintptr_t kGImGui = sdk::glob::GImGui;                    // ImGuiContext* GImGui
inline constexpr uintptr_t kNewFrame = sdk::fn::ImGui_NewFrame;            // ImGui::NewFrame
inline constexpr uintptr_t kAllocFunc = sdk::glob::GImAllocatorAllocFunc;  // GImAllocatorAllocFunc
inline constexpr uintptr_t kFreeFunc = sdk::glob::GImAllocatorFreeFunc;    // GImAllocatorFreeFunc
inline constexpr uintptr_t kAllocUser = sdk::glob::GImAllocatorUserData;   // GImAllocatorUserData
inline constexpr uintptr_t kImGuiInit = sdk::fn::NImGuiWrapper_ImGuiInit;  // NImGuiWrapper::ImGuiInit

// ---------------------------------------------------------------------------------------------------------- process, log, settings (core.cpp)
extern uintptr_t g_base;       // image base of stellaris.exe
extern HMODULE g_module;       // this DLL
extern volatile LONG g_in_detour, g_tick_depth;
extern volatile LONG64 g_frames_total, g_frames_in_tick, g_ticks;

void Log(const char* fmt, ...);  // <plugin folder>\logs\guidll.log (UTF-8)
void OpenLog();
void CloseLog();

const std::wstring& PluginDir();  // this plugin's folder, with the trailing backslash (found from the module's own address)
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::string ReadWholeFile(const std::wstring& path);
bool FileExistsW(const std::wstring& path);

struct Config {  // <plugin folder>\config\guidll.ini
    bool deck = true;            // the Command Deck skin (status capsule and deck); off: the host only
    bool deck_open = false;      // the deck window itself open at start (the capsule is always there)
    int theme = 0;               // colour theme of the skin, 0..3
    bool stars = true;
    bool dev_commands = false;   // logs\guidll.cmd: lines run by the host (tests); off for players
    bool dev_unload = false;     // a named event unloads the DLL (development builds only; the launcher never unloads plugins)
    std::vector<std::wstring> extra_mod_dirs;  // more folders scanned for declaration files (development)
};
extern Config g_cfg;
void LoadConfig();

// Fault-tolerant reads of engine memory: a bad pointer must never take the game down.
bool Rd(const void* p, void* out, size_t n);
template <typename T>
T RdOr(uintptr_t addr, T fallback) {
    T v;
    return Rd((const void*)addr, &v, sizeof(T)) ? v : fallback;
}

struct RawStr {  // MSVC std::string as the engine lays it out
    union {
        char buf[16];
        char* ptr;
    };
    uint64_t size;
    uint64_t cap;
};
struct RawCStr {  // engine CString: 16-byte header + std::string
    uint64_t header[2];
    RawStr s;
};
static_assert(sizeof(RawCStr) == 0x30, "CString is 0x30 bytes");

std::string RdStr(uintptr_t addr);
std::string StripMarkup(const std::string& in);  // the engine's rich text as plain text
void CallFreeCString(RawCStr* s);
std::string TakeCString(RawCStr& t, bool strip = true);  // copies the text out (rich-text markup stripped unless !strip) and frees the engine's buffer
bool BuildEngineCString(const std::string& s, RawCStr* out);  // an engine CString holding s; release with CallFreeCString
struct ScopeHolder {
    void* cmd = nullptr;     // the command object the engine built the scope in (never posted)
    uintptr_t scope = 0;     // its CEventScope
};
bool AcquirePlayerScope(ScopeHolder* h, std::string* why);  // a CEventScope with the player country as This / From / Root
void ReleaseScopeHolder(ScopeHolder* h);

// -------------------------------------------------------------------------------------------------------------- game snapshot (core.cpp)
struct ResInfo {
    std::string key;
    double stock = 0, income = 0, expense = 0, net = 0, max = -1;
};
struct Snapshot {
    bool in_game = false;
    uint32_t hours = 0, speed = 0;
    bool paused = false;
    void* idler = nullptr;
    uint32_t year = 0, month = 0, day = 0, day_index = 0;
    void* country = nullptr;
    uint32_t country_id = 0xFFFFFFFF;
    std::string name;
    std::vector<ResInfo> res;
    uint32_t colonies = 0, pops = 0;
    int32_t empire_size = 0;
    double mil = 0, tech = 0, eco = 0;
    double mil_max = 1, tech_max = 1, eco_max = 1, col_max = 1, pop_max = 1;  // the best empire in the galaxy per axis
    int64_t tick = 0, serial = 0;
};
struct Hist {  // one sample per game day
    std::deque<float> stock, net;
};
extern Snapshot g_snap;  // taken between turn ticks only: drawing code never reads state a worker may be mutating
extern int64_t g_snap_serial;
extern LONG64 g_force_snapshot_frame;
void TakeSnapshot();
const ResInfo* FindRes(const char* key);
const Hist* HistOf(const std::string& key);

// -------------------------------------------------------------------------------------- script effects and deferred actions (core.cpp)
// A mod's common/button_effects entry, run through the engine's own command path (CExecuteButtonEffectCommand), as a GUI effectButtonType does.
void* FindButtonEffect(const char* key);
// Builds the command for the player country, asks the engine whether it is valid (valid / reason), and posts it when `post`.
bool RunButtonEffect(const char* key, bool post, bool* valid, std::string* reason);

struct Pending {  // something to do between turn ticks
    enum Kind { Speed, Pause, Button } kind;
    int ival = 0;
    std::string key;
};
extern std::vector<Pending> g_pending;
void RunPending();

struct ScriptLogEntry {  // a posted script effect and what became of it
    std::string key, title, result;
    double time = 0;
    int64_t tick_posted = 0, serial_posted = 0;
    bool done = false, ok = false;
    double energy_before = 0, energy_after = 0;
    bool energy_known = false;
};
extern std::deque<ScriptLogEntry> g_script_log;
void CompleteScriptLog();  // after a snapshot: entries posted before it are done

// ----------------------------------------------------------------------------------------------------------------- runtime state (core.cpp)
extern double g_T;                          // ImGui time
extern float g_DT;                          // frame time
extern float g_S0, g_S, g_fit;              // UI scale: of the screen (fonts are baked at it), of the layout, and the fit of the layout to the window
extern ImFont *g_font_body, *g_font_bold, *g_font_title, *g_font_num, *g_font_num_s;
extern ImGuiContext* g_fonts_ctx;           // the context the fonts above belong to
ImFont* F(ImFont* f);                       // f, or ImGui's current font when f is not usable (no fonts of ours in this context)
void Fmt(char* out, size_t n, double v, bool sign = false);  // 12.3k, 4.5M, +7.0

// -------------------------------------------------------------------------------------------------------------------- localisation (loc.cpp)
std::string LocKey(const std::string& key);  // a loc key through the game's localisation; text with a space, or an unknown key, as written
// Like LocKey, but the text may contain [Root.some_variable], [Root.GetName] or a scripted_loc, which the engine evaluates for the player's country.
// Evaluated between turn ticks only (inside one the last value is returned) and again when the game state has changed.
std::string LocScoped(const std::string& key);
bool ScopedText(const std::string& raw_text, std::string* out);  // the engine's text processor over `raw_text` for the player's country

// ------------------------------------------------------------------------------------------------------ panels and the interface (host_api.cpp)
struct PanelOptions {
    bool decl = false;           // declared by a mod, not registered by a plugin
    bool title_is_loc = false;   // the title is a loc key
    float w = 360, h = 280;      // first size, in layout pixels
};
int RegisterPanelInternal(const StlGuiPanelDesc& d, const PanelOptions& o);
void RetirePanels(bool declared_only);
void DispatchPanels();                                      // from the frame, main thread
void PanelCommand(const char* id, int visible);             // development: "list" logs them, else shows / hides the panel
void DispatchStats(double* us_per_frame, long long* frames);
const StlGuiApi* HostApi();

// ------------------------------------------------------------------------------------------------------- mod-declared panels (decl_panels.cpp)
void RequestRescan();
void UpdateDeclPanels();  // scans the enabled mods once a game runs, and again when asked to

// ----------------------------------------------------------------------------------------------------------- the Command Deck skin (deck.cpp)
namespace deck {
void Frame(const ImGuiIO& io);                 // draws the capsule and the deck (when enabled), inside the host's style
void FrameStats(float frame_ms, LONG64 ticks); // once per frame, for the time page's graphs
void Toggle();                                 // the hot key
bool Command(const char* cmd, int value);      // development commands: deck hud tab theme res eval; true when handled
void Dump();                                   // development: log the state
}  // namespace deck

// ----------------------------------------------------------------------------------------------------------------- the ImGui host (imgui_host.cpp)
bool RunConsole(const char* line);  // one console line, as if typed in the game's console (main thread, not for `imgui off` mid-frame)
void Start();                       // the worker thread of the plugin: hooks, then waits (dev builds: for the unload event)

}  // namespace guidll
