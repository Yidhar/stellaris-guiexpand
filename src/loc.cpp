// The game's localisation: a loc key to the text of the current language.
#include "internal.h"

namespace guidll {
namespace {
std::unordered_map<std::string, std::string> g_loc_cache;
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

}  // namespace guidll
