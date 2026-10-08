# How stellaris-guiexpand works

[English](architecture.md) | [简体中文](architecture.zh-CN.md)

Notes for people who change the host. The research behind these choices (what was tried, what crashed) is in the Stellaris MCP repository:
`docs/gui_imgui_feasibility.md`, `docs/gui_native_system.md`, `docs/gui_plugin_api_investigation.md`.

## The idea

`stellaris.exe` contains Dear ImGui 1.85 (with its DX11 and Win32 backends) and uses it for its debug views. It is normally switched off. stellaris-guiexpand starts it,
adds its own fonts to the atlas, and draws inside the engine's ImGui frame. The engine's backends render the result; stellaris-guiexpand has no renderer of its own and
creates no window or swap chain.

## Files

| File | Responsibility |
|---|---|
| `src/internal.h` | declarations shared between the files, namespace `guiexpand` |
| `src/dllmain.cpp` | `DllMain` only starts a thread (it runs under the loader lock and on the launcher's remote thread) |
| `src/imgui_host.cpp` | hooks, the frame, starting the engine's ImGui, fonts, the development command file, `Start()` |
| `src/core.cpp` | plugin folder, log, settings; guarded engine memory reads; the game snapshot; the script channel; deferred actions |
| `src/host_api.cpp` | the panel registry, the C drawing table, `StlGui_GetApi`, dispatch with fault isolation |
| `src/decl_panels.cpp` | mod discovery, the script-syntax parser, the renderer of declared panels |
| `src/loc.cpp` | loc key to display text through the engine's localisation; texts with `[Root.xxx]` through the engine's scoped text processor |
| `sdk/stellaris_sdk.hpp` | generated; every engine address and offset the host uses |

## Three hooks, and not the fourth

| Hook | Why |
|---|---|
| `ImGui::NewFrame` | after the original returns, the engine's frame is open: stellaris-guiexpand draws everything here. It runs on the main thread, once per rendered frame |
| `NImGuiWrapper::ImGuiInit` | the engine builds the font atlas inside it, once. stellaris-guiexpand adds its fonts while the atlas is still open (ImGui's default font first, because the first font added becomes `io.FontDefault`, and the engine's own views rely on it) and sets the window handle (see below) |
| `CGameState::HandleTurnTick` | marks "inside a tick" so snapshots are taken only between ticks; also where the engine's ImGui is started once a game has ticked a while |

**`Present` is deliberately not hooked.** The Steam overlay re-patches the swap chain's shared vtable slot whenever a swap chain is created; a second hook on that
slot ends in endless recursion and a stack overflow. The `NewFrame` hook gives the same per-frame opportunity without sharing a slot with anything.

## Starting the engine's ImGui

`ImGuiInit` uses `GetActiveWindow()` to find the game window. In a background job or when the game is not the active window that returns null, and the Win32
backend then has no window (no input, a broken frame). stellaris-guiexpand sets `io.ImeWindowHandle` and the backend's window handle itself. If an ImGui was started before
stellaris-guiexpand (by hand with the console's `imgui on`), its atlas is already built and cannot take stellaris-guiexpand's fonts, so stellaris-guiexpand restarts it through the console (`imgui off`,
`imgui on`); `logs\norestart` skips this for diagnostics.

The engine's ImGui and stellaris-guiexpand's own copy are two builds of the same version. They share the context, so stellaris-guiexpand calls `SetAllocatorFunctions` with the engine's allocator
pair (memory allocated by one copy is freed by the other) and `SetCurrentContext` every frame (the engine can restart ImGui and gets a new context). `static_assert`s in
`imgui_host.cpp` compare `sizeof(ImGuiContext)` and the offsets of `ImGuiIO` members with constants the SDK dumper reads out of the engine's own code: if the engine's ImGui is
a different build the compile fails, not the game at run time.

## Data: snapshots between ticks

About a third of the draw callbacks happen **inside** `HandleTurnTick`, where the game state is being modified. stellaris-guiexpand reads state only when no tick is running and keeps
the result as the snapshot; callbacks always read a consistent one. Raw reads go through SEH-guarded helpers: a bad pointer returns a default and never crashes the game.
Fixed-point values are normalised (time and progress are scaled by 100000 in the engine).

## Actions: the script channel

A button builds an engine command, `CExecuteButtonEffectCommand` (size `0x198`: scope at `+0x20`, the effect pointer at `+0x190`, taken from the engine's
`CButtonEffectDatabase`), asks the engine's own `IsValid` (which gives the refusal reason in the engine's words), and posts it with `PostCommandToSession` at the
next safe moment. It works while paused. It is the command the game's own buttons use, so every client checks `potential` / `allow` again.

## Scoped localisation

A loc text that contains `[` goes through `CGameText::ProcessWithScope(result, text, scope)` with a `CEventScope` for the player's country, so `[Root.some_variable]`, `[Root.GetName]` and
`scripted_loc` (script values through one) are evaluated by the engine. The engine builds the scope inside a `CExecuteButtonEffectCommand` object (the one the script channel fills in);
stellaris-guiexpand uses that object only as a holder and destroys it without posting. The text processor reads game state, so it runs between turn ticks only, once per snapshot
and key (about a microsecond each); a callback that runs inside a tick gets the last value. Research, calling convention and the fingerprints: `docs/gui_scoped_localisation.md` in the
Stellaris MCP repository.

## Panels and fault isolation

`register_panel` can be called from any thread (the registry has a lock); dispatch happens on the main thread. Each call is wrapped: SEH for exceptions, a before / after
comparison of ImGui's window, group, colour, style-var and font stacks (extra entries are popped), `VirtualQuery` to check that the callback's code is still mapped, and a
count of faults per panel (three disable it). `docs/developers.md` has the table. The wrapper function holds no C++ objects with destructors (SEH and C++ unwinding do not
mix in one function).

## Elements, the theme and HUDs

Plugins register named elements (`register_element`). The declaration renderer hands every entry that is not one of its ten own to the registry (`DrawElement`), with the protection of a panel:
SEH around the call, ImGui's stacks restored, the code address checked with `VirtualQuery`, three faults disable the element (the panel then shows `[name: disabled]`). An element reads its entry through
opaque node handles (`StlGuiNodeApi`, over the parsed `SNode` tree); `draw_block` / `draw_row` / `draw_node` re-enter the renderer, so containers nest (depth-limited). A name nobody registered becomes a
dim note and one log line, with the plugin ids from the file's `stl_gui_requires`.

The player's **theme** (two accent colours of four palettes, and the constant text, good / bad / warn and panel colours) lives in the host and is handed to every callback, so that skins and components
share it. `get_history` serves the per-game-day series the host keeps (resource stocks and nets, 160 days) and its own frame-time and tick-rate series.

A declared panel with `kind = hud` is an undecorated, background-less window of a fixed size at a screen anchor (padding and border are pushed before the stack mark: they are the host's to pop); `hotkey`
is polled once a frame with `GetAsyncKeyState` while the game window is the foreground window, and `open = no` starts a panel hidden.

## Declared panels

At the first moment a game runs (and on `scan`), stellaris-guiexpand reads `dlc_load.json`, resolves each enabled `mod/*.mod` to its folder through `path=`, and parses
`interface/stl_gui/*.txt` with a small Paradox-script parser. Each declared panel is registered like a plugin's, with a renderer that walks the parsed tree every frame.
The only thing a declaration can do is show data and run a `button_effect`: there is no expression language.

## After a game patch

1. In the Stellaris MCP repository: `python tools/sdk_dumper/dump.py` (about 90 s; validates against the running game when there is one).
2. Here: `python tools/extract_sdk.py <path>/stellaris_sdk.hpp`, which copies only the symbols the sources use and fails on a missing one.
3. `tools\build.bat`; the `static_assert`s fail if the engine's ImGui changed.
4. Update `game.exe_timestamps` in `plugin/stl-plugin.json`, run `tools/check_plugin.py` (it compares it with the SDK's timestamp).
5. Run the live regression (`tools/live/guiexpand_test.py`, see `README.md`).
