# stellaris-guiexpand 是怎么工作的

[English](architecture.md) | [简体中文](architecture.zh-CN.md)

写给要改宿主的人。这些选择背后的研究（试过什么、什么崩了）在 Stellaris MCP 仓库里：`docs/gui_imgui_feasibility.md`、`docs/gui_native_system.md`、`docs/gui_plugin_api_investigation.md`。

## 基本思路

`stellaris.exe` 里编译进了 Dear ImGui 1.85（带 DX11 和 Win32 后端），用来做它的调试视图，平时是关着的。stellaris-guiexpand 把它启动起来，往它的字体图集里加自己的字体，然后在引擎的 ImGui 帧里画东西。结果由引擎的后端渲染；stellaris-guiexpand 自己没有渲染器，也不创建窗口或交换链。

## 文件

| 文件 | 职责 |
|---|---|
| `src/internal.h` | 各文件共用的声明，命名空间 `guiexpand` |
| `src/dllmain.cpp` | `DllMain` 只启动一个线程（它在加载器锁下、在启动器的远程线程上运行） |
| `src/imgui_host.cpp` | 钩子、每帧的处理、启动引擎的 ImGui、字体、开发用的命令文件、`Start()` |
| `src/core.cpp` | 插件文件夹、日志、设置；受保护的引擎内存读取；游戏快照；脚本通道；延后执行的动作 |
| `src/host_api.cpp` | 面板注册表、C 绘制函数表、`StlGui_GetApi`、带故障隔离的分发 |
| `src/decl_panels.cpp` | 发现 mod、脚本语法的解析器、声明面板的渲染器 |
| `src/loc.cpp` | 本地化键经引擎的本地化变成显示文字；带 `[Root.xxx]` 的文字经引擎的带作用域文字处理器 |
| `src/deck.cpp` | 参考皮肤（状态胶囊和 Command Deck）；和任何插件一样，是同一份数据的使用者 |
| `sdk/stellaris_sdk.hpp` | 生成的；宿主用到的所有引擎地址和偏移 |

## 三个钩子，没有第四个

| 钩子 | 原因 |
|---|---|
| `ImGui::NewFrame` | 原函数返回后，引擎的帧是打开的：stellaris-guiexpand 在这里画所有东西。它在主线程上、每渲染一帧运行一次 |
| `NImGuiWrapper::ImGuiInit` | 引擎在里面一次性建好字体图集。stellaris-guiexpand 趁图集还开着加入自己的字体（先加 ImGui 的默认字体，因为第一个加入的字体会成为 `io.FontDefault`，引擎自己的视图依赖它），并设置窗口句柄（见下） |
| `CGameState::HandleTurnTick` | 标记"正在 tick 里"，使快照只在 tick 之间取；游戏跑过一阵 tick 之后，也在这里启动引擎的 ImGui |

**刻意不钩 `Present`。** 每当创建交换链，Steam 叠加层都会重新修补交换链共享的 vtable 槽；在这个槽上再加一个钩子，会造成无限递归和栈溢出。`NewFrame` 钩子给出同样的"每帧一次"的机会，又不和任何东西共用一个槽。

## 启动引擎的 ImGui

`ImGuiInit` 用 `GetActiveWindow()` 找游戏窗口。在后台任务里，或者游戏窗口不是活动窗口时，它返回空，Win32 后端就没有窗口（没有输入，画面也是坏的）。stellaris-guiexpand 自己设置 `io.ImeWindowHandle` 和后端的窗口句柄。如果 ImGui 在 stellaris-guiexpand 之前就已启动（比如用控制台的 `imgui on` 手动启动），它的图集已经建好，没法再加 stellaris-guiexpand 的字体，所以 stellaris-guiexpand 通过控制台把它重启（`imgui off`、`imgui on`）；诊断时用 `logs\norestart` 可以跳过这一步。

引擎的 ImGui 和 stellaris-guiexpand 自带的那份是同一版本的两次编译。它们共用上下文，所以 stellaris-guiexpand 用引擎的分配器对调用 `SetAllocatorFunctions`（一份分配的内存由另一份释放），并且每帧调用 `SetCurrentContext`（引擎可能重启 ImGui，换成新的上下文）。`imgui_host.cpp` 里的 `static_assert` 把 `sizeof(ImGuiContext)` 和 `ImGuiIO` 各成员的偏移，与 SDK dumper 从引擎自己的代码里读出的常量做比对：如果引擎的 ImGui 是另一个构建，**编译**就会失败，而不是游戏在运行时出问题。

## 数据：tick 之间的快照

大约三分之一的绘制回调发生在 `HandleTurnTick` **里面**，那时游戏状态正在被修改。stellaris-guiexpand 只在没有 tick 运行时读取状态，并把结果存成快照；回调读到的永远是一致的一份。原始读取都经过受 SEH 保护的辅助函数：坏指针返回默认值，绝不会让游戏崩溃。定点数会被规范化（引擎里时间和进度放大了 100000 倍）。

## 动作：脚本通道

一个按钮会构造一个引擎命令 `CExecuteButtonEffectCommand`（大小 `0x198`：scope 在 `+0x20`，effect 指针在 `+0x190`，取自引擎的 `CButtonEffectDatabase`），问引擎自己的 `IsValid`（它用引擎自己的话给出拒绝原因），然后在下一个安全的时刻用 `PostCommandToSession` 投递。暂停时也能工作。它就是游戏自己的按钮用的命令，所以每个客户端都会再次检查 `potential` / `allow`。

## 作用域本地化

含有 `[` 的本地化文字，会连同玩家国家的 `CEventScope` 一起交给 `CGameText::ProcessWithScope(result, text, scope)`，于是 `[Root.some_variable]`、`[Root.GetName]` 和 `scripted_loc`（经它显示脚本值）都由引擎求值。scope 由引擎构造在一个 `CExecuteButtonEffectCommand` 对象里（就是脚本通道要填的那个）；stellaris-guiexpand 只把这个对象当作容器，不投递，用完直接销毁。文字处理器会读游戏状态，所以只在回合 tick 之间运行，每个快照、每个键求一次（每次约一微秒）；在 tick 里运行的回调拿到的是上一次的值。研究、调用约定和指纹见 Stellaris MCP 仓库的 `docs/gui_scoped_localisation.md`。

## 面板和故障隔离

`register_panel` 可以在任意线程调用（注册表有锁）；分发发生在主线程。每次调用都被包起来：用 SEH 处理异常，比较前后 ImGui 的窗口、分组、颜色、样式变量和字体栈（多出来的弹掉），用 `VirtualQuery` 检查回调的代码是否仍然映射着，并统计每个面板的故障次数（三次就停用）。表格见 `docs/developers.zh-CN.md`。这个包装函数里没有带析构函数的 C++ 对象（SEH 和 C++ 栈展开不能在同一个函数里混用）。

## 声明面板

在游戏第一次运行时（以及收到 `scan` 时），stellaris-guiexpand 读 `dlc_load.json`，通过 `path=` 把每个启用的 `mod/*.mod` 解析成它的文件夹，再用一个小的 Paradox 脚本解析器解析 `interface/stl_gui/*.txt`。每个声明面板像插件的面板一样被注册，渲染器每帧遍历解析出来的树。声明唯一能做的就是显示数据和执行一个 `button_effect`：没有表达式语言。

## 参考皮肤

`deck.cpp` 主要用 `ImDrawList` 的调用、而不是 ImGui 自带的控件，画出状态胶囊和 Command Deck，风格和游戏自己的相去甚远。它是同一份快照和脚本通道的一个普通使用者；`deck = 0` 可以把它完全关掉。它是最大的文件，将来可能拆成独立的插件。

## 游戏更新之后

1. 在 Stellaris MCP 仓库：`python tools/sdk_dumper/dump.py`（约 90 秒；有运行中的游戏时会对照它验证）。
2. 在本仓库：`python tools/extract_sdk.py <路径>/stellaris_sdk.hpp`，它只复制源码用到的符号，缺任何一个就报错。
3. `tools\build.bat`；如果引擎的 ImGui 变了，`static_assert` 会失败。
4. 更新 `plugin/stl-plugin.json` 里的 `game.exe_timestamps`，运行 `tools/check_plugin.py`（它会和 SDK 的时间戳比对）。
5. 做一遍游戏里的回归测试（`tools/live/guiexpand_test.py`，见 `README.md`）。
