# How guidll works

Notes for people who change the host. The research behind these choices (what was tried, what crashed) is in the Stellaris MCP repository:
`docs/gui_imgui_feasibility.md`, `docs/gui_native_system.md`, `docs/gui_plugin_api_investigation.md`.

## The idea

`stellaris.exe` contains Dear ImGui 1.85 (with its DX11 and Win32 backends) and uses it for its debug views. It is normally switched off. guidll starts it,
adds its own fonts to the atlas, and draws inside the engine's ImGui frame. The engine's backends render the result; guidll has no renderer of its own and
creates no window or swap chain.

## Files

| File | Responsibility |
|---|---|
| `src/internal.h` | declarations shared between the files, namespace `guidll` |
| `src/dllmain.cpp` | `DllMain` only starts a thread (it runs under the loader lock and on the launcher's remote thread) |
| `src/imgui_host.cpp` | hooks, the frame, starting the engine's ImGui, fonts, the development command file, `Start()` |
| `src/core.cpp` | plugin folder, log, settings; guarded engine memory reads; the game snapshot; the script channel; deferred actions |
| `src/host_api.cpp` | the panel registry, the C drawing table, `StlGui_GetApi`, dispatch with fault isolation |
| `src/decl_panels.cpp` | mod discovery, the script-syntax parser, the renderer of declared panels |
| `src/loc.cpp` | loc key to display text through the engine's localisation |
| `src/deck.cpp` | the reference skin (capsule and Command Deck); a consumer of the same data as any plugin |
| `sdk/stellaris_sdk.hpp` | generated; every engine address and offset the host uses |

## Three hooks, and not the fourth

| Hook | Why |
|---|---|
| `ImGui::NewFrame` | after the original returns, the engine's frame is open: guidll draws everything here. It runs on the main thread, once per rendered frame |
| `NImGuiWrapper::ImGuiInit` | the engine builds the font atlas inside it, once. guidll adds its fonts while the atlas is still open (ImGui's default font first, because the first font added becomes `io.FontDefault`, and the engine's own views rely on it) and sets the window handle (see below) |
| `CGameState::HandleTurnTick` | marks "inside a tick" so snapshots are taken only between ticks; also where the engine's ImGui is started once a game has ticked a while |

**`Present` is deliberately not hooked.** The Steam overlay re-patches the swap chain's shared vtable slot whenever a swap chain is created; a second hook on that
slot ends in endless recursion and a stack overflow. The `NewFrame` hook gives the same per-frame opportunity without sharing a slot with anything.

## Starting the engine's ImGui

`ImGuiInit` uses `GetActiveWindow()` to find the game window. In a background job or when the game is not the active window that returns null, and the Win32
backend then has no window (no input, a broken frame). guidll sets `io.ImeWindowHandle` and the backend's window handle itself. If an ImGui was started before
guidll (by hand with the console's `imgui on`), its atlas is already built and cannot take guidll's fonts, so guidll restarts it through the console (`imgui off`,
`imgui on`); `logs\norestart` skips this for diagnostics.

The engine's ImGui and guidll's own copy are two builds of the same version. They share the context, so guidll calls `SetAllocatorFunctions` with the engine's allocator
pair (memory allocated by one copy is freed by the other) and `SetCurrentContext` every frame (the engine can restart ImGui and gets a new context). `static_assert`s in
`imgui_host.cpp` compare `sizeof(ImGuiContext)` and the offsets of `ImGuiIO` members with constants the SDK dumper reads out of the engine's own code: if the engine's ImGui is
a different build the compile fails, not the game at run time.

## Data: snapshots between ticks

About a third of the draw callbacks happen **inside** `HandleTurnTick`, where the game state is being modified. guidll reads state only when no tick is running and keeps
the result as the snapshot; callbacks always read a consistent one. Raw reads go through SEH-guarded helpers: a bad pointer returns a default and never crashes the game.
Fixed-point values are normalised (time and progress are scaled by 100000 in the engine).

## Actions: the script channel

A button builds an engine command, `CExecuteButtonEffectCommand` (size `0x198`: scope at `+0x20`, the effect pointer at `+0x190`, taken from the engine's
`CButtonEffectDatabase`), asks the engine's own `IsValid` (which gives the refusal reason in the engine's words), and posts it with `PostCommandToSession` at the
next safe moment. It works while paused. It is the command the game's own buttons use, so every client checks `potential` / `allow` again.

## Panels and fault isolation

`register_panel` can be called from any thread (the registry has a lock); dispatch happens on the main thread. Each call is wrapped: SEH for exceptions, a before / after
comparison of ImGui's window, group, colour, style-var and font stacks (extra entries are popped), `VirtualQuery` to check that the callback's code is still mapped, and a
count of faults per panel (three disable it). `docs/developers.md` has the table. The wrapper function holds no C++ objects with destructors (SEH and C++ unwinding do not
mix in one function).

## Declared panels

At the first moment a game runs (and on `scan`), guidll reads `dlc_load.json`, resolves each enabled `mod/*.mod` to its folder through `path=`, and parses
`interface/stl_gui/*.txt` with a small Paradox-script parser. Each declared panel is registered like a plugin's, with a renderer that walks the parsed tree every frame.
The only thing a declaration can do is show data and run a `button_effect`: there is no expression language.

## The reference skin

`deck.cpp` draws the status capsule and the Command Deck mostly with `ImDrawList` calls instead of ImGui's stock widgets, in a style far from the game's. It is an ordinary consumer of the same
snapshot and script channel; `deck = 0` turns it off completely. It is the largest file, and a candidate to become a separate plugin one day.

## After a game patch

1. In the Stellaris MCP repository: `python tools/sdk_dumper/dump.py` (about 90 s; validates against the running game when there is one).
2. Here: `python tools/extract_sdk.py <path>/stellaris_sdk.hpp`, which copies only the symbols the sources use and fails on a missing one.
3. `tools\build.bat`; the `static_assert`s fail if the engine's ImGui changed.
4. Update `game.exe_timestamps` in `plugin/stl-plugin.json`, run `tools/check_plugin.py` (it compares it with the SDK's timestamp).
5. Run the live regression (`tools/live/guidll_test.py`, see `README.md`).
