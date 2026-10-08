// What other plugins see of the host: the panel registry (registration from any thread), the C drawing table, and the dispatch of their draw
// callbacks with fault isolation. The public interface is include/guidll/stellaris_gui_api.h.
#include "internal.h"

namespace guidll {
namespace {

struct HostPanel {
    int handle = 0;
    std::string id, title;
    uint32_t flags = 0;
    StlGuiDrawFn draw = nullptr;
    void* user = nullptr;
    bool visible = true;
    bool decl = false;  // declared by a mod (not registered by a plugin)
    bool title_is_loc = false;
    float w = 360, h = 280;
    volatile bool dead = false;
    int faults = 0;
    uint64_t calls = 0;
};
SRWLOCK g_panel_lock = SRWLOCK_INIT;
std::vector<std::shared_ptr<HostPanel>> g_panels;
int g_next_panel = 1;

int RegisterCommon(const StlGuiPanelDesc& d, const PanelOptions& o) {
    if (d.size < sizeof(StlGuiPanelDesc) || !d.draw || !d.id || !*d.id) return 0;
    auto p = std::make_shared<HostPanel>();
    p->id = d.id;
    p->title = d.title && *d.title ? d.title : d.id;
    p->flags = d.flags ? d.flags : STL_PANEL_WINDOW;
    p->draw = d.draw;
    p->user = d.user;
    p->decl = o.decl;
    p->title_is_loc = o.title_is_loc;
    p->w = o.w;
    p->h = o.h;
    AcquireSRWLockExclusive(&g_panel_lock);
    bool dup = false;
    for (const auto& q : g_panels)
        if (!q->dead && q->id == p->id) dup = true;
    if (!dup) {
        p->handle = g_next_panel++;
        g_panels.push_back(p);
    }
    ReleaseSRWLockExclusive(&g_panel_lock);
    Log("panel %s: %s (handle %d, code %p)", p->id.c_str(), dup ? "rejected, id already registered" : "registered", p->handle, (void*)d.draw);
    return dup ? 0 : p->handle;
}
int ApiRegisterPanel(const StlGuiPanelDesc* d) { return d ? RegisterCommon(*d, PanelOptions{}) : 0; }
void ApiUnregisterPanel(int handle) {
    AcquireSRWLockExclusive(&g_panel_lock);
    for (auto& q : g_panels)
        if (q->handle == handle && !q->dead) {
            q->dead = true;
            Log("panel %s: unregistered", q->id.c_str());
        }
    ReleaseSRWLockExclusive(&g_panel_lock);
}
int ApiGetSnapshot(StlGuiSnapshot* out) {
    if (!out || out->size < 16) return 0;
    StlGuiSnapshot t{};
    t.size = sizeof(t);
    t.in_game = g_snap.in_game;
    t.year = g_snap.year;
    t.month = g_snap.month;
    t.day = g_snap.day;
    t.speed = g_snap.speed;
    t.paused = g_snap.paused;
    t.player_country_id = g_snap.country_id;
    t.tick = g_snap.tick;
    snprintf(t.country_name, sizeof(t.country_name), "%s", g_snap.name.c_str());
    for (const auto& r : g_snap.res) {
        if (t.resource_count >= 32) break;
        StlGuiResource& o = t.resources[t.resource_count++];
        snprintf(o.key, sizeof(o.key), "%s", r.key.c_str());
        o.stock = r.stock;
        o.net = r.net;
        o.max = r.max;
    }
    const uint32_t n = std::min<uint32_t>(out->size, sizeof(t));
    const uint32_t want = out->size;
    memcpy(out, &t, n);
    out->size = want;
    return g_snap.in_game ? 1 : 0;
}
int ApiEffectState(const char* key, char* reason, uint32_t cap) {
    if (!key) return -1;
    if (g_tick_depth != 0) return -1;  // a turn tick is running: the engine's checks are not safe to run now
    bool valid = false;
    std::string why;
    RunButtonEffect(key, false, &valid, &why);
    if (reason && cap) snprintf(reason, cap, "%s", valid ? "" : why.c_str());
    return valid ? 1 : 0;
}
int ApiPostEffect(const char* key) {
    if (!key || !*key) return 0;
    g_pending.push_back({ Pending::Button, 0, key });
    return 1;
}
void ApiSetSpeed(int speed) { g_pending.push_back({ Pending::Speed, speed, {} }); }
void ApiSetPaused(int paused) { g_pending.push_back({ Pending::Pause, paused ? 1 : 0, {} }); }
void ApiLog(const char* plugin, const char* line) { Log("[%s] %s", plugin ? plugin : "?", line ? line : ""); }
int ApiLocalize(const char* key, char* out, uint32_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!key || !*key) return 0;
    const std::string s = LocScoped(key);
    const size_t n = std::min<size_t>(s.size(), cap - 1);
    memcpy(out, s.data(), n);
    out[n] = 0;
    return (int)n;
}

// the C drawing wrappers (a plugin without any ImGui of its own draws through these)
void UiText(const char* s) { ImGui::TextUnformatted(s ? s : ""); }
void UiTextColored(uint32_t c, const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextUnformatted(s ? s : "");
    ImGui::PopStyleColor();
}
int UiButton(const char* l) { return ImGui::Button(l ? l : "") ? 1 : 0; }
int UiCheckbox(const char* l, int* v) {
    bool b = v && *v;
    const bool changed = ImGui::Checkbox(l ? l : "", &b);
    if (v) *v = b ? 1 : 0;
    return changed ? 1 : 0;
}
int UiSlider(const char* l, float* v, float lo, float hi) { return v && ImGui::SliderFloat(l ? l : "", v, lo, hi) ? 1 : 0; }
void UiSameLine() { ImGui::SameLine(); }
void UiSeparator() { ImGui::Separator(); }
void UiProgress(float f, float w, float h, const char* o) { ImGui::ProgressBar(f, ImVec2(w, h), o); }
void UiTooltip(const char* s) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s ? s : "");
}
void UiCursor(float* xy) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (xy) xy[0] = p.x, xy[1] = p.y;
}
void UiAvail(float* xy) {
    const ImVec2 p = ImGui::GetContentRegionAvail();
    if (xy) xy[0] = p.x, xy[1] = p.y;
}
void UiDummy(float w, float h) { ImGui::Dummy(ImVec2(w, h)); }
void UiLine(float x1, float y1, float x2, float y2, uint32_t c, float t) { ImGui::GetWindowDrawList()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), c, t); }
void UiRect(float x1, float y1, float x2, float y2, uint32_t c, float r) { ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), c, r); }
void UiCircle(float x, float y, float r, uint32_t c) { ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(x, y), r, c, 24); }
void UiText2(float x, float y, uint32_t c, const char* s) { ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), c, s ? s : ""); }

const StlGuiUi g_ui = { sizeof(StlGuiUi), 0,  UiText,   UiTextColored, UiButton,  UiCheckbox, UiSlider, UiSameLine, UiSeparator,
                        UiProgress,       UiTooltip, UiCursor, UiAvail, UiDummy,  UiLine,     UiRect,   UiCircle,   UiText2 };
const StlGuiApi g_api = { sizeof(StlGuiApi), STL_GUI_API_VERSION, sdk::kExeTimestamp, 0, ApiRegisterPanel, ApiUnregisterPanel, ApiGetSnapshot,
                          ApiEffectState,    ApiPostEffect,       ApiSetSpeed,        ApiSetPaused, ApiLog, ApiLocalize };

// A callback that raises an exception must not take the game down, and one that leaves ImGui's stacks unbalanced (a Begin without End, a
// pushed colour never popped) must not break the frame of everybody after it: the stacks are put back to where they were.
struct StackMark {
    int windows, colors, vars, fonts, groups;
};
StackMark MarkStacks() {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    return { g.CurrentWindowStack.Size, g.ColorStack.Size, g.StyleVarStack.Size, g.FontStack.Size, g.GroupStack.Size };
}
int RestoreStacks(const StackMark& m, int windows_kept) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    int repaired = 0;
    while (g.CurrentWindowStack.Size > m.windows + windows_kept) {
        if (g.CurrentWindow && (g.CurrentWindow->Flags & ImGuiWindowFlags_ChildWindow)) ImGui::EndChild();
        else ImGui::End();
        ++repaired;
    }
    while (g.GroupStack.Size > m.groups) ImGui::EndGroup(), ++repaired;
    if (g.ColorStack.Size > m.colors) repaired += g.ColorStack.Size - m.colors, ImGui::PopStyleColor(g.ColorStack.Size - m.colors);
    if (g.StyleVarStack.Size > m.vars) repaired += g.StyleVarStack.Size - m.vars, ImGui::PopStyleVar(g.StyleVarStack.Size - m.vars);
    while (g.FontStack.Size > m.fonts) ImGui::PopFont(), ++repaired;
    return repaired;
}
bool CallDraw(StlGuiDrawFn fn, const StlGuiCallbackCtx* ctx, void* user, DWORD* code) {
    __try {
        fn(ctx, user);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// whether the code of a callback is still mapped (a plugin that was unloaded without unregistering)
bool CodeIsMapped(const void* p) {
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery(p, &m, sizeof(m)) && m.State == MEM_COMMIT && (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}


void DispatchImpl() {
    std::vector<std::shared_ptr<HostPanel>> list;
    AcquireSRWLockShared(&g_panel_lock);
    for (const auto& p : g_panels)
        if (!p->dead) list.push_back(p);
    ReleaseSRWLockShared(&g_panel_lock);
    if (list.empty()) return;

    ImGuiContext* ctx_ptr = ImGui::GetCurrentContext();
    StlGuiCallbackCtx cb{};
    cb.size = sizeof(cb);
    cb.api_version = STL_GUI_API_VERSION;
    cb.imgui_context = ctx_ptr;
    cb.imgui_alloc = *(void**)(g_base + kAllocFunc);
    cb.imgui_free = *(void**)(g_base + kFreeFunc);
    cb.imgui_alloc_user = *(void**)(g_base + kAllocUser);
    cb.imgui_version_num = IMGUI_VERSION_NUM;
    cb.imgui_sizeof_io = (uint32_t)sizeof(ImGuiIO);
    cb.imgui_sizeof_style = (uint32_t)sizeof(ImGuiStyle);
    cb.imgui_sizeof_drawvert = (uint32_t)sizeof(ImDrawVert);
    cb.imgui_sizeof_drawidx = (uint32_t)sizeof(ImDrawIdx);
    cb.ui = &g_ui;
    cb.api = &g_api;
    cb.font_body = F(g_font_body);
    cb.font_bold = F(g_font_bold);
    cb.font_numbers = F(g_font_num_s);

    for (const auto& p : list) {
        if (!CodeIsMapped((const void*)p->draw)) {
            p->dead = true;
            Log("panel %s: its code is no longer mapped (plugin unloaded without unregistering), dropped", p->id.c_str());
            continue;
        }
        if (!p->visible) continue;
        const StackMark mark = MarkStacks();
        const bool window = (p->flags & STL_PANEL_WINDOW) != 0;
        bool shown = true;
        if (window) {
            // a declared panel's title is a loc key; the id after ### keeps the window's identity when the language changes
            const std::string name = p->title_is_loc ? LocKey(p->title) + "###" + p->id : p->title;
            const float w = p->w, h = p->h;
            ImGui::SetNextWindowSize(ImVec2(w * g_S, h * g_S), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2((30.f + 400.f * (float)((p->handle - 1) % 3)) * g_S, (120.f + 40.f * (float)((p->handle - 1) / 3)) * g_S), ImGuiCond_FirstUseEver);
            shown = ImGui::Begin(name.c_str(), &p->visible, ImGuiWindowFlags_NoSavedSettings);
            if (shown) ImGui::SetWindowFontScale(g_fit);
        }
        bool ok = true;
        DWORD code = 0;
        if (shown) ok = CallDraw(p->draw, &cb, p->user, &code);
        ++p->calls;
        const int repaired = RestoreStacks(mark, window ? 1 : 0);
        if (window) ImGui::End();
        if (!ok) {
            ++p->faults;
            Log("panel %s: exception 0x%08lX in its draw callback (fault %d of 3), ImGui stacks restored (%d entries)", p->id.c_str(), code, p->faults, repaired);
            if (p->faults >= 3) {
                p->dead = true;
                Log("panel %s: disabled after 3 faults", p->id.c_str());
            }
        } else if (repaired) {
            Log("panel %s: left %d ImGui stack entries open, restored", p->id.c_str(), repaired);
        }
    }
}

LONG64 g_dispatch_qpc = 0, g_dispatch_frames = 0;

}  // namespace

int RegisterPanelInternal(const StlGuiPanelDesc& d, const PanelOptions& o) { return RegisterCommon(d, o); }

void RetirePanels(bool declared_only) {
    AcquireSRWLockExclusive(&g_panel_lock);
    for (auto& p : g_panels)
        if (!declared_only || p->decl) p->dead = true;
    ReleaseSRWLockExclusive(&g_panel_lock);
}

void DispatchPanels() {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    DispatchImpl();
    QueryPerformanceCounter(&t1);
    g_dispatch_qpc += t1.QuadPart - t0.QuadPart;
    ++g_dispatch_frames;
}

void DispatchStats(double* us_per_frame, long long* frames) {
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    *frames = (long long)g_dispatch_frames;
    *us_per_frame = g_dispatch_frames ? 1e6 * (double)g_dispatch_qpc / (double)qf.QuadPart / (double)g_dispatch_frames : 0.0;
}

// development: "list" logs the panels, otherwise shows (1) or hides (0) the panel with that id
void PanelCommand(const char* id, int visible) {
    AcquireSRWLockShared(&g_panel_lock);
    for (const auto& p : g_panels) {
        if (!strcmp(id, "list")) Log("panel %d %s '%s' flags %u visible %d dead %d calls %llu faults %d", p->handle, p->id.c_str(), p->title.c_str(), p->flags,
                                    (int)p->visible, (int)p->dead, (unsigned long long)p->calls, p->faults);
        else if (p->id == id) p->visible = visible != 0;
    }
    ReleaseSRWLockShared(&g_panel_lock);
}

const StlGuiApi* HostApi() { return &g_api; }

}  // namespace guidll

// The one export other plugins look for (GetProcAddress by name). Returns the function table for any API version this host implements.
extern "C" __declspec(dllexport) const StlGuiApi* StlGui_GetApi(uint32_t requested_version) {
    if (requested_version == 0 || requested_version > STL_GUI_API_VERSION) return nullptr;
    return guidll::HostApi();
}
