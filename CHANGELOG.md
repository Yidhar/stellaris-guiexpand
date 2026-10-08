# Changelog

## 0.1.0 (unreleased)

The first version as its own repository. Everything before it was a prototype inside the Stellaris MCP repository (`docs/gui_probe/`).

- The host: starts the engine's Dear ImGui (1.85), adds fonts, draws inside its frame through three hooks (`ImGui::NewFrame`, `NImGuiWrapper::ImGuiInit`,
  `CGameState::HandleTurnTick`).
- The public C interface `include/stellaris_guiexpand/stellaris_gui_api.h` (API version 1): panels (window and overlay), the game snapshot, script effects, speed and pause, log,
  and a C drawing table for plugins without ImGui. Helper headers for finding the host and for binding a plugin's own ImGui (with a layout check).
- Fault isolation for plugin panels: exceptions, unbalanced ImGui stacks, unloaded code, mismatched ImGui configuration.
- Panels declared by mods in `interface/stl_gui/*.txt` (`stl_gui_version = 1`).
- The reference skin: status capsule and Command Deck (overview, economy, time, script, settings; four themes).
- Plugin of the Stellaris launcher (manifest schema 2, settings in `config\stellaris_guiexpand.ini`), built for the `stellaris.exe` with PE timestamp `0x6ABEAA3F` (Stellaris 4.5.2).
- Examples: a plugin with its own ImGui, a plain C plugin.
- Scoped localisation: every text of a declared panel may contain `[Root.<variable>]`, `[Root.GetName]` or a `scripted_loc` (script values through one), evaluated by the
  engine for the player's country (`CGameText::ProcessWithScope`); `StlGuiApi::localize` for plugins (appended, API version unchanged). Research: `docs/gui_scoped_localisation.md`
  in the Stellaris MCP repository.

Differences from the prototype: the plugin id is `stellaris-guiexpand` and the log, settings and development files live in the plugin folder (`logs\`, `config\`); the demo button effects
are `guiexpand_test_*` and come from the stellaris-guiexpand-test-mod repository; the host's panel registry takes the window size and title kind as options instead of knowing about declared panels.

Naming: the working name was `guidll`; before the first release it became `stellaris-guiexpand` (plugin id, repository, folder), `stellaris_guiexpand.dll`
(`stellaris_guiexpand.ini`, `logs\stellaris_guiexpand.log`), C++ namespace `guiexpand`, headers in `include/stellaris_guiexpand/`. The C interface keeps its `Stl` / `STL_` prefix and the
export `StlGui_GetApi`.
