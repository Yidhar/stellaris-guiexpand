# guidll

[English](README.md) | [简体中文](README.zh-CN.md)

**Stellaris 4.5.2**（Windows x64，`-dx11` 版）的公共 GUI 宿主。它启动 `stellaris.exe` 里本来就编译进去的 Dear ImGui，在游戏里画面板：

- **插件开发者**用一个很小的 C 接口（`include/guidll/stellaris_gui_api.h`）就能显示面板：C、带或不带自己 ImGui 的 C++、任何能调用 C 函数的语言都行。不用挂钩子，不读引擎内存，游戏更新后也不用重新编译；
- **mod 作者**在自己 mod 的文本文件里（`interface/stl_gui/*.txt`，用游戏自己的脚本语法）描述一个面板，宿主负责画：带本地化，按钮执行 mod 自己的 `button_effect`。不需要 DLL，不需要编程；
- **玩家**在一个地方看到所有已装插件的面板，外加一个可选的参考皮肤：状态胶囊和 *Command Deck*。

它是 **Stellaris 启动器**的一个插件（插件规范 v2），不需要 mod，不改存档。

![三种来源的面板：自带 ImGui 的插件、纯 C 的插件、mod 声明的面板](docs/images/three_sources.png)

*从左到右：自带 ImGui 的插件；没有任何 ImGui 的纯 C 插件；mod 的文本文件声明的面板。*

| 发布文件 | 内容 |
|---|---|
| `guidll-v<版本>.zip` | **插件文件夹本身**：`stl-plugin.json`、`guidll.dll`、`defaults\guidll.ini`、本说明 |
| `guidll-sdk-v<版本>.zip` | C 接口头文件、示例插件和两份指南，给其他插件的开发者 |

## 你是谁，读哪里

| 你是 | 读 |
|---|---|
| 玩家 | 本页：[安装](#安装和使用)、[设置](#设置) |
| **插件开发者** | [docs/developers.md](docs/developers.md) 和 `examples/` |
| **mod 作者** | [docs/mod-authors.md](docs/mod-authors.md)；可运行的 mod 见 [guidll-test-mod](https://github.com/Yidhar/guidll-test-mod) 仓库 |
| 想知道原理 | [docs/architecture.md](docs/architecture.md) |

## 兼容性

- 只适配**一个确切的游戏版本**：`stellaris.exe` 的 PE 时间戳写在清单（`game.exe_timestamps`）和发布说明里。启动器不会把插件加载进别的版本，DLL 自己也会再核对一次，不符就什么都不挂。游戏更新后要重新生成 SDK 子集并重新编译（见[构建](#构建)）。
- 使用 guidll 接口的插件和 mod **不依赖游戏版本**，只有宿主依赖。
- 引擎地址不是写死的：来自从已安装的可执行文件生成的 SDK（`sdk/stellaris_sdk.hpp`，由 `tools/extract_sdk.py` 从 [Stellaris MCP](https://github.com/Yidhar/stellaris-mcp) 仓库的生成器输出里抽出的子集）。
- 测过的是单人。声明面板和插件的按钮通过引擎自己的命令路径（游戏自己的按钮用的同一个 `CExecuteButtonEffectCommand`）执行脚本 effect，每个客户端都会校验；多人没测过。

## 安装和使用

1. 从 [Releases](https://github.com/Yidhar/guidll/releases) 下载 `guidll-v<版本>.zip`（旁边的 `.sha256` 是校验和）。
2. 解压到 `Documents\Paradox Interactive\Stellaris\plugins\guidll\`（zip 没有顶层文件夹：`stl-plugin.json` 直接在该文件夹里），或者解压到任意位置，用启动器安装：`stl plugin install <文件夹>`。
3. 在播放集里启用（`stl plugin enable guidll`，或启动器的插件页）。
4. **用 Stellaris 启动器启动游戏**（`stl launch` 或它的“开始”按钮）。启动器等游戏窗口出现后加载插件。从 Steam 或 Paradox 启动器启动的游戏没有插件。
5. 进入游戏后，屏幕底部有状态胶囊。点它的圆环或按 **Ctrl + Shift + G** 打开 Command Deck。

其他插件和 mod 的面板显示在屏幕左侧的窗口里，可以用 ImGui 的常规操作拖动和关闭。

## 设置

插件文件夹里的 `config\guidll.ini`（启动器用 `defaults\guidll.ini` 生成，并提供编辑；改了要重启游戏）：

| 键 | 默认 | 含义 |
|---|---|---|
| `deck` | 1 | 参考皮肤（胶囊和 Command Deck）。`0` = 只做宿主：插件和 mod 的面板照常显示 |
| `deck_open` | 0 | 启动时就打开 Deck 窗口 |
| `theme` | 0 | 0 极光、1 余烬、2 翠绿、3 绯红 |
| `stars` | 1 | Deck 背景里的星空 |
| `extra_mod_dirs` | | 除当前播放集的 mod 外，额外扫描声明面板的文件夹（用 `;` 分隔）：开发 mod 时不用启用它 |
| `dev_commands` | 0 | 开发用：宿主执行 `logs\guidll.cmd` 里的行（`tools/live/guidll_test.py` 用） |
| `dev_unload` | 0 | 开发用：通过事件卸载，试新版本不用重启游戏。正式发布的插件不能自己卸载 |

日志是插件文件夹里的 `logs\guidll.log`。不会往游戏文件夹里写任何东西。

## 目录

```
include/guidll/    公共接口：stellaris_gui_api.h（C）、stellaris_gui_client.h（找到宿主）、stellaris_gui_imgui.hpp（自带 ImGui 的绑定）
examples/          cpp_imgui：自带 ImGui 的插件；c：纯 C 插件
src/               宿主（core、host_api、decl_panels、deck、imgui_host、loc、dllmain）
sdk/               stellaris_sdk.hpp：生成的子集（所写 exe 版本的 RVA 和偏移）
plugin/            stl-plugin.json 和 defaults\guidll.ini
tools/             build.bat、check_plugin.py、extract_sdk.py、gen_ui_glyphs.py、live/guidll_test.py
docs/              developers.md、mod-authors.md、architecture.md
```

## 构建

Visual Studio 2022（x64）和 CMake 3.20。MinHook 和 Dear ImGui 1.85 由 CMake 下载。

```
tools\build.bat [vcvars64.bat 的路径]     # -> build\plugin\guidll（安装：stl plugin install --link build\plugin\guidll）
python tools\check_plugin.py --dir build\plugin\guidll
```

游戏更新后：在 Stellaris MCP 仓库里运行 `tools/sdk_dumper/dump.py`，然后 `python tools/extract_sdk.py <该仓库>\stellaris_bridge\include\sdk\stellaris_sdk.hpp`，重新编译。如果引擎的 ImGui 的布局和本 DLL 编译所用的不一致，`src/imgui_host.cpp` 里的 `static_assert` 会让构建失败。

`python tools/gen_ui_glyphs.py` 重新生成 `src/ui_glyphs.inc`（界面文字用到的字符；引擎只在启动时建一次字体图集，之后不能再加字形）。它过期时 CI 会失败。

### 测试

没有单元测试框架：宿主跑在游戏里。`tools/live/guidll_test.py` 准备好插件文件夹，把宿主和示例插件注入运行中的游戏（用 stellaris-perf 仓库的 bench 脚本和启动器的 `stl inject`，路径见文件头），并驱动宿主的开发命令。guidll-test-mod 仓库里的测试 mod 提供按钮 effect 和一个声明面板。

## 许可

MIT，见 `LICENSE`。Dear ImGui（MIT）和 MinHook（BSD-2-Clause）在构建时下载并静态链接。
