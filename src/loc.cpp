// The game's localisation: a loc key to the text of the current language, and text with script values in it.
#include "internal.h"

namespace guiexpand {
namespace {
std::unordered_map<std::string, std::string> g_loc_cache;   // key -> display text
std::unordered_map<std::string, std::string> g_raw_cache;   // key -> the text as the loc file has it ([...] and § markup kept)

bool CallLocalizeKey(const std::string& key, RawCStr* out) {
    struct View {
        const char* data;
        uint64_t size;
    } v{ key.data(), key.size() };
    __try {
        ((void* (*)(void*, const void*))(g_base + sdk::fn::PdxLocalize))(out, &v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// CGameText::ProcessWithScope(result, text, scope): the engine's text processor with its scope tables (country, planet, ship ... 49 kinds);
// the result CString is constructed by the call
bool CallProcessWithScope(RawCStr* out, const RawCStr* text, uintptr_t scope) {
    __try {
        ((void* (*)(void*, const void*, const void*))(g_base + sdk::fn::CGameText_ProcessWithScope))(out, text, (const void*)scope);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int logged = 0;
        if (logged++ < 5) Log("ProcessWithScope raised an exception (see the console command 'scoped' to reproduce)");
        return false;
    }
}

// the text of a key as the loc file has it, or empty when the game does not know the key
std::string LocRaw(const std::string& key) {
    auto it = g_raw_cache.find(key);
    if (it != g_raw_cache.end()) return it->second;
    RawCStr out{};
    out.s.cap = 15;
    std::string raw;
    if (CallLocalizeKey(key, &out)) raw = TakeCString(out, false);
    return g_raw_cache.emplace(key, raw).first->second;
}

struct ScopedEntry {
    std::string value;
    int64_t serial = -1;  // the snapshot the value was computed for
};
std::unordered_map<std::string, ScopedEntry> g_scoped;
}  // namespace

// a loc key goes through the game's localisation; anything with a space (or a key the game does not know) is shown as written
std::string LocKey(const std::string& key) {
    if (key.empty() || key.find(' ') != std::string::npos) return key;
    auto it = g_loc_cache.find(key);
    if (it != g_loc_cache.end()) return it->second;
    RawCStr out{};
    out.s.cap = 15;
    std::string text = key;
    if (CallLocalizeKey(key, &out)) {
        std::string t = TakeCString(out);
        if (!t.empty()) text = t;
    }
    return g_loc_cache.emplace(key, text).first->second;
}

bool ScopedText(const std::string& raw_text, std::string* out) {
    ScopeHolder holder;
    std::string why;
    if (!AcquirePlayerScope(&holder, &why)) {
        static int logged = 0;
        if (logged++ < 5) Log("scoped text: no scope (%s)", why.c_str());
        return false;
    }
    RawCStr in{}, res{};
    bool ok = false;
    if (BuildEngineCString(raw_text, &in)) {
        res.s.cap = 15;  // an empty CString; the engine constructs the result over it
        ok = CallProcessWithScope(&res, &in, holder.scope);
        if (in.s.cap > 15) CallFreeCString(&in);
        if (ok) *out = TakeCString(res);
    }
    ReleaseScopeHolder(&holder);
    return ok;
}

std::string LocScoped(const std::string& key) {
    if (key.empty() || key.find(' ') != std::string::npos) return key;
    const std::string raw = LocRaw(key);
    // no [...] in it: nothing for the scope to resolve ("[[" is a literal bracket and is handled by the engine too, so it counts as markup)
    if (raw.find('[') == std::string::npos) return LocKey(key);
    ScopedEntry& e = g_scoped[key];
    // the text processor reads game state: only between turn ticks, and once per snapshot (a paused game does not change)
    if (e.serial != g_snap.serial && g_tick_depth == 0 && g_snap.in_game) {
        std::string v;
        if (ScopedText(raw, &v)) e.value = v;
        else if (e.serial < 0) e.value = LocKey(key);
        e.serial = g_snap.serial;
    }
    return e.serial < 0 ? std::string("…") : e.value;
}

}  // namespace guiexpand
