// Engine access and game data: guarded reads and calls, the player's snapshot, script effects through the engine's command path, deferred actions,
// the log and the settings. Everything here runs on the game's main thread unless noted.
#include "internal.h"

namespace guiexpand {

uintptr_t g_base = 0;
HMODULE g_module = nullptr;
volatile LONG g_in_detour = 0, g_tick_depth = 0;
volatile LONG64 g_frames_total = 0, g_frames_in_tick = 0, g_ticks = 0;

// ------------------------------------------------------------------------------------------------------ plugin folder, log, settings
const std::wstring& PluginDir() {
    static const std::wstring dir = [] {
        HMODULE self = nullptr;
        wchar_t path[MAX_PATH * 4] = {};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&PluginDir),
                           &self);
        GetModuleFileNameW(self, path, (DWORD)std::size(path));
        std::wstring p(path);
        return p.substr(0, p.find_last_of(L"\\/") + 1);
    }();
    return dir;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::string ReadWholeFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
bool FileExistsW(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

namespace {
FILE* g_log = nullptr;
SRWLOCK g_log_lock = SRWLOCK_INIT;  // panels register from the threads of other plugins, which log as well
}  // namespace

void OpenLog() {
    const std::wstring dir = PluginDir() + L"logs\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"stellaris_guiexpand.log";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > (1u << 20)) {  // keep the last megabyte-sized log as .old
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _wfopen(path.c_str(), L"ab");
}
void CloseLog() {
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log) fclose(g_log);
    g_log = nullptr;
    ReleaseSRWLockExclusive(&g_log_lock);
}
void Log(const char* fmt, ...) {
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "[%04d-%02d-%02d %02d:%02d:%02d] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

Config g_cfg;
void LoadConfig() {
    const std::wstring ini = PluginDir() + L"config\\stellaris_guiexpand.ini";
    auto flag = [&](const wchar_t* key, bool def) { return GetPrivateProfileIntW(L"guiexpand", key, def ? 1 : 0, ini.c_str()) != 0; };
    g_cfg.deck = flag(L"deck", true);
    g_cfg.deck_open = flag(L"deck_open", false);
    g_cfg.stars = flag(L"stars", true);
    g_cfg.theme = std::clamp((int)GetPrivateProfileIntW(L"guiexpand", L"theme", 0, ini.c_str()), 0, kThemeCount - 1);
    g_theme = g_cfg.theme;
    g_cfg.dev_commands = flag(L"dev_commands", false);
    g_cfg.dev_unload = flag(L"dev_unload", false);
    wchar_t dirs[2048] = {};
    GetPrivateProfileStringW(L"guiexpand", L"extra_mod_dirs", L"", dirs, (DWORD)std::size(dirs), ini.c_str());
    g_cfg.extra_mod_dirs.clear();
    std::wstring all = dirs;
    for (size_t at = 0; at < all.size();) {
        size_t end = all.find(L';', at);
        if (end == std::wstring::npos) end = all.size();
        std::wstring d = all.substr(at, end - at);
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/' || d.back() == L' ')) d.pop_back();
        while (!d.empty() && d.front() == L' ') d.erase(0, 1);
        if (!d.empty()) g_cfg.extra_mod_dirs.push_back(d);
        at = end + 1;
    }
}

// -------------------------------------------------------------------------------------------------------------------- engine memory
// Fault-tolerant reads of engine memory: a bad pointer must never take the game down.
bool Rd(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string RdStr(uintptr_t addr) {
    RawStr raw{};
    if (!Rd((const void*)addr, &raw, sizeof(raw)) || raw.size == 0 || raw.size > 255) return {};
    const char* src = raw.cap > 15 ? raw.ptr : (const char*)addr;
    char tmp[256];
    if (!Rd(src, tmp, (size_t)raw.size)) return {};
    return std::string(tmp, (size_t)raw.size);
}

// Renders the engine's rich text as plain text: 0x13 wraps an icon name (the pass/fail marks become characters, other icons are dropped),
// any other control byte starts a colour code whose one-letter key ('Y', 'R' ... or '!') is dropped with it.
std::string StripMarkup(const std::string& in) {
    std::string out;
    const size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)in[i];
        if (c == 0x13) {
            std::string icon;
            size_t j = i + 1;
            for (; j < n && (unsigned char)in[j] != 0x13 && in[j] != ' ' && (unsigned char)in[j] >= 0x20; ++j)
                if (in[j] == '|') break;
                else icon += in[j];
            if (icon == "trigger_no") out += "\xC3\x97";   // multiplication sign
            else if (icon == "trigger_yes") out += "\xE2\x88\x9A";  // square root sign, the closest tick the fonts have
            while (j < n && (unsigned char)in[j] != 0x13 && in[j] != ' ' && (unsigned char)in[j] >= 0x20) ++j;
            i = (j < n && (unsigned char)in[j] == 0x13) ? j : j - 1;
        } else if (c >= 0x20 || c == '\n') {
            out += (char)c;
        } else if (i + 1 < n) {
            const unsigned char k = (unsigned char)in[i + 1];
            if (k == '!' || (k >= 'A' && k <= 'Z') || (k >= 'a' && k <= 'z')) ++i;
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    for (char& ch : out)
        if (ch == '\n') ch = ' ';
    return out;
}

// ---- guarded engine calls
void* CallGetPlayerCountry() {
    __try {
        return ((void* (*)())(g_base + sdk::fn::GetPlayerCountry))();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool CallBuildString(const void* persistent_name, RawCStr* out) {
    __try {
        ((void* (*)(const void*, void*))(g_base + sdk::fn::CPersistentName_BuildString))(persistent_name, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void CallFreeCString(RawCStr* s) {
    __try {
        ((void (*)(void*))(g_base + sdk::fn::CString_Free))(s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
std::string TakeCString(RawCStr& t, bool strip) {
    std::string out;
    if (t.s.size > 0 && t.s.size < 4096) {
        const char* p = t.s.cap > 15 ? t.s.ptr : t.s.buf;
        char tmp[4100];
        if (p && Rd(p, tmp, (size_t)t.s.size)) out.assign(tmp, (size_t)t.s.size);
    }
    if (t.s.cap > 15) CallFreeCString(&t);
    return strip ? StripMarkup(out) : out;
}
bool CallResourceMax(void* res, void* country, int64_t* out) {
    __try {
        ((void* (*)(const void*, int64_t*, const void*))(g_base + sdk::fn::CStrategicResource_GetMaximumForCountry))(res, out, country);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct PauseSettings {  // SPauseGameSettings, as the bench DLL calls it
    uint64_t unknown[2]{};
    char who[16]{};
    uint64_t who_size = 0;
    uint64_t who_capacity = 15;
    uint8_t paused = 1;
    uint8_t source = 2;
    uint8_t pad[14]{};
};
void CallSetPaused(void* idler, bool paused) {
    PauseSettings s;
    s.paused = paused ? 1 : 0;
    __try {
        ((void (*)(void*, const void*))(g_base + sdk::fn::CInGameIdler_SetPaused))(idler, &s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void CallSetSpeed(void* idler, int speed) {
    __try {
        ((void (*)(void*, int))(g_base + sdk::fn::CInGameIdler_SetGameSpeed))(idler, speed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ------------------------------------------------------------------------------------------------------------- game snapshot
Snapshot g_snap;
int64_t g_snap_serial = 0;
LONG64 g_force_snapshot_frame = 0;
std::vector<std::string> g_res_names;
std::vector<void*> g_res_ptrs;
std::vector<Hist> g_hist;
uint32_t g_last_day_index = 0xFFFFFFFF;
constexpr size_t kHistLen = 160;

void LoadResourceNames() {
    if (!g_res_names.empty()) return;
    uintptr_t db = RdOr<uintptr_t>(g_base + sdk::glob::CStrategicResourceDatabase_pInstance, 0);
    if (db < 0x10000) return;
    uint32_t count = RdOr<uint32_t>(db + 0x14, 0);
    uintptr_t arr = RdOr<uintptr_t>(db + 0x08, 0);
    if (!count || count > 256 || !arr) return;
    std::vector<std::string> names;
    std::vector<void*> ptrs;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t r = RdOr<uintptr_t>(arr + i * 8, 0);
        ptrs.push_back((void*)r);
        names.push_back(r ? RdStr(r + 0x30) : "");
    }
    g_res_names = std::move(names);
    g_res_ptrs = std::move(ptrs);
    g_hist.assign(g_res_names.size(), Hist{});
    Log("%zu resources: %s ...", g_res_names.size(), g_res_names.size() > 3 ? (g_res_names[0] + "," + g_res_names[1] + "," + g_res_names[2]).c_str() : "");
}

// military_power & co. are "ptr:fixed_point" in the SDK: follow the pointer when it is one, else take the value itself.
double ReadPower(uintptr_t country, std::ptrdiff_t off) {
    uintptr_t p = RdOr<uintptr_t>(country + off, 0);
    int64_t v = 0;
    if (p > 0x10000 && Rd((const void*)p, &v, sizeof(v))) return v / 100000.0;
    return (double)(int64_t)p / 100000.0;
}

template <typename F>
void ForEachCountry(F&& f) {
    uintptr_t db = RdOr<uintptr_t>(g_base + sdk::db::CCountry, 0);
    uintptr_t arr = db ? RdOr<uintptr_t>(db + 0x18, 0) : 0;
    uint32_t cap = db ? RdOr<uint32_t>(db + 0x20, 0) : 0;
    if (!arr || cap > 100000) return;
    for (uint32_t i = 0; i < cap; ++i) {
        uintptr_t obj = RdOr<uintptr_t>(arr + (uintptr_t)i * 16 + 8, 0);
        if (obj > 0x10000) f(obj);
    }
}

void TakeSnapshot() {
    Snapshot s;
    s.tick = g_ticks;
    s.serial = ++g_snap_serial;
    uintptr_t state = RdOr<uintptr_t>(g_base + sdk::glob::g_CurrentGameState, 0);
    uintptr_t idler = RdOr<uintptr_t>(g_base + sdk::glob::g_CurrentInGameIdler, 0);
    if (state < 0x10000 || idler < 0x10000) {
        g_snap = Snapshot{};
        return;
    }
    s.hours = RdOr<uint32_t>(state + sdk::rt::CGameState_date_hours, 0);
    if (s.hours < 24u * 1825000u) {
        g_snap = Snapshot{};
        return;
    }
    s.speed = RdOr<uint32_t>(idler + sdk::rt::CInGameIdler_speed, 0);
    s.paused = (RdOr<uint32_t>(idler + sdk::rt::CInGameIdler_paused, 0) & 0xFF) != 0;
    s.idler = (void*)idler;
    s.day_index = s.hours / 24 - 1825000;
    s.year = s.day_index / 360;
    s.month = 1 + (s.day_index % 360) / 30;
    s.day = 1 + s.day_index % 30;

    void* country = CallGetPlayerCountry();
    uint32_t cid = country ? RdOr<uint32_t>((uintptr_t)country + sdk::rt::CCountry_id, 0xFFFFFFFF) : 0xFFFFFFFF;
    if (!country || cid == 0xFFFFFFFF) {
        s.in_game = false;
        g_snap = s;
        return;
    }
    s.in_game = true;
    s.country = country;
    s.country_id = cid;
    const uintptr_t c = (uintptr_t)country;

    static int name_refresh = 0;
    if (g_snap.name.empty() || g_snap.country_id != cid || ++name_refresh > 120) {
        name_refresh = 0;
        RawCStr out{};
        out.s.cap = 15;
        if (CallBuildString((const void*)(c + sdk::ent::CCountry::name), &out)) s.name = TakeCString(out);
    } else {
        s.name = g_snap.name;
    }

    LoadResourceNames();
    uintptr_t bal = RdOr<uintptr_t>(c + sdk::ent::CCountry::type + 0x18, 0);
    uintptr_t stock_arr = bal ? RdOr<uintptr_t>(bal + 0x30, 0) : 0;
    uintptr_t inc_arr = RdOr<uintptr_t>(c + sdk::ent::CCountry::budget + 0x2D8, 0);
    uintptr_t exp_arr = RdOr<uintptr_t>(c + sdk::ent::CCountry::budget + 0x2F8, 0);
    uintptr_t net_arr = RdOr<uintptr_t>(c + sdk::ent::CCountry::budget + 0x318, 0);
    for (size_t i = 0; i < g_res_names.size(); ++i) {
        if (g_res_names[i].empty()) continue;
        ResInfo r;
        r.key = g_res_names[i];
        if (stock_arr) r.stock = RdOr<int64_t>(stock_arr + i * 8, 0) / 100000.0;
        if (inc_arr) r.income = RdOr<int64_t>(inc_arr + i * 8, 0) / 100000.0;
        if (exp_arr) r.expense = RdOr<int64_t>(exp_arr + i * 8, 0) / 100000.0;
        if (net_arr) r.net = RdOr<int64_t>(net_arr + i * 8, 0) / 100000.0;
        int64_t base_max = RdOr<int64_t>((uintptr_t)g_res_ptrs[i] + 0x110, -1);
        int64_t mx = 0;
        if (base_max >= 0 && CallResourceMax(g_res_ptrs[i], country, &mx)) r.max = mx / 100000.0;
        s.res.push_back(std::move(r));
    }

    s.colonies = RdOr<uint32_t>(c + sdk::ent::CCountry::owned_planets + 0x14, 0);
    s.pops = (uint32_t)std::max(0, RdOr<int32_t>(c + sdk::ent::CCountry::num_sapient_pops, 0));
    s.empire_size = RdOr<int32_t>(c + sdk::ent::CCountry::empire_size, 0);
    s.mil = ReadPower(c, sdk::ent::CCountry::military_power);
    s.tech = ReadPower(c, sdk::ent::CCountry::tech_power);
    s.eco = ReadPower(c, sdk::ent::CCountry::economy_power);

    // The best empire in the galaxy per axis, so the radar can show where the player stands. Refreshed once per game day.
    s.mil_max = g_snap.mil_max;
    s.tech_max = g_snap.tech_max;
    s.eco_max = g_snap.eco_max;
    s.col_max = g_snap.col_max;
    s.pop_max = g_snap.pop_max;
    if (s.day_index != g_last_day_index) {
        double mm = 1, tm = 1, em = 1, cm = 1, pm = 1;
        ForEachCountry([&](uintptr_t o) {
            if (RdOr<uint32_t>(o + sdk::rt::CCountry_id, 0xFFFFFFFF) == 0xFFFFFFFF) return;
            mm = std::max(mm, ReadPower(o, sdk::ent::CCountry::military_power));
            tm = std::max(tm, ReadPower(o, sdk::ent::CCountry::tech_power));
            em = std::max(em, ReadPower(o, sdk::ent::CCountry::economy_power));
            cm = std::max(cm, (double)RdOr<uint32_t>(o + sdk::ent::CCountry::owned_planets + 0x14, 0));
            pm = std::max(pm, (double)std::max(0, RdOr<int32_t>(o + sdk::ent::CCountry::num_sapient_pops, 0)));
        });
        s.mil_max = std::max(mm, s.mil);
        s.tech_max = std::max(tm, s.tech);
        s.eco_max = std::max(em, s.eco);
        s.col_max = std::max(cm, (double)s.colonies);
        s.pop_max = std::max(pm, (double)s.pops);
        // one history sample per game day
        for (size_t i = 0; i < s.res.size() && i < g_hist.size(); ++i) {
            size_t slot = 0;
            for (; slot < g_res_names.size(); ++slot)
                if (g_res_names[slot] == s.res[i].key) break;
            if (slot >= g_hist.size()) continue;
            g_hist[slot].stock.push_back((float)s.res[i].stock);
            g_hist[slot].net.push_back((float)s.res[i].net);
            if (g_hist[slot].stock.size() > kHistLen) g_hist[slot].stock.pop_front();
            if (g_hist[slot].net.size() > kHistLen) g_hist[slot].net.pop_front();
        }
        g_last_day_index = s.day_index;
    }
    g_snap = std::move(s);
}

const ResInfo* FindRes(const char* key) {
    for (const auto& r : g_snap.res)
        if (r.key == key) return &r;
    return nullptr;
}
const Hist* HistOf(const std::string& key) {
    for (size_t i = 0; i < g_res_names.size() && i < g_hist.size(); ++i)
        if (g_res_names[i] == key) return &g_hist[i];
    return nullptr;
}

// ------------------------------------------------------------------------------------------------------------------------- theme, series
const ThemeDef kThemes[kThemeCount] = {
    { IM_COL32(0, 229, 200, 255), IM_COL32(150, 100, 255, 255), "极光  AURORA" },
    { IM_COL32(255, 184, 64, 255), IM_COL32(255, 90, 160, 255), "余烬  EMBER" },
    { IM_COL32(90, 235, 150, 255), IM_COL32(60, 170, 255, 255), "翡翠  VERDANT" },
    { IM_COL32(255, 100, 110, 255), IM_COL32(255, 214, 90, 255), "赤焰  CRIMSON" },
};
int g_theme = 0;

std::deque<float> g_frame_ms, g_tick_rate;
static void PushSample(std::deque<float>& d, float v, size_t cap) {
    d.push_back(v);
    while (d.size() > cap) d.pop_front();
}
void FrameStats(float frame_ms, LONG64 ticks) {
    PushSample(g_frame_ms, frame_ms, 120);
    static LONG64 last_ticks = 0;
    static double last_t = 0;
    if (g_T - last_t >= 0.5) {
        PushSample(g_tick_rate, (float)((ticks - last_ticks) / (g_T - last_t)), 120);
        last_ticks = ticks;
        last_t = g_T;
    }
}

int HistorySeries(const char* series, float* out, uint32_t cap) {
    if (!series || !out || !cap) return 0;
    std::string s = series;
    const std::deque<float>* d = nullptr;
    if (s == "@frame_ms") {
        d = &g_frame_ms;
    } else if (s == "@tick_rate") {
        d = &g_tick_rate;
    } else {
        bool net = false;
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".net") == 0) {
            net = true;
            s.resize(s.size() - 4);
        }
        if (const Hist* h = HistOf(s)) d = net ? &h->net : &h->stock;
    }
    if (!d) return 0;
    const size_t n = std::min<size_t>(d->size(), cap);
    for (size_t i = 0; i < n; ++i) out[i] = (*d)[d->size() - n + i];
    return (int)n;
}

// ------------------------------------------------------------------------------------ script channel (CExecuteButtonEffectCommand)
void* FindButtonEffect(const char* key) {
    static std::unordered_map<std::string, std::pair<uintptr_t, void*>> cache;  // key -> {db, entry}
    uintptr_t db = RdOr<uintptr_t>(g_base + sdk::glob::TGameDatabase_CButtonEffectDatabase_pInstance, 0);
    if (db < 0x10000) return nullptr;
    auto it = cache.find(key);
    if (it != cache.end() && it->second.first == db) return it->second.second;
    uintptr_t arr = RdOr<uintptr_t>(db + 0x50, 0);
    int32_t n = RdOr<int32_t>(db + 0x5C, 0);
    if (!arr || n <= 0 || n > 20000) return nullptr;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t p = RdOr<uintptr_t>(arr + (uintptr_t)i * 8, 0);
        if (p > 0x10000 && RdStr(p + 0x20) == key) {
            cache[key] = { db, (void*)p };
            return (void*)p;
        }
    }
    return nullptr;
}

bool CallFactory(uintptr_t fn, void** out) {
    __try {
        *out = ((void* (*)())fn)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallIsValidSlot(void* cmd, RawCStr* reason, bool* ok) {
    __try {
        auto fn = *(bool (**)(void*, void*))(*(uintptr_t*)cmd + 8 * sizeof(void*));  // CCommand::IsValid(CString*)
        *ok = fn(cmd, reason);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void CallDestroyCommand(void* cmd) {
    __try {
        (*(void* (**)(void*, unsigned))(*(uintptr_t*)cmd))(cmd, 1);  // scalar deleting destructor
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
bool CallPostCommand(void* cmd) {
    __try {
        ((void (*)(void*, bool))(g_base + sdk::fn::PostCommand))(cmd, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool TokenMatches(uintptr_t vtable, uint32_t token) {
    uint8_t code[6];
    uintptr_t fn = RdOr<uintptr_t>(vtable + 10 * sizeof(void*), 0);
    if (!fn || !Rd((const void*)fn, code, sizeof(code)) || code[0] != 0xB8 || code[5] != 0xC3) return false;
    return *(uint32_t*)(code + 1) == token;
}

// A CEventScope for the player country (This = From = Root = the country), for engine calls that take one. The engine builds the scope inside
// a command object; the object is only the holder: it is never posted and is destroyed by ReleaseScopeHolder.
bool AcquirePlayerScope(ScopeHolder* h, std::string* why) {
    namespace spec = sdk::cmd::execute_button_effect;
    *h = ScopeHolder{};
    if (!g_snap.in_game || g_snap.country_id == 0xFFFFFFFF) {
        *why = "不在游戏中";
        return false;
    }
    if (!TokenMatches(g_base + spec::kVtableRva, spec::kToken)) {
        *why = "命令 vtable 与 SDK 不符";
        return false;
    }
    void* cmd = nullptr;
    if (!CallFactory(g_base + spec::kFactoryRva, &cmd) || !cmd || *(uintptr_t*)cmd != g_base + spec::kVtableRva) {
        *why = "命令工厂失败";
        return false;
    }
    const uintptr_t scope = (uintptr_t)cmd + spec::scope;
    // The engine's default CEventScope points root/from/prev at itself; if that is not what we see the layout is not what this code expects.
    if (RdOr<uintptr_t>(scope + 0x30, 0) != scope || RdOr<uintptr_t>(scope + 0x38, 0) != scope) {
        CallDestroyCommand(cmd);
        *why = "CEventScope 布局不符";
        return false;
    }
    // CScopeObjectReference::SetCountry: type 4, id, two zeroed words.
    *(uint64_t*)(scope + 0x08) = 4;
    *(uint32_t*)(scope + 0x10) = g_snap.country_id;
    *(uint64_t*)(scope + 0x14) = 0;
    *(uint64_t*)(scope + 0x1C) = 0;
    h->cmd = cmd;
    h->scope = scope;
    return true;
}
void ReleaseScopeHolder(ScopeHolder* h) {
    if (h->cmd) CallDestroyCommand(h->cmd);
    *h = ScopeHolder{};
}

// An engine CString holding `s`. Texts longer than the inline buffer live in a block of the engine's allocator, so that the engine (and
// CallFreeCString) can release it; the engine only reads it.
bool BuildEngineCString(const std::string& s, RawCStr* out) {
    *out = RawCStr{};
    if (s.size() <= 15) {
        memcpy(out->s.buf, s.data(), s.size());
        out->s.cap = 15;
    } else {
        char* heap = (char*)((void* (*)(size_t))(g_base + sdk::kRvaEngineAlloc))(s.size() + 1);
        if (!heap) return false;
        memcpy(heap, s.c_str(), s.size() + 1);
        out->s.ptr = heap;
        out->s.cap = s.size();
    }
    out->s.size = s.size();
    return true;
}

// Builds the command for `key` with the player country as This/From/Root, asks the engine whether it is valid and, if `post`, queues it.
// Returns false when the command could not be built or posted; `valid` / `reason` carry the engine's verdict.
bool RunButtonEffect(const char* key, bool post, bool* valid, std::string* reason) {
    namespace spec = sdk::cmd::execute_button_effect;
    *valid = false;
    if (!g_snap.in_game) {
        *reason = "不在游戏中";
        return false;
    }
    void* effect = FindButtonEffect(key);
    if (!effect) {
        *reason = "未找到 button_effect（测试 mod 没有启用？）";
        return false;
    }
    ScopeHolder holder;
    if (!AcquirePlayerScope(&holder, reason)) return false;
    void* cmd = holder.cmd;
    *(void**)((uintptr_t)cmd + spec::effect) = effect;

    RawCStr why{};
    why.s.cap = 15;
    bool ok = false;
    if (!CallIsValidSlot(cmd, &why, &ok)) {
        CallDestroyCommand(cmd);
        *reason = "IsValid 触发异常";
        return false;
    }
    std::string text = TakeCString(why);
    *valid = ok;
    if (!ok) {
        *reason = text.empty() ? "引擎未给出原因（potential 不成立）" : text;
        CallDestroyCommand(cmd);
        return false;
    }
    if (!post) {
        CallDestroyCommand(cmd);
        return true;
    }
    if (!CallPostCommand(cmd)) {
        *reason = "PostCommand 触发异常";
        return false;  // the engine may own it by now: do not free
    }
    *reason = "已入队";
    return true;
}

std::deque<ScriptLogEntry> g_script_log;
std::vector<Pending> g_pending;

// ----------------------------------------------------------------------------------------------------------------- runtime state
double g_T = 0;
float g_DT = 0.016f;
float g_S0 = 1.f, g_S = 1.f, g_fit = 1.f;
ImFont *g_font_body = nullptr, *g_font_bold = nullptr, *g_font_title = nullptr, *g_font_num = nullptr, *g_font_num_s = nullptr;
ImGuiContext* g_fonts_ctx = nullptr;

ImFont* F(ImFont* f) { return (g_fonts_ctx == ImGui::GetCurrentContext() && f) ? f : ImGui::GetFont(); }

void Fmt(char* out, size_t n, double v, bool sign) {
    const double a = fabs(v);
    const char* sg = sign && v > 0.004 ? "+" : "";
    if (a >= 1e9) snprintf(out, n, "%s%.2fB", sg, v / 1e9);
    else if (a >= 1e6) snprintf(out, n, "%s%.2fM", sg, v / 1e6);
    else if (a >= 1e4) snprintf(out, n, "%s%.1fk", sg, v / 1e3);
    else if (a >= 100) snprintf(out, n, "%s%.0f", sg, v);
    else snprintf(out, n, "%s%.1f", sg, v);
}

// ------------------------------------------------------------------------------------------------- deferred actions
void RunPending() {
    for (const Pending& p : g_pending) {
        if (!g_snap.in_game || !g_snap.idler) break;
        switch (p.kind) {
        case Pending::Speed:
            CallSetSpeed(g_snap.idler, p.ival);
            Log("speed -> %d", p.ival);
            break;
        case Pending::Pause:
            CallSetPaused(g_snap.idler, p.ival != 0);
            Log("pause -> %d", p.ival);
            break;
        case Pending::Button: {
            ScriptLogEntry e;
            e.key = p.key;
            e.title = p.key;
            e.time = g_T;
            e.tick_posted = g_ticks;
            e.serial_posted = g_snap_serial;
            g_force_snapshot_frame = g_frames_total + 8;
            if (const ResInfo* en = FindRes("energy")) {
                e.energy_before = en->stock;
                e.energy_known = true;
            }
            bool valid = false;
            std::string why;
            bool ok = RunButtonEffect(p.key.c_str(), true, &valid, &why);
            e.ok = ok;
            e.result = why;
            e.done = !ok;  // a posted command is finished once a tick has run
            Log("button %s: posted=%d valid=%d '%s'", p.key.c_str(), (int)ok, (int)valid, why.c_str());
            g_script_log.push_front(std::move(e));
            while (g_script_log.size() > 12) g_script_log.pop_back();
            break;
        }
        }
    }
    g_pending.clear();
}

void CompleteScriptLog() {
    for (auto& e : g_script_log)
        if (e.ok && !e.done && g_snap.serial > e.serial_posted) {
            e.done = true;
            if (const ResInfo* en = FindRes("energy")) e.energy_after = en->stock;
        }
}

}  // namespace guiexpand
