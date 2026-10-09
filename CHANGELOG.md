# Changelog

## 0.1.0 (2026-10-09)

The Command Deck (a status capsule and a five-page window) that the first prototype drew itself is **not part of the host any more**: it is rebuilt from declarations with the
components of stellaris-argon-ui (`examples/command-deck` in that repository), which shows that a mod can do it without a DLL. The host keeps only what a component needs:
the theme, the data, the registry.

Added on the way to the component library (`stellaris-argon-ui`), all append-only in the interface:

- **Elements**: `register_element` / `unregister_element` and the node accessors (`StlGuiNodeApi`): a plugin registers named elements that declarations use like the host's own, containers draw their
  content through the host, with the protection of a panel (three faults disable an element). A missing element is a dim note and a log line (`stl_gui_requires` names what to install).
- **Theme** shared by everything (`ctx->theme`, `theme_info`, `set_theme`), more in the callback context (`ui_scale`, `fit`, `delta_time`, `time`, `font_title`, `font_numbers_large`), snapshot
  appended with income / expense, the countries' figures and the galaxy's strongest values, `get_history`, `panel_visibility`.
- **Declared panels**: `kind = hud` with `anchor` / `offset`, `hotkey`, `open = no`, `movable = yes`.
- **Characters**: the font atlas is built once, so it gets the characters of the localisation of the active mods that declare panels, the game's `concepts*.yml` and the names of the game's resources
  in the player's language (a rare character such as the 珞 of Zro showed as `?` before).
- `examples/element`: a plugin with seven elements in plain C; the test mod has a panel and a HUD that use them.

The first version as its own repository. Everything before it was a prototype inside the Stellaris MCP repository (`docs/gui_probe/`).

- The host: starts the engine's Dear ImGui (1.85), adds fonts, draws inside its frame through three hooks (`ImGui::NewFrame`, `NImGuiWrapper::ImGuiInit`,
  `CGameState::HandleTurnTick`).
- The public C interface `include/stellaris_guiexpand/stellaris_gui_api.h` (API version 1): panels (window and overlay), the game snapshot, script effects, speed and pause, log,
  and a C drawing table for plugins without ImGui. Helper headers for finding the host and for binding a plugin's own ImGui (with a layout check).
- Fault isolation for plugin panels: exceptions, unbalanced ImGui stacks, unloaded code, mismatched ImGui configuration.
- Panels declared by mods in `interface/stl_gui/*.txt` (`stl_gui_version = 1`).
- Plugin of the Stellaris launcher (manifest schema 2, settings in `config\stellaris_guiexpand.ini`), built for the `stellaris.exe` with PE timestamp `0x6ABEAA3F` (Stellaris 4.5.2).
- Examples: a plugin with its own ImGui, a plain C plugin.
- Scoped localisation: every text of a declared panel may contain `[Root.<variable>]`, `[Root.GetName]` or a `scripted_loc` (script values through one), evaluated by the
  engine for the player's country (`CGameText::ProcessWithScope`); `StlGuiApi::localize` for plugins (appended, API version unchanged). Research: `docs/gui_scoped_localisation.md`
  in the Stellaris MCP repository.

Differences from the prototype: the plugin id is `stellaris-guiexpand` and the log, settings and development files live in the plugin folder (`logs\`, `config\`); the demo button effects
are `guiexpand_test_*` and come from the stellaris-guiexpand-test-mod repository; the host's panel registry takes the window size and title kind as options instead of knowing about declared panels.

The developer guide, the mod author guide and the architecture notes are also in Chinese (`docs/*.zh-CN.md`).

Naming: the working name was `guidll`; before the first release it became `stellaris-guiexpand` (plugin id, repository, folder), `stellaris_guiexpand.dll`
(`stellaris_guiexpand.ini`, `logs\stellaris_guiexpand.log`), C++ namespace `guiexpand`, headers in `include/stellaris_guiexpand/`. The C interface keeps its `Stl` / `STL_` prefix and the
export `StlGui_GetApi`.
