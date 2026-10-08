// What other plugins see of the host: the panel registry (registration from any thread), the C drawing table, and the dispatch of their draw
// callbacks with fault isolation. The public interface is include/stellaris_guiexpand/stellaris_gui_api.h.
#include "internal.h"

namespace guiexpand {
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
    bool hud = false;       // an undecorated window anchored to a screen edge
    int anchor = 0;
    float ox = 0, oy = 0;
    uint32_t hotkey = 0;    // HK_* | vk << 8
    bool hk_down = false;
    volatile bool dead = false;
    int faults = 0;
    uint64_t calls = 0;
};
SRWLOCK g_panel_lock = SRWLOCK_INIT;
std::vector<std::shared_ptr<HostPanel>> g_panels;
int g_next_panel = 1;

struct HostElement {
    int handle = 0;
    std::string name, provider;
    StlGuiElementFn draw = nullptr;
    void* user = nullptr;
    volatile bool dead = false;
    int faults = 0, repair_logs = 0;
    uint64_t calls = 0;
};
SRWLOCK g_el_lock = SRWLOCK_INIT;
std::vector<std::shared_ptr<HostElement>> g_elements;
int g_next_element = 1;

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
    p->visible = o.open;
    p->hud = o.hud;
    p->anchor = o.anchor;
    p->ox = o.ox;
    p->oy = o.oy;
    p->hotkey = o.hotkey;
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
    for (uint32_t i = 0; i < t.resource_count; ++i) {
        t.income[i] = g_snap.res[i].income;
        t.expense[i] = g_snap.res[i].expense;
    }
    t.colonies = g_snap.colonies;
    t.pops = g_snap.pops;
    t.empire_size = g_snap.empire_size;
    t.day_index = g_snap.day_index;
    t.military_power = g_snap.mil;
    t.tech_power = g_snap.tech;
    t.economy_power = g_snap.eco;
    t.colonies_max = g_snap.col_max;
    t.pops_max = g_snap.pop_max;
    t.military_power_max = g_snap.mil_max;
    t.tech_power_max = g_snap.tech_max;
    t.economy_power_max = g_snap.eco_max;
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

int ApiGetHistory(const char* series, float* out, uint32_t cap) { return HistorySeries(series, out, cap); }

void FillTheme(int i, StlGuiTheme* t) {  // t->size is set by the caller
    const ThemeDef& d = kThemes[i];
    t->index = (uint32_t)i;
    t->theme_count = kThemeCount;
    t->name = d.name;
    t->accent = d.a;
    t->accent2 = d.b;
    t->text = IM_COL32(226, 233, 255, 255);
    t->text_dim = IM_COL32(138, 150, 188, 255);
    t->good = IM_COL32(90, 235, 150, 255);
    t->bad = IM_COL32(255, 100, 110, 255);
    t->warn = IM_COL32(255, 190, 80, 255);
    t->panel = IM_COL32(11, 15, 32, 205);
    t->border = IM_COL32(255, 255, 255, 24);
}
int ApiThemeInfo(int index, StlGuiTheme* out) {
    if (!out || out->size < 16 || index < 0 || index >= kThemeCount) return 0;
    StlGuiTheme t{};
    t.size = sizeof(t);
    FillTheme(index, &t);
    const uint32_t want = out->size;
    memcpy(out, &t, std::min<uint32_t>(want, sizeof(t)));
    out->size = want;
    return 1;
}
int ApiSetTheme(int index) {
    if (index < 0 || index >= kThemeCount || index == g_theme) return 0;
    g_theme = index;
    return 1;
}

int ApiPanelVisibility(const char* id, int op) {
    if (!id) return -1;
    int result = -1;
    AcquireSRWLockShared(&g_panel_lock);
    for (const auto& p : g_panels) {
        if (p->dead || p->id != id) continue;
        if (op == 0) p->visible = false;
        else if (op == 1) p->visible = true;
        else if (op == 2) p->visible = !p->visible;
        result = p->visible ? 1 : 0;
        break;
    }
    ReleaseSRWLockShared(&g_panel_lock);
    return result;
}

int ApiRegisterElement(const StlGuiElementDesc* d) {
    if (!d || d->size < sizeof(StlGuiElementDesc) || !d->draw || !d->name || !*d->name) return 0;
    const std::string name = d->name;
    if (IsBuiltinElement(name)) {
        Log("element %s: rejected, it is one of the host's own elements", name.c_str());
        return 0;
    }
    auto e = std::make_shared<HostElement>();
    e->name = name;
    e->provider = d->provider ? d->provider : "";
    e->draw = d->draw;
    e->user = d->user;
    AcquireSRWLockExclusive(&g_el_lock);
    bool dup = false;
    for (const auto& q : g_elements)
        if (!q->dead && q->name == name) dup = true;
    if (!dup) {
        e->handle = g_next_element++;
        g_elements.push_back(e);
    }
    ReleaseSRWLockExclusive(&g_el_lock);
    Log("element %s: %s (provider %s, handle %d, code %p)", name.c_str(), dup ? "rejected, name already registered" : "registered", e->provider.c_str(), e->handle,
        (void*)d->draw);
    return dup ? 0 : e->handle;
}
void ApiUnregisterElement(int handle) {
    AcquireSRWLockExclusive(&g_el_lock);
    for (auto& q : g_elements)
        if (q->handle == handle && !q->dead) {
            q->dead = true;
            Log("element %s: unregistered", q->name.c_str());
        }
    ReleaseSRWLockExclusive(&g_el_lock);
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
                          ApiEffectState,    ApiPostEffect,       ApiSetSpeed,        ApiSetPaused, ApiLog, ApiLocalize,
                          ApiGetHistory,     ApiRegisterElement,  ApiUnregisterElement, ApiPanelVisibility, ApiThemeInfo, ApiSetTheme };

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


// where a HUD window goes: the point of the screen it is anchored to, and which point of the window sits there; offsets are the distance inward
void AnchorPoint(int anchor, ImVec2 disp, float ox, float oy, ImVec2* pos, ImVec2* pivot) {
    const bool left = anchor == ANCHOR_TOP_LEFT || anchor == ANCHOR_LEFT_CENTER || anchor == ANCHOR_BOTTOM_LEFT;
    const bool right = anchor == ANCHOR_TOP_RIGHT || anchor == ANCHOR_RIGHT_CENTER || anchor == ANCHOR_BOTTOM_RIGHT;
    const bool top = anchor == ANCHOR_TOP_LEFT || anchor == ANCHOR_TOP_CENTER || anchor == ANCHOR_TOP_RIGHT;
    const bool bottom = anchor == ANCHOR_BOTTOM_LEFT || anchor == ANCHOR_BOTTOM_CENTER || anchor == ANCHOR_BOTTOM_RIGHT;
    pivot->x = left ? 0.f : right ? 1.f : 0.5f;
    pivot->y = top ? 0.f : bottom ? 1.f : 0.5f;
    pos->x = left ? ox : right ? disp.x - ox : disp.x * 0.5f + ox;
    pos->y = top ? oy : bottom ? disp.y - oy : disp.y * 0.5f + oy;
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
    cb.ui_scale = g_S;
    cb.fit = g_fit;
    cb.delta_time = g_DT;
    cb.time = (float)g_T;
    cb.font_title = F(g_font_title);
    cb.font_numbers_large = F(g_font_num);
    StlGuiTheme theme{};
    theme.size = sizeof(theme);
    FillTheme(g_theme, &theme);
    cb.theme = &theme;
    cb.node = DeclNodeApi();

    for (const auto& p : list) {
        if (!CodeIsMapped((const void*)p->draw)) {
            p->dead = true;
            Log("panel %s: its code is no longer mapped (plugin unloaded without unregistering), dropped", p->id.c_str());
            continue;
        }
        if (!p->visible) continue;
        const bool window = (p->flags & STL_PANEL_WINDOW) != 0;
        const bool hud = window && p->hud;
        if (hud) {  // no padding and no border: the components of a HUD place everything themselves (pushed before the mark: they are ours to pop)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        }
        const StackMark mark = MarkStacks();
        bool shown = true;
        if (window) {
            // a declared panel's title is a loc key; the id after ### keeps the window's identity when the language changes
            const std::string name = p->title_is_loc ? LocKey(p->title) + "###" + p->id : p->title;
            const float w = p->w, h = p->h;
            if (hud) {
                ImVec2 pos, pivot;
                AnchorPoint(p->anchor, ImGui::GetIO().DisplaySize, p->ox * g_S, p->oy * g_S, &pos, &pivot);
                ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
                ImGui::SetNextWindowSize(ImVec2(w * g_S, h * g_S), ImGuiCond_Always);
                shown = ImGui::Begin(name.c_str(), nullptr,
                                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
            } else {
                ImGui::SetNextWindowSize(ImVec2(w * g_S, h * g_S), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowPos(ImVec2((30.f + 400.f * (float)((p->handle - 1) % 3)) * g_S, (120.f + 40.f * (float)((p->handle - 1) / 3)) * g_S), ImGuiCond_FirstUseEver);
                shown = ImGui::Begin(name.c_str(), &p->visible, ImGuiWindowFlags_NoSavedSettings);
            }
            if (shown) ImGui::SetWindowFontScale(g_fit);
        }
        bool ok = true;
        DWORD code = 0;
        if (shown) ok = CallDraw(p->draw, &cb, p->user, &code);
        ++p->calls;
        const int repaired = RestoreStacks(mark, window ? 1 : 0);
        if (window) ImGui::End();
        if (hud) ImGui::PopStyleVar(2);
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

bool CallElement(StlGuiElementFn fn, const StlGuiCallbackCtx* ctx, const StlGuiNode* node, void* user, DWORD* code) {
    __try {
        fn(ctx, node, user);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

ElementResult DrawElement(const std::string& name, const StlGuiNode* node, const StlGuiCallbackCtx* ctx) {
    std::shared_ptr<HostElement> el;
    bool dead_match = false;
    AcquireSRWLockShared(&g_el_lock);
    for (const auto& e : g_elements) {
        if (e->name != name) continue;
        if (!e->dead) {
            el = e;
            break;
        }
        dead_match = true;
    }
    ReleaseSRWLockShared(&g_el_lock);
    if (!el) return dead_match ? ElementResult::Disabled : ElementResult::NotRegistered;
    if (!CodeIsMapped((const void*)el->draw)) {
        el->dead = true;
        Log("element %s: its code is no longer mapped (plugin unloaded without unregistering), dropped", name.c_str());
        return ElementResult::Disabled;
    }
    const StackMark mark = MarkStacks();
    DWORD code = 0;
    const bool ok = CallElement(el->draw, ctx, node, el->user, &code);
    ++el->calls;
    const int repaired = RestoreStacks(mark, 0);
    if (!ok) {
        ++el->faults;
        Log("element %s: exception 0x%08lX in its draw callback (fault %d of 3), ImGui stacks restored (%d entries)", name.c_str(), code, el->faults, repaired);
        if (el->faults >= 3) {
            el->dead = true;
            Log("element %s: disabled after 3 faults", name.c_str());
        }
    } else if (repaired && el->repair_logs++ < 3) {
        Log("element %s: left %d ImGui stack entries open, restored", name.c_str(), repaired);
    }
    return ElementResult::Drawn;
}

std::string ElementProvider(const std::string& name) {
    std::string out;
    AcquireSRWLockShared(&g_el_lock);
    for (const auto& e : g_elements)
        if (e->name == name && !e->dead) out = e->provider;
    ReleaseSRWLockShared(&g_el_lock);
    return out;
}

// "ctrl+shift+g", "f9", "alt+1": modifiers | virtual key << 8; 0 when it is not a hot key
uint32_t ParseHotkey(const std::string& text) {
    uint32_t mods = 0, vk = 0;
    std::string tok;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != '+') {
            tok += (char)tolower((unsigned char)text[i]);
            continue;
        }
        if (tok == "ctrl" || tok == "control") mods |= HK_CTRL;
        else if (tok == "shift") mods |= HK_SHIFT;
        else if (tok == "alt") mods |= HK_ALT;
        else if (tok.size() == 1 && ((tok[0] >= 'a' && tok[0] <= 'z') || (tok[0] >= '0' && tok[0] <= '9'))) vk = (uint32_t)toupper((unsigned char)tok[0]);
        else if (tok.size() >= 2 && tok[0] == 'f' && atoi(tok.c_str() + 1) >= 1 && atoi(tok.c_str() + 1) <= 12) vk = (uint32_t)(VK_F1 + atoi(tok.c_str() + 1) - 1);
        else if (tok == "space") vk = VK_SPACE;
        else if (tok == "tab") vk = VK_TAB;
        else if (tok == "enter") vk = VK_RETURN;
        else if (tok == "escape" || tok == "esc") vk = VK_ESCAPE;
        else if (!tok.empty()) return 0;
        tok.clear();
    }
    return vk ? (mods | (vk << 8)) : 0;
}

int ParseAnchor(const std::string& text) {
    std::string t;
    for (char c : text) t += (c == '-' || c == ' ') ? '_' : (char)tolower((unsigned char)c);
    static const std::pair<const char*, int> names[] = {
        { "top_left", ANCHOR_TOP_LEFT },       { "top_center", ANCHOR_TOP_CENTER },       { "top_right", ANCHOR_TOP_RIGHT },
        { "left_center", ANCHOR_LEFT_CENTER }, { "center", ANCHOR_CENTER },               { "right_center", ANCHOR_RIGHT_CENTER },
        { "bottom_left", ANCHOR_BOTTOM_LEFT }, { "bottom_center", ANCHOR_BOTTOM_CENTER }, { "bottom_right", ANCHOR_BOTTOM_RIGHT },
    };
    for (const auto& n : names)
        if (t == n.first) return n.second;
    return -1;
}

void PollPanelHotkeys() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool ours = pid == GetCurrentProcessId();  // only while the game window is the foreground window
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    AcquireSRWLockShared(&g_panel_lock);
    for (auto& p : g_panels) {
        if (p->dead || !p->hotkey) continue;
        const uint32_t mods = p->hotkey & 0xFF;
        const int vk = (int)(p->hotkey >> 8);
        const bool now = ours && down(vk) && (!(mods & HK_CTRL) || down(VK_CONTROL)) && (!(mods & HK_SHIFT) || down(VK_SHIFT)) && (!(mods & HK_ALT) || down(VK_MENU));
        if (now && !p->hk_down) p->visible = !p->visible;
        p->hk_down = now;
    }
    ReleaseSRWLockShared(&g_panel_lock);
}

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
    if (!strcmp(id, "list")) {
        AcquireSRWLockShared(&g_el_lock);
        for (const auto& e : g_elements)
            Log("element %d %s provider '%s' dead %d calls %llu faults %d", e->handle, e->name.c_str(), e->provider.c_str(), (int)e->dead,
                (unsigned long long)e->calls, e->faults);
        ReleaseSRWLockShared(&g_el_lock);
    }
}

const StlGuiApi* HostApi() { return &g_api; }

}  // namespace guiexpand

// The one export other plugins look for (GetProcAddress by name). Returns the function table for any API version this host implements.
extern "C" __declspec(dllexport) const StlGuiApi* StlGui_GetApi(uint32_t requested_version) {
    if (requested_version == 0 || requested_version > STL_GUI_API_VERSION) return nullptr;
    return guiexpand::HostApi();
}
