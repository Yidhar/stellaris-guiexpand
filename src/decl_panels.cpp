// Panels declared by mods. A mod describes a panel in the script syntax the game itself uses, in interface/stl_gui/*.txt (a folder the engine never
// reads, so nothing is logged for it and the multiplayer checksum ignores it), and the host draws it. Without the host the file does nothing.
// Grammar and the bindings: docs/mod-authors.md.
#include <shlobj.h>

#include <set>

#include "internal.h"

namespace guiexpand {
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
    std::vector<std::string> requires_;  // stl_gui_requires: plugin ids whose elements the file uses (only for the message of a missing element)
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

// the non-ASCII code points of `n` bytes of UTF-8, added to `out` (ImGui's glyph ids are 16 bits here: nothing above the BMP)
void AddUtf8Chars(const char* text, size_t size, std::set<uint32_t>& out) {
    for (size_t i = 0; i < size;) {
        const unsigned char c = (unsigned char)text[i];
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (c >= 0x80 && n > 1 && i + n <= size) {
            uint32_t cp = c & (0xFF >> (n + 1));
            for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
            if (cp > 0x7F && cp <= 0xFFFF) out.insert(cp);
        }
        i += n;
    }
}

// the non-ASCII code points of every .yml under `dir` (UTF-8), added to `out`
void CollectLocChars(const std::wstring& dir, std::set<uint32_t>& out, int depth = 0, const wchar_t* prefix = nullptr) {  // prefix: only files that start with it
    if (depth > 4) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectLocChars(dir + L"\\" + name, out, depth + 1, prefix);
        } else if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".yml") == 0 && (!prefix || _wcsnicmp(name.c_str(), prefix, wcslen(prefix)) == 0)) {
            const std::string text = ReadWholeFile(dir + L"\\" + name);
            AddUtf8Chars(text.data(), text.size(), out);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The keys of the game's resources: the top-level names of common/strategic_resources/*.txt, in the base game and in every DLC folder
std::set<std::string> GameResourceKeys(const std::wstring& game) {
    std::set<std::string> keys;
    std::vector<std::wstring> dirs = { game + L"common\\strategic_resources\\" };
    WIN32_FIND_DATAW fd;
    if (HANDLE h = FindFirstFileW((game + L"dlc\\*").c_str(), &fd); h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') dirs.push_back(game + L"dlc\\" + fd.cFileName + L"\\common\\strategic_resources\\");
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const std::wstring& d : dirs) {
        HANDLE h = FindFirstFileW((d + L"*.txt").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::vector<Tok> toks = LexScript(ReadWholeFile(d + fd.cFileName));
            SNode root;
            size_t p = 0;
            if (!ParseScriptBlock(toks, p, root, 0)) continue;
            for (const SNode& k : root.kids)
                if (k.block && !k.key.empty() && k.key[0] != '@') keys.insert(k.key);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return keys;
}

// The language the player chose: `language="l_simp_chinese"` in the game's settings.txt, as the name of the folder under localisation\ ("simp_chinese")
std::wstring PlayerLanguageDir() {
    const std::string text = ReadWholeFile(StellarisDocs() + L"settings.txt");
    const size_t at = text.find("language=\"l_");
    if (at == std::string::npos) return {};
    const size_t from = at + 12, to = text.find('"', from);
    return to == std::string::npos ? std::wstring() : Utf8ToWide(text.substr(from, to - from));
}

// Characters of the values of the lines `<key>: "..."` and `concept_<key>: "..."` for the given keys, in every .yml under `dir`. A resource's name is defined
// under its own key in some file of the game's localisation (sr_zro: "..." is in main_2_l_*.yml), and as a concept in another.
void CollectKeyedLocChars(const std::wstring& dir, const std::set<std::string>& keys, std::set<uint32_t>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.yml").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string text = ReadWholeFile(dir + L"\\" + fd.cFileName);
        for (size_t line = 0; line < text.size();) {
            size_t end = text.find('\n', line);
            if (end == std::string::npos) end = text.size();
            size_t k = line;
            while (k < end && (text[k] == ' ' || text[k] == '\t')) ++k;
            const size_t colon = text.find(':', k);
            if (colon != std::string::npos && colon < end) {
                std::string key = text.substr(k, colon - k);
                if (key.compare(0, 8, "concept_") == 0) key.erase(0, 8);
                if (keys.count(key)) AddUtf8Chars(text.data() + colon, end - colon, out);
            }
            line = end + 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

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
            std::vector<std::string> requires_all;
            if (const SNode* rq = ChildOf(root, "stl_gui_requires"))
                for (const SNode& r : rq->kids)
                    if (!r.block && !r.value.empty()) requires_all.push_back(r.value);
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
                d->requires_ = requires_all;
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
                // kind = window (default) | hud, anchor, offset = { x y }, hotkey, open = yes | no
                const std::string kind = ValOf(k, "kind", "window");
                if (kind == "hud") opts.hud = true;
                else if (kind != "window") Log("mod %s: %s: panel %s: unknown kind '%s', shown as a window", mod.c_str(), fname.c_str(), d->id.c_str(), kind.c_str());
                if (opts.hud) {
                    opts.movable = ValOf(k, "movable", "no") == "yes";
                    const std::string an = ValOf(k, "anchor", "center");
                    opts.anchor = ParseAnchor(an);
                    if (opts.anchor < 0) {
                        Log("mod %s: %s: panel %s: unknown anchor '%s', centred", mod.c_str(), fname.c_str(), d->id.c_str(), an.c_str());
                        opts.anchor = ANCHOR_CENTER;
                    }
                    if (const SNode* off = ChildOf(k, "offset"); off && off->kids.size() >= 2) {
                        opts.ox = (float)atof(off->kids[0].value.c_str());
                        opts.oy = (float)atof(off->kids[1].value.c_str());
                    }
                }
                if (const std::string hk = ValOf(k, "hotkey"); !hk.empty()) {
                    opts.hotkey = ParseHotkey(hk);
                    if (!opts.hotkey) Log("mod %s: %s: panel %s: '%s' is not a hot key (like ctrl+shift+g, f9)", mod.c_str(), fname.c_str(), d->id.c_str(), hk.c_str());
                }
                opts.open = ValOf(k, "open", "yes") != "no";
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

const StlGuiCallbackCtx* g_ctx = nullptr;     // the callback context of the declared panel being drawn
const DeclPanel* g_cur_panel = nullptr;
int g_elem_depth = 0;                         // nesting of containers (an element drawing blocks that hold elements ...)

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
// an entry that is not one of the host's: an element a plugin registered, or a note that there is none
void DrawUnknown(const SNode& n) {
    if (n.key.empty()) return;
    const ElementResult r = g_ctx ? DrawElement(n.key, (const StlGuiNode*)&n, g_ctx) : ElementResult::NotRegistered;
    if (r == ElementResult::Drawn) return;
    if (r == ElementResult::NotRegistered && !n.block) return;  // a plain `key = value` that nobody claims: a parameter of the entry around it
    std::string need = "not installed";
    if (g_cur_panel && !g_cur_panel->requires_.empty()) {
        need = "needs ";
        for (size_t i = 0; i < g_cur_panel->requires_.size(); ++i) need += (i ? ", " : "") + g_cur_panel->requires_[i];
    }
    ImGui::TextDisabled("[%s: %s]", n.key.c_str(), r == ElementResult::Disabled ? "disabled" : need.c_str());
    static std::unordered_set<std::string> said;
    if (said.insert((g_cur_panel ? g_cur_panel->mod + ":" + g_cur_panel->id : std::string()) + "/" + n.key).second)
        Log("panel %s: element '%s' is %s", g_cur_panel ? (g_cur_panel->mod + ":" + g_cur_panel->id).c_str() : "?", n.key.c_str(),
            r == ElementResult::Disabled ? "disabled (it faulted three times)" : ("not registered (" + need + ")").c_str());
}

void DrawDeclNode(const SNode& n, int depth) {
    if (depth > 10) return;
    if (n.key == "row" && n.block) {
        DrawDeclBlock(n, true, depth);
    } else if (n.key == "text" && n.block) {
        ImGui::TextWrapped("%s", LocScoped(ValOf(n, "text")).c_str());
    } else if (n.key == "separator") {
        ImGui::Separator();
    } else if (n.key == "spacer") {
        ImGui::Dummy(ImVec2(0, (float)atof(n.value.c_str())));
    } else if (n.key == "date" && n.block) {
        ImGui::Text("%s  %04u.%02u.%02u", LocScoped(ValOf(n, "label")).c_str(), g_snap.year, g_snap.month, g_snap.day);
    } else if (n.key == "stat" && n.block) {
        bool known;
        const double v = StatValue(ValOf(n, "stat"), &known);
        const std::string st = ValOf(n, "stat");
        char t[32];
        if (st == "colonies" || st == "pops" || st == "empire_size") snprintf(t, sizeof(t), "%.0f", v);
        else Fmt(t, sizeof(t), v);
        ImGui::Text("%s  %s", LocScoped(ValOf(n, "label")).c_str(), known ? t : "?");
    } else if (n.key == "value" && n.block) {
        const ResInfo* r = FindRes(ValOf(n, "resource").c_str());
        const std::string show = ValOf(n, "show", "stock");
        char t[32] = "?";
        if (r) Fmt(t, sizeof(t), show == "net" ? r->net : show == "income" ? r->income : show == "expense" ? r->expense : show == "max" ? r->max : r->stock, show == "net");
        ImGui::Text("%s  %s", LocScoped(ValOf(n, "label")).c_str(), t);
    } else if (n.key == "gauge" && n.block) {
        const ResInfo* r = FindRes(ValOf(n, "resource").c_str());
        ImGui::TextUnformatted(LocScoped(ValOf(n, "label")).c_str());
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
        ImGui::TextUnformatted(LocScoped(ValOf(n, e.valid ? "yes" : "no")).c_str());
    } else if (n.key == "button" && n.block) {
        const std::string effect = ValOf(n, "effect");
        const EffCache& e = EffState(effect);
        if (!e.valid) ImGui::BeginDisabled();
        const bool clicked = ImGui::Button(LocScoped(ValOf(n, "text", effect.c_str())).c_str());
        if (!e.valid) ImGui::EndDisabled();
        if (!e.valid && !e.reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", e.reason.c_str());
        if (clicked && e.valid) g_pending.push_back({ Pending::Button, 0, effect });
    } else {
        DrawUnknown(n);
    }
}
// ---- the accessors a registered element reads its declaration with (StlGuiNodeApi)
const SNode& NodeOf(const StlGuiNode* n) {
    static const SNode empty;
    return n ? *(const SNode*)n : empty;
}
const char* NodeKey(const StlGuiNode* n) { return NodeOf(n).key.c_str(); }
int NodeIsBlock(const StlGuiNode* n) { return NodeOf(n).block ? 1 : 0; }
const char* NodeSelfValue(const StlGuiNode* n) { return NodeOf(n).value.c_str(); }
const char* NodeValue(const StlGuiNode* n, const char* key, const char* def) {
    const SNode* c = key ? ChildOf(NodeOf(n), key) : nullptr;
    return c && !c->block ? c->value.c_str() : def;
}
double NodeNumber(const StlGuiNode* n, const char* key, double def) {
    const char* v = NodeValue(n, key, nullptr);
    return v ? atof(v) : def;
}
const StlGuiNode* NodeChild(const StlGuiNode* n, const char* key) { return key ? (const StlGuiNode*)ChildOf(NodeOf(n), key) : nullptr; }
uint32_t NodeChildCount(const StlGuiNode* n) { return (uint32_t)NodeOf(n).kids.size(); }
const StlGuiNode* NodeChildAt(const StlGuiNode* n, uint32_t i) {
    const SNode& b = NodeOf(n);
    return i < b.kids.size() ? (const StlGuiNode*)&b.kids[i] : nullptr;
}
const char* NodeText(const StlGuiNode* n, const char* key, const char* def) {
    static std::string ring[16];  // a text stays valid for the next 15 calls
    static unsigned slot = 0;
    const char* v = NodeValue(n, key, nullptr);
    if (!v) return def;
    std::string& out = ring[slot++ & 15];
    out = LocScoped(v);
    return out.c_str();
}
void NodeDrawBlock(const StlGuiNode* b) {
    if (!b || g_elem_depth > 12) return;
    ++g_elem_depth;
    DrawDeclBlock(NodeOf(b), false, 0);
    --g_elem_depth;
}
void NodeDrawRow(const StlGuiNode* b) {
    if (!b || g_elem_depth > 12) return;
    ++g_elem_depth;
    DrawDeclBlock(NodeOf(b), true, 0);
    --g_elem_depth;
}
void NodeDrawNode(const StlGuiNode* n) {
    if (!n || g_elem_depth > 12) return;
    ++g_elem_depth;
    DrawDeclNode(NodeOf(n), 0);
    --g_elem_depth;
}
const StlGuiNodeApi g_node_api = { sizeof(StlGuiNodeApi), 0, NodeKey, NodeIsBlock, NodeSelfValue, NodeValue, NodeNumber, NodeChild, NodeChildCount,
                                   NodeChildAt, NodeText, NodeDrawBlock, NodeDrawRow, NodeDrawNode };

void DrawDeclPanel(const StlGuiCallbackCtx* ctx, void* user) {
    const DeclPanel* d = (const DeclPanel*)user;
    if (!g_snap.in_game) {
        ImGui::TextDisabled("not in a game");
        return;
    }
    g_ctx = ctx;
    g_cur_panel = d;
    g_elem_depth = 0;
    DrawDeclBlock(d->content, false, 0);
    g_ctx = nullptr;
    g_cur_panel = nullptr;
}

bool g_rescan = false;

}  // namespace

std::string ModGlyphText() {
    std::set<uint32_t> cps;
    int mods = 0;
    for (const auto& [mod, dir] : EnabledMods()) {
        if (GetFileAttributesW((dir + L"\\interface\\stl_gui").c_str()) == INVALID_FILE_ATTRIBUTES) continue;  // only mods that declare panels
        ++mods;
        CollectLocChars(dir + L"\\localisation", cps);
    }
    // the names the game itself gives things a component shows (a resource's name is in concepts_l_<language>.yml of the game's own localisation)
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    std::wstring game = exe;
    game.resize(game.find_last_of(L"\\/") + 1);
    WIN32_FIND_DATAW fd;
    if (HANDLE h = FindFirstFileW((game + L"localisation\\*").c_str(), &fd); h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring lang = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && lang != L"." && lang != L"..") CollectLocChars(game + L"localisation\\" + lang, cps, 0, L"concepts");
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // ... and the resources' own names (not only concepts: 泽珞 is `sr_zro` in main_2_l_simp_chinese.yml), in the language the player plays in
    size_t named = 0;
    if (const std::wstring lang = PlayerLanguageDir(); !lang.empty()) {
        const size_t before = cps.size();
        CollectKeyedLocChars(game + L"localisation\\" + lang, GameResourceKeys(game), cps);
        named = cps.size() - before;
    }
    std::string out;
    for (uint32_t cp : cps) {  // back to UTF-8 (BMP only)
        if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    Log("glyphs: %zu characters from the localisation of %d mod(s) that declare panels, the game's concept names and its resource names (%zu of them only from resources)", cps.size(), mods, named);
    return out;
}

const StlGuiNodeApi* DeclNodeApi() { return &g_node_api; }

bool IsBuiltinElement(const std::string& name) {
    static const char* const kNames[] = { "text", "separator", "spacer", "date", "value", "gauge", "stat", "badge", "button", "row" };
    for (const char* n : kNames)
        if (name == n) return true;
    return false;
}

void RequestRescan() { g_rescan = true; }

void UpdateDeclPanels() {
    static bool scanned = false;
    if ((!scanned && g_snap.in_game) || g_rescan) {
        scanned = true;
        g_rescan = false;
        ScanMods();
    }
}

}  // namespace guiexpand
