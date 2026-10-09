/* Draft of the public C interface of the Stellaris GUI host plugin (prototype for docs/gui_plugin_api_investigation.md).
 *
 * Rules the interface is built on:
 *  - Pure C, no C++ types, no ownership crossing the boundary: plugins are built with their own static CRT (/MT), so memory allocated on one
 *    side is never freed on the other and nothing like std::string is passed.
 *  - Every struct starts with `uint32_t size`. The caller fills it with sizeof() of the struct it was compiled against; the receiver only reads
 *    or writes the members that fit into `size`. New members are only ever appended: an old plugin keeps working with a new host and the
 *    other way round.
 *  - Strings are UTF-8, NUL-terminated, copied by the receiver when it keeps them.
 *  - Draw callbacks run on the game's main thread, inside the engine's ImGui frame; nothing in this interface is thread-safe.
 *  - The host never calls into a plugin outside a callback the plugin registered, and drops a panel whose code is no longer mapped.
 */
#ifndef STELLARIS_GUI_API_H
#define STELLARIS_GUI_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STL_GUI_API_VERSION 1

/* ---------------------------------------------------------------------------------------- discovery
 * The host plugin's DLL exports one function. A plugin finds it with GetModuleHandleW(STL_GUI_HOST_DLL) + GetProcAddress, on a thread of
 * its own: plugins are loaded in playset order with no dependency between them, so the host may appear before or after the plugin. */
struct StlGuiApi;
typedef const struct StlGuiApi* (*StlGui_GetApi_fn)(uint32_t requested_version);
#define STL_GUI_EXPORT_NAME "StlGui_GetApi"
#define STL_GUI_HOST_DLL L"stellaris_guiexpand.dll" /* the host plugin's DLL (plugin id `stellaris-guiexpand`) */

/* ---------------------------------------------------------------------------------------- what a draw callback receives */
struct StlGuiUi;
struct StlGuiNodeApi;

/* The colours every component of a skin shares, so that one choice of the player changes them all (colours are 0xAABBGGRR, ImGui's ImU32). */
typedef struct StlGuiTheme {
    uint32_t size;
    uint32_t index;        /* of this theme, 0 .. theme_count - 1 */
    uint32_t theme_count;
    uint32_t reserved0;
    const char* name;      /* UTF-8, for a picker */
    uint32_t accent, accent2; /* the two colours of the theme's gradient */
    uint32_t text, text_dim;
    uint32_t good, bad, warn;
    uint32_t panel;        /* background of a panel or card, with its alpha */
    uint32_t border;
} StlGuiTheme;

typedef struct StlGuiCallbackCtx {
    uint32_t size;
    uint32_t api_version;
    /* The engine's Dear ImGui (ImGui 1.85 as of Stellaris 4.5.2). A plugin that compiles its own copy of ImGui binds it with these two calls
     * (stellaris_gui_imgui.hpp does it): ImGui::SetAllocatorFunctions(alloc, free, user); ImGui::SetCurrentContext(imgui_context).
     * The pointer changes when the engine restarts its ImGui, so it is passed on every call. */
    void* imgui_context;
    void* imgui_alloc;
    void* imgui_free;
    void* imgui_alloc_user;
    uint32_t imgui_version_num; /* IMGUI_VERSION_NUM of the engine's ImGui, e.g. 18500 */
    uint32_t reserved0;
    /* The same drawing calls without any ImGui in the plugin (see StlGuiUi). */
    const struct StlGuiUi* ui;
    const struct StlGuiApi* api;
    /* Fonts of the host, as ImFont* of the engine's atlas (usable with ImGui::PushFont by a plugin that has its own ImGui). */
    void* font_body;
    void* font_bold;
    void* font_numbers;
    /* sizeof() of the engine's ImGui types, for a plugin with its own ImGui to compare with its own: a copy built with another imconfig.h
     * (32-bit draw indices, a custom ImDrawVert, wide ImWchar ...) has other layouts and must not touch the shared context. */
    uint32_t imgui_sizeof_io;
    uint32_t imgui_sizeof_style;
    uint32_t imgui_sizeof_drawvert;
    uint32_t imgui_sizeof_drawidx;

    /* Appended after the first release: check `size` before reading the members below. */
    float ui_scale;     /* the host's layout scale (the screen's, times `fit`): multiply every pixel size you lay out with it */
    float fit;          /* the part of it that fits the layout into a small window (0.55 .. 1); text drawn with a font size of your own is scaled by it */
    float delta_time;   /* seconds of the last frame */
    float time;         /* seconds since the start of the host's ImGui, for animations */
    void* font_title;          /* more fonts of the host, ImFont*: titles */
    void* font_numbers_large;  /* big numbers */
    const struct StlGuiTheme* theme;     /* the player's current theme */
    const struct StlGuiNodeApi* node;    /* the accessors of a declaration node; set in element callbacks */
} StlGuiCallbackCtx;

typedef void (*StlGuiDrawFn)(const StlGuiCallbackCtx* ctx, void* user);

/* ---------------------------------------------------------------------------------------- panels */
enum {
    STL_PANEL_WINDOW = 1,  /* the host opens a window titled `title` around the callback and closes it again, whatever the callback does */
    STL_PANEL_OVERLAY = 2, /* the callback draws anything it likes (a HUD, its own windows); the host only restores ImGui's stacks afterwards */
};

typedef struct StlGuiPanelDesc {
    uint32_t size;
    uint32_t flags;      /* STL_PANEL_* */
    const char* id;      /* stable, unique: "<plugin id>.<panel>" */
    const char* title;   /* shown to the user */
    StlGuiDrawFn draw;
    void* user;
} StlGuiPanelDesc;

/* ---------------------------------------------------------------------------------------- game data (read only, snapshot taken between turn ticks) */
typedef struct StlGuiResource {
    char key[32];
    double stock;
    double net;
    double max; /* < 0: no cap */
} StlGuiResource;

typedef struct StlGuiSnapshot {
    uint32_t size;
    uint32_t in_game;
    uint32_t year, month, day;
    uint32_t speed;
    uint32_t paused;
    uint32_t player_country_id;
    uint32_t resource_count; /* entries written to resources[] */
    int64_t tick;            /* snapshot counter, changes with every turn tick */
    char country_name[96];
    StlGuiResource resources[32];

    /* Appended after the first release: check `size` before reading the members below (get_snapshot copies only what fits into the caller's `size`). */
    double income[32];   /* per month, indexed like resources[] */
    double expense[32];
    uint32_t colonies, pops;
    int32_t empire_size;
    uint32_t day_index;  /* days since 2200.01.01 */
    double military_power, tech_power, economy_power;
    /* the strongest empire of the galaxy on each axis (refreshed once a game day): 100 % of a radar or a ring */
    double colonies_max, pops_max, military_power_max, tech_power_max, economy_power_max;
} StlGuiSnapshot;

/* ---------------------------------------------------------------------------------------- elements (components that declarations can use)
 * A mod declares a panel in interface/stl_gui/*.txt (docs/mod-authors.md). Besides the host's own elements (text, value, gauge, button ...), a plugin
 * can register elements of its own by name; a declaration that says `ring = { resource = energy }` is then drawn by the plugin that registered `ring`.
 * That is how a component library is built. The declaration is read through the node accessors below. */
typedef struct StlGuiNode StlGuiNode; /* opaque: one entry of a declaration file, owned by the host, valid for the whole run of the game */

typedef struct StlGuiNodeApi {
    uint32_t size;
    uint32_t reserved0;
    /* the entry's own key (`ring` in `ring = { ... }`); "" for a nameless block */
    const char* (*key)(const StlGuiNode* n);
    /* 1 for `key = { ... }`, 0 for `key = value` */
    int (*is_block)(const StlGuiNode* n);
    /* the entry's own value (`6` in `spacer = 6`); "" for a block */
    const char* (*self_value)(const StlGuiNode* n);
    /* the value of the child `key = word` of a block (the first such child), or `def` when there is none or it is a block */
    const char* (*value)(const StlGuiNode* n, const char* key, const char* def);
    double (*number)(const StlGuiNode* n, const char* key, double def);
    /* the first child named `key` (a block or a plain entry), or NULL */
    const StlGuiNode* (*child)(const StlGuiNode* n, const char* key);
    uint32_t (*child_count)(const StlGuiNode* n);
    const StlGuiNode* (*child_at)(const StlGuiNode* n, uint32_t index);
    /* a child's value as the text to show: a localisation key through the game's localisation (with [Root.xxx] evaluated), or the text as written */
    const char* (*text)(const StlGuiNode* n, const char* key, const char* def); /* valid until the next call of text() */
    /* draw the children of a block with the host's renderer (vertically, or side by side with draw_row): how a container (card, tabs) shows its content.
     * `draw_node` draws one entry as an element (a built-in one, or another registered element). */
    void (*draw_block)(const StlGuiNode* block);
    void (*draw_row)(const StlGuiNode* block);
    void (*draw_node)(const StlGuiNode* n);
} StlGuiNodeApi;

typedef void (*StlGuiElementFn)(const StlGuiCallbackCtx* ctx, const StlGuiNode* node, void* user);

typedef struct StlGuiElementDesc {
    uint32_t size;
    uint32_t flags;        /* reserved, 0 */
    const char* name;      /* the key in declaration files: lower_snake_case, not one of the host's own elements */
    const char* provider;  /* the plugin id, for messages ("element ring is missing: install stellaris-argon-ui") */
    StlGuiElementFn draw;
    void* user;
} StlGuiElementDesc;

/* ---------------------------------------------------------------------------------------- the host's function table */
typedef struct StlGuiApi {
    uint32_t size;
    uint32_t version;
    uint32_t game_exe_timestamp; /* PE TimeDateStamp of the stellaris.exe this host was built for */
    uint32_t reserved0;

    /* returns a handle > 0, or 0 when the descriptor is rejected (duplicate id, no callback) */
    int (*register_panel)(const StlGuiPanelDesc* desc);
    /* a plugin that unloads (development only: the launcher never does) unregisters first; a panel whose code is gone is dropped by the host */
    void (*unregister_panel)(int handle);

    /* fills *out up to out->size bytes; returns 1, or 0 before a game is running */
    int (*get_snapshot)(StlGuiSnapshot* out);

    /* Script effects (common/button_effects of a mod), run by the engine's own command path (valid in single player, the engine rechecks it).
     * effect_state: 1 = may run now, 0 = the engine refuses (reason, UTF-8, filled when given), -1 = unknown right now.
     * post_effect: queues it for the next safe moment; returns 1 when queued. */
    int (*effect_state)(const char* effect_key, char* reason, uint32_t reason_cap);
    int (*post_effect)(const char* effect_key);

    /* the game's own speed / pause, through the engine's setters (queued, applied between ticks) */
    void (*set_speed)(int speed);
    void (*set_paused)(int paused);

    /* one line into the host's log, prefixed with the plugin's id */
    void (*log)(const char* plugin_id, const char* utf8_line);

    /* Appended after the first release: check `size` before using the members below.
     *
     * A localisation key of the game (a mod's localisation/*.yml) as display text for the player's country. The text may contain [Root.some_variable],
     * [Root.GetName] or a scripted_loc, which the engine evaluates (script values included through a scripted_loc `value = value:...`). UTF-8, the
     * game's rich text markup removed; copies at most cap-1 bytes, always NUL-terminated, returns the number copied (0: nothing to show).
     * The value is taken between turn ticks and refreshed when the game state has changed, so it is cheap to call every frame. A text containing a
     * space is returned as written. Only valid inside a draw callback. */
    int (*localize)(const char* key, char* out, uint32_t cap);

    /* A series of the last game days (about 160, oldest first), for charts. `series` is a resource key (its stock), "<resource>.net" (its monthly net),
     * "@frame_ms" (the host's frame time, sampled each frame) or "@tick_rate" (turn ticks per second). Copies at most `cap` of the newest samples and
     * returns how many; 0 for an unknown series. */
    int (*get_history)(const char* series, float* out, uint32_t cap);

    /* Elements (see above). register_element returns a handle > 0, or 0 when the name is empty, taken, or one of the host's own. A registered
     * element's callback runs with the same protection as a panel's (exceptions caught, ImGui stacks restored, disabled after three faults). */
    int (*register_element)(const StlGuiElementDesc* desc);
    void (*unregister_element)(int handle);

    /* Show or hide a panel of any source by its id (a declared panel's id is "<mod name>:<id>"): op 0 hide, 1 show, 2 toggle, -1 only ask.
     * Returns the panel's visibility afterwards (1 / 0), or -1 when there is no such panel. */
    int (*panel_visibility)(const char* panel_id, int op);

    /* The player's theme. theme_info fills *out (size set by the caller) for theme `index`, returns 0 when there is no such theme; set_theme returns 1 when it
     * changed the current one. The current theme is also in the callback context. */
    int (*theme_info)(int index, StlGuiTheme* out);
    int (*set_theme)(int index);
} StlGuiApi;

/* ---------------------------------------------------------------------------------------- drawing without ImGui (for plugins in any language)
 * Thin wrappers over the host's ImGui; only valid inside a draw callback. Colours are 0xAABBGGRR as ImGui's ImU32. */
typedef struct StlGuiUi {
    uint32_t size;
    uint32_t reserved0;
    void (*text)(const char* utf8);
    void (*text_colored)(uint32_t rgba, const char* utf8);
    int (*button)(const char* label);              /* 1 when clicked */
    int (*checkbox)(const char* label, int* value); /* 1 when changed */
    int (*slider_float)(const char* label, float* value, float lo, float hi);
    void (*same_line)(void);
    void (*separator)(void);
    void (*progress_bar)(float fraction, float width, float height, const char* overlay);
    void (*tooltip)(const char* utf8); /* shown when the previous item is hovered */
    void (*get_cursor_screen_pos)(float* xy);
    void (*get_content_region_avail)(float* xy);
    void (*dummy)(float w, float h); /* reserve space, e.g. for a custom drawing */
    /* the current window's draw list */
    void (*draw_line)(float x1, float y1, float x2, float y2, uint32_t col, float thickness);
    void (*draw_rect_filled)(float x1, float y1, float x2, float y2, uint32_t col, float rounding);
    void (*draw_circle_filled)(float x, float y, float radius, uint32_t col);
    void (*draw_text)(float x, float y, uint32_t col, const char* utf8);
} StlGuiUi;

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* STELLARIS_GUI_API_H */
