// Panels declared by mods. A mod describes a panel in the script syntax the game itself uses, in interface/stl_gui/*.txt (a folder the engine never
// reads, so nothing is logged for it and the multiplayer checksum ignores it), and the host draws it. Without the host the file does nothing.
// Grammar and the bindings: docs/mod-authors.md.
#include <shlobj.h>

#include "internal.h"

namespace guidll {
namespace {

struct SNode {
    std::string key, value;
    bool block = false;
    std::vector<SNode> kids;
};
struct Tok {
    char type;  // '{' '}' '=' or 'w' (a bare word) / 's' (a quoted string)
    std::string text;
};
std::vector<Tok> LexScript(const std::string& s) {
    std::vector<Tok> t;
    size_t i = 0;
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) i = 3;
    while (i < s.size()) {
        const char c = s[i];
        if (isspace((unsigned char)c)) {
            ++i;
        } else if (c == '#') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '{' || c == '}' || c == '=') {
            t.push_back({ c, std::string(1, c) });
            ++i;
        } else if (c == '"') {
            size_t j = i + 1;
            std::string v;
            while (j < s.size() && s[j] != '"') v += s[j++];
            t.push_back({ 's', v });
            i = j + 1;
        } else {
            size_t j = i;
            while (j < s.size() && !isspace((unsigned char)s[j]) && !strchr("{}=#\"", s[j])) ++j;
            t.push_back({ 'w', s.substr(i, j - i) });
            i = j;
        }
    }
    return t;
}
bool ParseScriptBlock(const std::vector<Tok>& t, size_t& p, SNode& out, int depth) {
    if (depth > 24) return false;
    while (p < t.size()) {
        const Tok& k = t[p];
        if (k.type == '}') {
            ++p;
            return depth > 0;
        }
        if (k.type == '=') return false;
        SNode n;
        if (k.type == '{') {  // a nameless block, an item of a list
            n.block = true;
            ++p;
            if (!ParseScriptBlock(t, p, n, depth + 1)) return false;
        } else if (p + 1 < t.size() && t[p + 1].type == '=') {  // key = value / key = { ... }
            n.key = k.text;
            p += 2;
            if (p >= t.size()) return false;
            if (t[p].type == '{') {
                n.block = true;
                ++p;
                if (!ParseScriptBlock(t, p, n, depth + 1)) return false;
            } else if (t[p].type == 'w' || t[p].type == 's') {
                n.value = t[p].text;
                ++p;
            } else {
                return false;
            }
        } else {  // a bare item of a list
            n.value = k.text;
            ++p;
        }
        out.kids.push_back(std::move(n));
    }
    return depth == 0;
}
const SNode* ChildOf(const SNode& n, const char* key) {
    for (const SNode& k : n.kids)
        if (k.key == key) return &k;
    return nullptr;
}
std::string ValOf(const SNode& n, const char* key, const char* def = "") {
    const SNode* c = ChildOf(n, key);
    return c && !c->block ? c->value : std::string(def);
}

struct DeclPanel {
    std::string mod, id, title_key;
    SNode content;
    float w = 360, h = 300;
};
std::vector<std::shared_ptr<DeclPanel>> g_decl;  // referenced by the panels' user pointer: kept for the process lifetime

std::wstring StellarisDocs() {
    wchar_t buf[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, buf))) return {};
    return std::wstring(buf) + L"\\Paradox Interactive\\Stellaris\\";
}
// (mod name, folder) of every mod in dlc_load.json's enabled_mods, through the `path=` of its .mod file, plus the extra folders of the settings
std::vector<std::pair<std::string, std::wstring>> EnabledMods() {
    std::vector<std::pair<std::string, std::wstring>> out;
    const std::wstring docs = StellarisDocs();
    const std::string json = ReadWholeFile(docs + L"dlc_load.json");
    size_t at = 0;
    while ((at = json.find("\"mod/", at)) != std::string::npos) {
        const size_t end = json.find('"', at + 1);
        if (end == std::string::npos) break;
        const std::string rel = json.substr(at + 1, end - at - 1);  // mod/xxx.mod
        at = end + 1;
        const std::string mod = ReadWholeFile(docs + Utf8ToWide(rel));
        const size_t pp = mod.find("path=\"");
        if (pp == std::string::npos) continue;
        const size_t pe = mod.find('"', pp + 6);
        if (pe == std::string::npos) continue;
        std::wstring dir = Utf8ToWide(mod.substr(pp + 6, pe - pp - 6));
        for (wchar_t& ch : dir)
            if (ch == L'/') ch = L'\\';
        if (dir.size() < 2 || dir[1] != L':') dir = docs + dir;  // not absolute: relative to the user folder
        std::string name = rel.substr(rel.find('/') + 1);
        if (name.size() > 4) name.resize(name.size() - 4);
        out.emplace_back(name, dir);
    }
    for (const std::wstring& d : g_cfg.extra_mod_dirs) out.emplace_back("extra:" + WideToUtf8(d.substr(d.find_last_of(L"\\/") + 1)), d);
    return out;
}

void DrawDeclPanel(const StlGuiCallbackCtx* ctx, void* user);

void ScanMods() {
    RetirePanels(true);  // a rescan replaces what the last one registered
    int files = 0, panels = 0;
    for (const auto& [mod, dir] : EnabledMods()) {
        const std::wstring folder = dir + L"\\interface\\stl_gui\\";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((folder + L"*.txt").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            ++files;
            const std::string fname = WideToUtf8(fd.cFileName);
            const std::vector<Tok> toks = LexScript(ReadWholeFile(folder + fd.cFileName));
            SNode root;
            size_t p = 0;
            if (!ParseScriptBlock(toks, p, root, 0)) {
                Log("mod %s: %s: syntax error, file ignored", mod.c_str(), fname.c_str());
                continue;
            }
            if (ValOf(root, "stl_gui_version") != "1") {
                Log("mod %s: %s: no stl_gui_version = 1, file ignored", mod.c_str(), fname.c_str());
                continue;
            }
            for (const SNode& k : root.kids) {
                if (k.key != "panel" || !k.block) continue;
                auto d = std::make_shared<DeclPanel>();
                d->mod = mod;
                d->id = ValOf(k, "id");
                d->title_key = ValOf(k, "title", d->id.c_str());
                if (const SNode* c = ChildOf(k, "content")) d->content = *c;
                if (const SNode* sz = ChildOf(k, "size"); sz && sz->kids.size() >= 2) {
                    d->w = (float)atof(sz->kids[0].value.c_str());
                    d->h = (float)atof(sz->kids[1].value.c_str());
                }
                if (d->id.empty()) {
                    Log("mod %s: %s: a panel without id, ignored", mod.c_str(), fname.c_str());
                    continue;
                }
                g_decl.push_back(d);
                const std::string host_id = mod + ":" + d->id;
                StlGuiPanelDesc desc{};
                desc.size = sizeof(desc);
                desc.flags = STL_PANEL_WINDOW;
                desc.id = host_id.c_str();
                desc.title = d->title_key.c_str();
                desc.draw = DrawDeclPanel;
                desc.user = d.get();
                PanelOptions opts;
                opts.decl = true;
                opts.title_is_loc = true;
                opts.w = d->w;
                opts.h = d->h;
                if (RegisterPanelInternal(desc, opts)) ++panels;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Log("mod scan: %d file(s), %d declared panel(s)", files, panels);
}

struct EffCache {
    bool valid = false;
    std::string reason;
    double t = -10;
};
std::unordered_map<std::string, EffCache> g_eff;
const EffCache& EffState(const std::string& key) {  // the engine's verdict on a button_effect, refreshed twice a second
    EffCache& e = g_eff[key];
    if (g_T - e.t > 0.5 && g_tick_depth == 0) {
        bool valid = false;
        std::string why;
        RunButtonEffect(key.c_str(), false, &valid, &why);
        e.valid = valid;
        e.reason = valid ? "" : why;
        e.t = g_T;
    }
    return e;
}

void DrawDeclNode(const SNode& n, int depth);
void DrawDeclBlock(const SNode& block, bool row, int depth) {
    bool first = true;
    for (const SNode& k : block.kids) {
        if (row && !first) ImGui::SameLine();
        DrawDeclNode(k, depth + 1);
        first = false;
    }
}
double StatValue(const std::string& stat, bool* known) {
    *known = true;
    if (stat == "colonies") return g_snap.colonies;
    if (stat == "pops") return g_snap.pops;
    if (stat == "empire_size") return g_snap.empire_size;
    if (stat == "military_power") return g_snap.mil;
    if (stat == "tech_power") return g_snap.tech;
    if (stat == "economy_power") return g_snap.eco;
    *known = false;
    return 0;
}
void DrawDeclNode(const SNode& n, int depth) {
    if (depth > 10) return;
    if (n.key == "row" && n.block) {
        DrawDeclBlock(n, true, depth);
    } else if (n.key == "text" && n.block) {
        ImGui::TextWrapped("%s", LocKey(ValOf(n, "text")).c_str());
    } else if (n.key == "separator") {
        ImGui::Separator();
    } else if (n.key == "spacer") {
        ImGui::Dummy(ImVec2(0, (float)atof(n.value.c_str())));
    } else if (n.key == "date" && n.block) {
        ImGui::Text("%s  %04u.%02u.%02u", LocKey(ValOf(n, "label")).c_str(), g_snap.year, g_snap.month, g_snap.day);
    } else if (n.key == "stat" && n.block) {
        bool known;
        const double v = StatValue(ValOf(n, "stat"), &known);
        const std::string st = ValOf(n, "stat");
        char t[32];
        if (st == "colonies" || st == "pops" || st == "empire_size") snprintf(t, sizeof(t), "%.0f", v);
        else Fmt(t, sizeof(t), v);
        ImGui::Text("%s  %s", LocKey(ValOf(n, "label")).c_str(), known ? t : "?");
    } else if (n.key == "value" && n.block) {
        const ResInfo* r = FindRes(ValOf(n, "resource").c_str());
        const std::string show = ValOf(n, "show", "stock");
        char t[32] = "?";
        if (r) Fmt(t, sizeof(t), show == "net" ? r->net : show == "income" ? r->income : show == "expense" ? r->expense : show == "max" ? r->max : r->stock, show == "net");
        ImGui::Text("%s  %s", LocKey(ValOf(n, "label")).c_str(), t);
    } else if (n.key == "gauge" && n.block) {
        const ResInfo* r = FindRes(ValOf(n, "resource").c_str());
        ImGui::TextUnformatted(LocKey(ValOf(n, "label")).c_str());
        char t[48] = "?";
        float frac = 0.f;
        if (r) {
            char a[32], b[32];
            Fmt(a, sizeof(a), r->stock);
            Fmt(b, sizeof(b), r->net, true);
            snprintf(t, sizeof(t), "%s  (%s)", a, b);
            frac = r->max > 0 ? (float)(r->stock / r->max) : 0.f;
        }
        ImGui::ProgressBar(frac, ImVec2(-1, 0), t);
    } else if (n.key == "badge" && n.block) {  // a button_effect used as a yes/no question to the game: is it allowed right now?
        const EffCache& e = EffState(ValOf(n, "probe"));
        const ImU32 col = e.valid ? IM_COL32(90, 235, 150, 255) : IM_COL32(255, 100, 110, 255);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 6, p.y + ImGui::GetTextLineHeight() * 0.5f), 4.5f, col, 12);
        ImGui::Dummy(ImVec2(16, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted(LocKey(ValOf(n, e.valid ? "yes" : "no")).c_str());
    } else if (n.key == "button" && n.block) {
        const std::string effect = ValOf(n, "effect");
        const EffCache& e = EffState(effect);
        if (!e.valid) ImGui::BeginDisabled();
        const bool clicked = ImGui::Button(LocKey(ValOf(n, "text", effect.c_str())).c_str());
        if (!e.valid) ImGui::EndDisabled();
        if (!e.valid && !e.reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", e.reason.c_str());
        if (clicked && e.valid) g_pending.push_back({ Pending::Button, 0, effect });
    }
}
void DrawDeclPanel(const StlGuiCallbackCtx*, void* user) {
    const DeclPanel* d = (const DeclPanel*)user;
    if (!g_snap.in_game) {
        ImGui::TextDisabled("not in a game");
        return;
    }
    DrawDeclBlock(d->content, false, 0);
}

bool g_rescan = false;

}  // namespace

void RequestRescan() { g_rescan = true; }

void UpdateDeclPanels() {
    static bool scanned = false;
    if ((!scanned && g_snap.in_game) || g_rescan) {
        scanned = true;
        g_rescan = false;
        ScanMods();
    }
}

}  // namespace guidll
