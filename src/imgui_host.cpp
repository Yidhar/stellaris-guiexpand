// The ImGui host: starts the engine's own Dear ImGui when a game runs, adds our fonts to its atlas, and draws inside its frame (a hook on
// ImGui::NewFrame). Three engine functions are hooked (ImGui::NewFrame, NImGuiWrapper::ImGuiInit, CGameState::HandleTurnTick), nothing else:
// in particular not Present, whose shared vtable slot the Steam overlay re-patches (a second hook there ends in endless recursion).
#include "MinHook.h"

#include "internal.h"
#include "ui_glyphs.inc"

// The engine's Dear ImGui is compiled into stellaris.exe and this DLL carries its own copy, drawing into the engine's context. The SDK
// (tools/sdk_dumper, anchors.py) reads the engine's layout constants out of its code; if the engine's ImGui ever differs from the 1.85
// this copy is built from, the build stops here instead of corrupting the shared context at run time.
static_assert(sizeof(ImGuiContext) == sdk::rt::ImGuiContext_sizeof, "the engine's ImGuiContext differs from the ImGui this DLL is built with");
static_assert(offsetof(ImGuiContext, IO) + offsetof(ImGuiIO, MetricsActiveAllocations) == sdk::rt::ImGuiContext_io_MetricsActiveAllocations,
              "ImGuiContext::IO layout differs from the engine's");
static_assert(offsetof(ImGuiIO, ImeWindowHandle) == sdk::rt::ImGuiIO_ImeWindowHandle, "ImGuiIO::ImeWindowHandle differs from the engine's");
static_assert(offsetof(ImGuiIO, BackendPlatformUserData) == sdk::rt::ImGuiIO_BackendPlatformUserData,
              "ImGuiIO::BackendPlatformUserData differs from the engine's");

namespace guiexpand {
namespace {

using FnVoid = void (*)();
using FnTick = void (*)(void*, void*);
FnVoid g_orig_new_frame = nullptr;
FnVoid g_orig_imgui_init = nullptr;
FnTick g_orig_tick = nullptr;
bool g_allocators_set = false;

void AddFontsToContext();
void FixPlatformWindowHandle(ImGuiIO& io);

// ---------------------------------------------------------------------------------------------------------------- development commands
std::vector<std::pair<std::string, std::string>> g_probe_queue;  // ("loc" | "scoped", argument), run outside turn ticks

void RunProbes() {
    for (const auto& [kind, arg] : g_probe_queue) {
        if (kind == "loc") {
            Log("loc %s: plain '%s' | scoped '%s'", arg.c_str(), LocKey(arg).c_str(), LocScoped(arg).c_str());
        } else if (kind == "scopedbench") {  // the cost of one evaluation of the text, including building and releasing the scope
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            std::string out;
            int ok = 0;
            QueryPerformanceCounter(&t0);
            for (int i = 0; i < 200; ++i) ok += ScopedText(arg, &out) ? 1 : 0;
            QueryPerformanceCounter(&t1);
            Log("scopedbench '%s': %d of 200 ok, %.1f us per evaluation", arg.c_str(), ok, 1e6 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart / 200.0);
        } else {
            std::string out;
            const bool ok = ScopedText(arg, &out);
            Log("scoped '%s' -> %d '%s'", arg.c_str(), (int)ok, out.c_str());
        }
    }
    g_probe_queue.clear();
}

// logs\stellaris_guiexpand.cmd (only with dev_commands = 1): lines such as "deck 1", "tab 3", "post guiexpand_test_grant_energy", "dump". The file is deleted once run.
void PollCommandFile() {
    static int n = 0;
    if (++n % 20) return;
    const std::wstring path = PluginDir() + L"logs\\stellaris_guiexpand.cmd";
    if (!FileExistsW(path)) return;
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char cmd[32] = {}, arg[128] = {};
        if (sscanf(line, "%31s %127s", cmd, arg) < 1) continue;
        const int iv = atoi(arg);
        if (deck::Command(cmd, iv)) continue;
        if (!strcmp(cmd, "scan")) RequestRescan();
        else if (!strcmp(cmd, "panel")) {  // "panel <id> 0/1", "panel list"
            char pid[96] = {};
            int vis = 1;
            sscanf(line, "%*s %95s %d", pid, &vis);
            PanelCommand(pid, vis);
        }
        else if (!strcmp(cmd, "speed")) g_pending.push_back({ Pending::Speed, iv, {} });
        else if (!strcmp(cmd, "pause")) g_pending.push_back({ Pending::Pause, iv, {} });
        else if (!strcmp(cmd, "post")) g_pending.push_back({ Pending::Button, 0, arg });
        else if (!strcmp(cmd, "console")) {  // "console imgui hide": one console line, as if typed (not `imgui off`: that must not run mid-frame)
            std::string rest(line);
            rest.erase(0, rest.find(' ') == std::string::npos ? rest.size() : rest.find(' ') + 1);
            while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.pop_back();
            Log("console '%s' -> %d", rest.c_str(), (int)RunConsole(rest.c_str()));
        } else if (!strcmp(cmd, "dump")) {
            ImGuiIO& dio = ImGui::GetIO();
            double us = 0;
            long long frames = 0;
            DispatchStats(&us, &frames);
            Log("panel dispatch: %.1f us per frame on average over %lld frames", us, frames);
            Log("imgui: ctx %p frame %d display %.0fx%.0f windows %d active %d vtx %d idx %d capture mouse %d kbd %d | S %.2f fit %.2f fonts_ctx %p",
                (void*)ImGui::GetCurrentContext(), ImGui::GetFrameCount(), dio.DisplaySize.x, dio.DisplaySize.y, dio.MetricsRenderWindows, dio.MetricsActiveWindows,
                dio.MetricsRenderVertices, dio.MetricsRenderIndices, (int)dio.WantCaptureMouse, (int)dio.WantCaptureKeyboard, g_S, g_fit, (void*)g_fonts_ctx);
            Log("dump: in_game=%d date=%04u.%02u.%02u speed=%u paused=%d country=%u name='%s' colonies=%u pops=%u size=%d mil=%.1f/%.1f tech=%.1f/%.1f eco=%.1f/%.1f",
                (int)g_snap.in_game, g_snap.year, g_snap.month, g_snap.day, g_snap.speed, (int)g_snap.paused, g_snap.country_id, g_snap.name.c_str(), g_snap.colonies,
                g_snap.pops, g_snap.empire_size, g_snap.mil, g_snap.mil_max, g_snap.tech, g_snap.tech_max, g_snap.eco, g_snap.eco_max);
            for (const auto& r : g_snap.res)
                if (fabs(r.stock) > 0 || fabs(r.net) > 0)
                    Log("  %-22s stock %12.2f  income %9.2f  expense %9.2f  net %9.2f  max %.0f", r.key.c_str(), r.stock, r.income, r.expense, r.net, r.max);
            deck::Dump();
        } else if (!strcmp(cmd, "loc") || !strcmp(cmd, "scoped") || !strcmp(cmd, "scopedbench")) {
            // "loc <key>": the plain and the scoped text of a loc key.  "scoped <text with [Root.x]>": the engine's text processor over the rest of
            // the line. Both read game state, so they run at the next frame outside a turn tick (this file is polled at a fixed frame interval,
            // which can keep landing inside one).
            std::string rest(line);
            rest.erase(0, rest.find(' ') == std::string::npos ? rest.size() : rest.find(' ') + 1);
            while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.pop_back();
            g_probe_queue.push_back({ cmd, rest });
        }
    }
    fclose(f);
    DeleteFileW(path.c_str());
}

// Ctrl + Shift + G shows / hides the deck (only while the game window is the foreground window)
bool g_hotkey_down = false;
void PollHotkey() {
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('G') & 0x8000);
    if (down && !g_hotkey_down) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) deck::Toggle();
    }
    g_hotkey_down = down;
}

// ------------------------------------------------------------------------------------------------------------------------- the frame
// Runs right after the engine's ImGui::NewFrame, on the main thread; the engine's own views are updated after this returns.
void Frame() {
    ImGuiContext* ctx = *(ImGuiContext**)(g_base + kGImGui);
    if (!ctx) return;
    if (!g_allocators_set) {
        // Memory this copy of ImGui allocates inside the shared context is freed by the engine's copy (and the reverse): use the engine's allocators.
        ImGui::SetAllocatorFunctions((ImGuiMemAllocFunc) * (void**)(g_base + kAllocFunc), (ImGuiMemFreeFunc) * (void**)(g_base + kFreeFunc),
                                     *(void**)(g_base + kAllocUser));
        g_allocators_set = true;
    }
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the engine would write imgui.ini into the game folder; every frame, as a restarted context can reuse the address
    if (!io.ImeWindowHandle) FixPlatformWindowHandle(io);  // a context started while the game window was inactive (see FixPlatformWindowHandle)
    static ImGuiContext* seen = nullptr;
    if (ctx != seen) {
        seen = ctx;
        Log("engine context %p, ImGui %s, fonts %s", (void*)ctx, ImGui::GetVersion(), g_fonts_ctx == ctx ? "ours" : "default only");
    }
    {
        static float last_w = -2, last_h = -2;
        if (io.DisplaySize.x != last_w || io.DisplaySize.y != last_h) {
            Log("display size %.0fx%.0f -> %.0fx%.0f (frame %d, ctx %p)", last_w, last_h, io.DisplaySize.x, io.DisplaySize.y, ImGui::GetFrameCount(), (void*)ctx);
            last_w = io.DisplaySize.x;
            last_h = io.DisplaySize.y;
        }
    }
    g_T = ImGui::GetTime();
    g_DT = std::clamp(io.DeltaTime, 0.001f, 0.1f);
    g_fit = std::clamp(std::min(io.DisplaySize.x / (1230.f * g_S0), io.DisplaySize.y / (840.f * g_S0)), 0.55f, 1.f);
    g_S = g_S0 * g_fit;
    FrameStats(io.DeltaTime * 1000.f, g_ticks);
    if (g_cfg.dev_commands) PollCommandFile();
    PollHotkey();
    PollPanelHotkeys();
    if (g_tick_depth == 0) {
        RunPending();
        static LONG64 last_frame = 0;
        if (g_snap.tick != g_ticks || g_frames_total - last_frame >= 90 || (g_force_snapshot_frame && g_frames_total >= g_force_snapshot_frame)) {
            g_force_snapshot_frame = 0;
            last_frame = g_frames_total;
            TakeSnapshot();
            CompleteScriptLog();
        }
        RunProbes();
    }

    // Our style only for our windows: the engine's own ImGui views keep theirs.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(9, 12, 26, 240));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 40));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(226, 233, 255, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f * g_S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * g_S, 10 * g_S));
    ImGui::PushFont(F(g_font_body));
    deck::Frame(io);
    UpdateDeclPanels();  // the panels mods declare, scanned once a game runs
    DispatchPanels();    // the panels plugins registered through StlGui_GetApi, and the declared ones
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
}

void NewFrameDetour() {
    InterlockedIncrement(&g_in_detour);
    InterlockedIncrement64(&g_frames_total);
    if (g_tick_depth > 0) InterlockedIncrement64(&g_frames_in_tick);
    g_orig_new_frame();  // the engine's frame is open
    __try {
        Frame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int logged = 0;
        if (logged++ < 3) Log("exception 0x%08lX in Frame", (unsigned long)GetExceptionCode());
    }
    InterlockedDecrement(&g_in_detour);
}

void AutoStart();
void TickDetour(void* gs, void* cmds) {
    if (g_tick_depth == 0) AutoStart();
    InterlockedIncrement(&g_tick_depth);
    g_orig_tick(gs, cmds);
    InterlockedDecrement(&g_tick_depth);
    InterlockedIncrement64(&g_ticks);
}

// ------------------------------------------------------------------------------------------------------------------------------ fonts
// The engine's ImGuiInit takes the window for its Win32 backend from GetActiveWindow(), which is null while the game window is not the
// active one (alt-tabbed away, or a DLL reloaded from outside). It then stores 0 as io.ImeWindowHandle and in the backend data, and from
// then on the engine skips the platform NewFrame (it is guarded by io.ImeWindowHandle != 0): DisplaySize stays -1 and nothing is drawn.
// Fills both in from the process's own main window.
HWND FindGameWindow() {
    struct Ctx {
        DWORD pid;
        HWND found;
    } c{ GetCurrentProcessId(), nullptr };
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* ctx = (Ctx*)p;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            RECT r;
            if (pid == ctx->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && GetClientRect(h, &r) && r.right > 200 && r.bottom > 200) {
                ctx->found = h;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&c);
    return c.found;
}
void FixPlatformWindowHandle(ImGuiIO& io) {
    if (io.ImeWindowHandle) return;
    HWND hwnd = FindGameWindow();
    void** backend = (void**)io.BackendPlatformUserData;  // imgui_impl_win32 data: hWnd is its first member
    if (!hwnd || !backend) return;
    io.ImeWindowHandle = hwnd;
    if (!*backend) *backend = hwnd;
    Log("the engine's ImGui had no window handle (game window was not active when it started): set it to %p", (void*)hwnd);
}

void AddFontsToContext() {
    ImGuiContext* ctx = *(ImGuiContext**)(g_base + kGImGui);
    if (!ctx) return;
    ImGui::SetAllocatorFunctions((ImGuiMemAllocFunc) * (void**)(g_base + kAllocFunc), (ImGuiMemFreeFunc) * (void**)(g_base + kFreeFunc), *(void**)(g_base + kAllocUser));
    g_allocators_set = true;
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    FixPlatformWindowHandle(io);
    if (io.Fonts->IsBuilt()) {
        Log("atlas already built: keeping the default font");
        return;
    }
    char win[MAX_PATH], seg[MAX_PATH], segb[MAX_PATH], yh[MAX_PATH], yhb[MAX_PATH], bahn[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);  // ASCII on every Windows install
    snprintf(seg, MAX_PATH, "%s\\Fonts\\segoeui.ttf", win);
    snprintf(segb, MAX_PATH, "%s\\Fonts\\segoeuib.ttf", win);
    snprintf(yh, MAX_PATH, "%s\\Fonts\\msyh.ttc", win);
    snprintf(yhb, MAX_PATH, "%s\\Fonts\\msyhbd.ttc", win);
    snprintf(bahn, MAX_PATH, "%s\\Fonts\\bahnschrift.ttf", win);
    const float s = g_S0;
    // Latin plus the arrows / geometric shapes / dashes the UI uses; the CJK face adds the common Chinese set on top.
    static const ImWchar extra[] = { 0x2010, 0x2027, 0x2190, 0x21FF, 0x2212, 0x221A, 0x25A0, 0x25FF, 0 };
    static ImVector<ImWchar> latin_ranges, cjk_ranges;
    latin_ranges.clear();
    cjk_ranges.clear();
    {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesDefault());
        b.AddRanges(extra);
        b.BuildRanges(&latin_ranges);
        ImFontGlyphRangesBuilder c;
        c.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        c.AddRanges(extra);
        c.AddText(kUiText);
        c.BuildRanges(&cjk_ranges);
    }
    ImFontConfig base;
    base.OversampleH = 2;
    base.OversampleV = 1;
    auto exists = [](const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; };
    auto face = [&](const char* latin, const char* cjk, float px, bool with_cjk) -> ImFont* {
        ImFont* f = exists(latin) ? io.Fonts->AddFontFromFileTTF(latin, px, &base, latin_ranges.Data) : io.Fonts->AddFontDefault();
        if (with_cjk && exists(cjk)) {
            ImFontConfig m = base;
            m.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(cjk, px, &m, cjk_ranges.Data);
        }
        return f;
    };
    // The first font added becomes io.FontDefault, which is what the engine's own ImGui views use: keep ImGui's built-in font in that slot so they
    // look as before, and select ours explicitly (PushFont) for our windows.
    io.Fonts->AddFontDefault();
    g_font_body = face(seg, yh, 17.f * s, true);
    g_font_bold = face(segb, yhb, 17.f * s, true);
    g_font_title = face(segb, yhb, 30.f * s, false);
    g_font_num = face(bahn, bahn, 46.f * s, false);
    g_font_num_s = face(bahn, bahn, 26.f * s, false);
    g_fonts_ctx = ctx;
    Log("fonts added to context %p (scale %.2f, %d fonts)", (void*)ctx, s, io.Fonts->Fonts.Size);
}

void ImGuiInitDetour() {
    g_orig_imgui_init();
    __try {
        AddFontsToContext();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("exception 0x%08lX while adding fonts", (unsigned long)GetExceptionCode());
    }
}

bool CallRunCommandNow(void* console, RawCStr* cmd) {
    __try {
        ((void (*)(void*, const void*))(g_base + sdk::fn::CConsole_RunCommandNow))(console, cmd);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- turnkey start: once a game is running and has ticked a while, start the engine's ImGui ourselves (what `imgui on` does).
// Runs from the HandleTurnTick hook, i.e. on the main thread between ticks. The Present vtable slot is deliberately not used: the Steam
// overlay re-patches that shared slot whenever a swap chain is created, which turns a second slot hook into endless recursion.
bool NoRestartFlag() { return FileExistsW(PluginDir() + L"logs\\norestart"); }  // diagnostics: look at a running context as it is

void AutoStart() {
    static bool done = false;
    if (done || g_ticks < 30) return;
    if (RdOr<uintptr_t>(g_base + sdk::glob::g_CurrentInGameIdler, 0) < 0x10000 || RdOr<uintptr_t>(g_base + sdk::glob::g_CurrentGameState, 0) < 0x10000) return;
    done = true;
    const bool norestart = NoRestartFlag();  // a function with __try holds no objects that need unwinding
    __try {
        void* ctx = *(void**)(g_base + kGImGui);
        if (ctx && (void*)g_fonts_ctx == ctx) return;  // already ours
        if (ctx && norestart) {
            Log("engine ImGui already running (%p), 'norestart' present: leaving it alone (default font only)", ctx);
            return;
        }
        if (ctx) {
            // ImGui was started before this DLL (`imgui on` by hand, or an earlier copy of this DLL): its atlas is built and cannot take our
            // fonts any more, so let the engine tear it down and start it again; ImGuiInit goes through our detour.
            Log("engine ImGui already running (%p): restarting it to get our fonts into the atlas", ctx);
            RunConsole("imgui off");
            RunConsole("imgui on");
        } else {
            Log("starting the engine's ImGui from the tick hook");
            ((void (*)())(g_base + kImGuiInit))();  // goes through our detour, which adds the fonts
        }
        Log("context now %p", *(void**)(g_base + kGImGui));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("exception in ImGuiInit");
    }
}

}  // namespace

// Runs one console line as if typed into the in-game console. A line longer than the CString's inline buffer (15 characters) lives in a block
// from the engine's allocator, like command string payloads; RunCommandNow only reads it.
bool RunConsole(const char* line) {
    const uintptr_t console = RdOr<uintptr_t>(g_base + sdk::glob::CConsole_pInstance, 0);
    const size_t n = strlen(line);
    if (console < 0x10000 || n == 0 || n > 255) return false;
    RawCStr cmd{};
    if (!BuildEngineCString(line, &cmd)) return false;
    const bool ok = CallRunCommandNow((void*)console, &cmd);
    if (cmd.s.cap > 15) CallFreeCString(&cmd);
    return ok;
}

// The plugin's own thread (DllMain only starts it): checks the game build, installs the hooks, and in a development build waits for the unload event.
void Start() {
    OpenLog();
    LoadConfig();
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(g_base + ((const IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    Log("loaded from %s, base 0x%llX, exe timestamp 0x%08X", WideToUtf8(PluginDir()).c_str(), (unsigned long long)g_base, nt->FileHeader.TimeDateStamp);
    g_S0 = std::clamp(GetSystemMetrics(SM_CYSCREEN) / 1080.f, 1.f, 2.f);
    if (nt->FileHeader.TimeDateStamp != sdk::kExeTimestamp) {
        Log("wrong game build (SDK 0x%08X), not hooking", sdk::kExeTimestamp);
        return;
    }
    if (MH_Initialize() == MH_OK && MH_CreateHook((void*)(g_base + kNewFrame), (void*)&NewFrameDetour, (void**)&g_orig_new_frame) == MH_OK &&
        MH_CreateHook((void*)(g_base + kImGuiInit), (void*)&ImGuiInitDetour, (void**)&g_orig_imgui_init) == MH_OK &&
        MH_CreateHook((void*)(g_base + sdk::fn::CGameState_HandleTurnTick), (void*)&TickDetour, (void**)&g_orig_tick) == MH_OK &&
        MH_EnableHook(MH_ALL_HOOKS) == MH_OK) {
        Log("hooked NewFrame, ImGuiInit and HandleTurnTick");
    } else {
        Log("hook failed");
        return;
    }
    if (!g_cfg.dev_unload) return;  // the launcher never unloads plugins, and a plugin must not unload itself: the thread ends, the hooks stay

    // development builds only: unload by event, so that a new build can be tried without restarting the game
    char name[64];
    wsprintfA(name, "Local\\stellaris_guiexpand_unload_%lu", GetCurrentProcessId());
    HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, name);
    if (!ev) return;
    WaitForSingleObject(ev, INFINITE);
    MH_DisableHook(MH_ALL_HOOKS);
    for (int i = 0; i < 300 && (g_in_detour || g_tick_depth); ++i) Sleep(10);
    Sleep(300);
    MH_Uninitialize();
    Log("unloading");
    CloseLog();
    CloseHandle(ev);
    FreeLibraryAndExitThread(g_module, 0);
}

}  // namespace guiexpand
