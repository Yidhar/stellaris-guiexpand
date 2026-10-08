# Writing a plugin that shows panels through stellaris-guiexpand

For developers of Stellaris launcher plugins (DLLs). If you want to add a panel from a **mod** (no DLL), read [mod-authors.md](mod-authors.md) ([简体中文](mod-authors.zh-CN.md)) instead.

You get a window in the game, drawn with the engine's own Dear ImGui, by giving the host a function. The host calls it every frame, inside the
engine's ImGui frame, on the main thread. You do not hook the renderer, you do not read engine memory, and your plugin does not carry a game
build: after a game patch the **host** is updated, your plugin is not rebuilt.

The whole interface is `include/stellaris_guiexpand/stellaris_gui_api.h` (plain C). Two small helper headers sit next to it. Working plugins are in `examples/`.

## The rules

- C only across the boundary: no C++ types, no ownership crosses the DLL boundary (build your plugin with `/MT` and nobody frees anyone's memory).
- Every struct starts with `uint32_t size`. The caller fills in the `sizeof` it was compiled with; the receiver reads and writes only inside `size`.
  New members are only ever appended. An old plugin works with a new host and the reverse.
- Strings are UTF-8, NUL-terminated. The receiver copies what it keeps.
- Your callback runs on the **main thread**, inside the engine's ImGui frame. Keep it short (it runs every frame). Everything in the interface except
  `register_panel` / `unregister_panel` is **not thread-safe**: call it from a callback only.
- The host calls into your plugin only through the callbacks you registered.

## Finding the host

The host DLL (`stellaris_guiexpand.dll`, plugin id `stellaris-guiexpand`) exports one function:

```c
const StlGuiApi* StlGui_GetApi(uint32_t requested_version);   // NULL when that version is not supported
```

The launcher does not have load-order or dependency fields, so **do not assume the host is loaded before you**. `stellaris_gui_client.h` gives you the
discovery in one call; poll it from **your own thread** (never from `DllMain`, which holds the loader lock):

```c
#include "stellaris_gui_client.h"

static DWORD WINAPI ConnectThread(LPVOID) {
    const StlGuiApi* api = NULL;
    while (!(api = stl_gui_try_connect(STL_GUI_HOST_DLL, STL_GUI_API_VERSION))) Sleep(250);   // GetModuleHandleW + GetProcAddress
    // register panels here
    return 0;
}
```

Measured: plugins injected before the host found it within 250 ms of its appearance and their panels were registered before the host's ImGui had started.
If the host is never loaded (the player disabled it), your thread idles; it costs nothing. Log once and stop polling if you prefer.

## Registering a panel

```c
StlGuiPanelDesc d = { sizeof(d) };
d.flags = STL_PANEL_WINDOW;           // or STL_PANEL_OVERLAY
d.id    = "myplugin.status";          // stable and unique: "<your plugin id>.<panel>"
d.title = "My plugin";
d.draw  = DrawStatus;                 // void DrawStatus(const StlGuiCallbackCtx* ctx, void* user)
d.user  = NULL;
int handle = api->register_panel(&d); // > 0, or 0 when the id is taken or there is no callback
```

| Flag | The host |
|---|---|
| `STL_PANEL_WINDOW` | opens a window titled `title` around your callback and always closes it; your callback draws the content. Movable and closable (closing hides it) |
| `STL_PANEL_OVERLAY` | calls your callback with nothing open: draw anything (a HUD with `GetForegroundDrawList`, your own windows). Afterwards the host repairs ImGui's stacks |

The callback receives a `StlGuiCallbackCtx`: the API version, the engine's ImGui context and allocator pair, the engine's ImGui version, the `sizeof`
of four ImGui types (used by the binding below), a table of C drawing functions (`ctx->ui`), the host's function table (`ctx->api`), and three fonts
(`font_body`, `font_bold`, `font_numbers`, as `ImFont*`). The context pointer is passed **on every call**: the engine can restart ImGui and gets a new
one.

## Three ways to draw

| | A: your own ImGui | B: the C table (`ctx->ui`) |
|---|---|---|
| You bring | ImGui **1.85** with the default `imconfig.h`, and `stellaris_gui_imgui.hpp` | only `stellaris_gui_api.h` |
| Languages | C++ | anything that can call C functions (C, Rust, Zig, Python through ctypes...) |
| Widgets | all of ImGui, custom controls, `ImDrawList`, your own fonts | text, coloured text, button, checkbox, slider, progress bar, tooltip, same-line, separator, cursor and available region, dummy, and `DrawList` line / rect / circle / text |
| Tied to the engine's ImGui version | **yes**: the same version and configuration, or the binding refuses to draw | no: the interface does not expose ImGui |

### A: your own ImGui

Compile ImGui 1.85 into your plugin (statically, `/MT`), then bind it to the engine's context at the top of every callback:

```cpp
#include "imgui.h"
#include "stellaris_gui_api.h"
#include "stellaris_gui_imgui.hpp"

static void DrawStatus(const StlGuiCallbackCtx* ctx, void*) {
    if (!StlGuiBindImGui(ctx)) {                 // sets the allocators and the current context; checks the layout
        static bool said = false;
        if (!said) { said = true; ctx->api->log("myplugin", StlGuiBindFailure()); }
        return;                                  // refuses to draw rather than corrupt the shared context
    }
    ImGui::Text("Hello from my plugin");
    if (ImGui::Button("Pause")) ctx->api->set_paused(1);
}
```

`StlGuiBindImGui` compares the engine's `ImGuiIO`, `ImGuiStyle`, `ImDrawVert` and `ImDrawIdx` sizes and the version number with the ones your copy was built
with. A plugin built with `#define ImDrawIdx unsigned int` was measured to refuse with *"sizeof(ImDrawIdx) differs from the engine's"* while the other panels kept drawing
(`example_imgui_badcfg` in the build is exactly that).

If the engine's ImGui version ever changes, plugins of this kind must be rebuilt against it (they are refused until then: no crash). Plugins of kind B are not affected.

### B: the C table

```c
static void DrawStatus(const StlGuiCallbackCtx* ctx, void* user) {
    const StlGuiUi* ui = ctx->ui;
    ui->text("Hello from a plain C plugin");
    if (ui->button("Pause")) ctx->api->set_paused(1);
}
```

`examples/c/consumer_c.c` is a complete plugin with no ImGui anywhere in it.

## Game data and actions

All through `ctx->api`, inside callbacks:

| Call | |
|---|---|
| `get_snapshot(&snap)` | date, speed, pause, the player country's id and name, and up to 32 resources (`key`, `stock`, `net` per month, `max` or `< 0` for no cap). It is taken between turn ticks, so a callback always reads a consistent one; `snap.tick` changes with every tick. Returns 0 before a game is running |
| `effect_state(key, reason, cap)` | a mod's `common/button_effects` entry: `1` may run now, `0` the engine refuses (the **engine's own reason text** goes to `reason`), `-1` unknown. The engine evaluates `potential` and `allow` |
| `post_effect(key)` | queue the effect for the next safe moment, through the engine's own command (`CExecuteButtonEffectCommand`), for the player country. Returns 1 when queued. The engine checks it again. Verified in single player |
| `set_speed(n)` / `set_paused(b)` | the game's own setters, applied between ticks |
| `log(plugin_id, line)` | a line in stellaris-guiexpand's log, prefixed with your id |
| `localize(key, out, cap)` | *(appended after the first release: check `api->size >= offsetof(StlGuiApi, localize) + sizeof(void*)`)* a localisation key of the game (for example of a mod) as display text for the player's country. The text may contain `[Root.my_variable]`, `[Root.GetName]` or a `scripted_loc`, which the engine evaluates: that is how you show what a mod's script computes. Cheap to call every frame (the value is refreshed between ticks, when the game state changed). Returns the number of bytes copied; a text with a space in it is returned as written |

A button effect can also be a **question**: write an effect with an empty `effect = { }` and any triggers in `potential` / `allow`, and `effect_state` tells
you whether they hold. This is how a mod exposes "is this flag set?" or "has the player this technology?" without you reading any memory.

Anything the interface does not offer (fleets, planets, ...) needs you to read engine memory yourself with your own SDK. That is allowed and the host does not
prevent it; it also means your plugin is tied to a game build again.

## Faults are contained

The host protects the game from a misbehaving panel. All measured in game:

| Your callback... | The host |
|---|---|
| raises an exception (null pointer) | catches it (SEH), logs the code, restores ImGui's stacks, keeps the game running; **disables the panel after three faults** |
| leaves ImGui stacks unbalanced (window, group, colour, style var, font) | compares the stacks before and after and restores the extra entries; the panels after yours are unaffected |
| is unloaded without unregistering | checks before each call that the callback's address is still committed executable memory (`VirtualQuery`), otherwise drops the panel |
| uses a different ImGui configuration | your binding refuses to draw (above) |

Not preventable: corrupting the heap or ImGui internals, an infinite loop, or blocking for long inside the callback.

The launcher never unloads plugins and a plugin must not unload itself, so `unregister_panel` and the unload events in the examples are development aids.

## Versions

`StlGuiApi::version` and `STL_GUI_API_VERSION` change only when something is removed or changes meaning (adding members does not change it). You request a version;
the host returns the table or `NULL`, in which case degrade quietly (log and do not register). `game_exe_timestamp` in the table says which `stellaris.exe` the host was
built for.

Your plugin's manifest can leave `game.exe_timestamps` out if it uses only this interface: the launcher's spec says it only loads a plugin "that locates addresses in the exe" into the builds it lists, and does not check when none are listed. The host carries that restriction for you.

## Try the examples

`examples/cpp_imgui/consumer_imgui.cpp` and `examples/c/consumer_c.c` build with the main project (`tools\build.bat`: `example_imgui.dll`, `example_c.dll`,
`example_imgui_badcfg.dll`). To see them in a running game without the launcher, `python tools/live/guiexpand_test.py` stages and injects them (the header of the
script has the details). Each example's `consumer_<tag>.cmd` file next to its DLL accepts `fault` (raise an access violation in the callback) and `leak`
(leave ImGui stacks unbalanced), to watch the host's protection work.
