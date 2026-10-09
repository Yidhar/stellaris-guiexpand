# 写一个通过 stellaris-guiexpand 显示面板的插件

[English](developers.md) | [简体中文](developers.zh-CN.md)

写给 Stellaris 启动器插件（DLL）的开发者。如果你想从 **mod** 里加面板（不写 DLL），请看 [mod-authors.zh-CN.md](mod-authors.zh-CN.md)（[English](mod-authors.md)）。

你给宿主一个函数，就能在游戏里得到一个窗口，用引擎自己的 Dear ImGui 画。宿主每帧在引擎的 ImGui 帧里、在主线程上调用它。你不用挂渲染器的钩子，不用读引擎内存，你的插件也不绑定游戏版本：游戏更新后更新的是**宿主**，你的插件不用重新编译。

整个接口是 `include/stellaris_guiexpand/stellaris_gui_api.h`（纯 C）。旁边有两个小的辅助头文件。能直接运行的插件在 `examples/` 里。

## 规则

- 边界上只用 C：不传 C++ 类型，也不跨 DLL 边界转移所有权（用 `/MT` 构建你的插件，谁也不释放别人的内存）。
- 每个结构体都以 `uint32_t size` 开头。调用方填它编译时的 `sizeof`；接收方只读写 `size` 以内的部分。新成员只会追加。老插件配新宿主、新插件配老宿主都能工作。
- 字符串是 UTF-8，以 NUL 结尾。接收方需要保留的内容自己复制。
- 你的回调在**主线程**上、在引擎的 ImGui 帧里运行。要短（每帧都会运行）。接口里除了 `register_panel` / `unregister_panel` 之外，其他都**不是线程安全**的：只能在回调里调用。
- 宿主只通过你注册的回调调用你的插件。

## 找到宿主

宿主 DLL（`stellaris_guiexpand.dll`，插件 id `stellaris-guiexpand`）导出一个函数：

```c
const StlGuiApi* StlGui_GetApi(uint32_t requested_version);   // 不支持该版本时返回 NULL
```

启动器没有加载顺序或依赖字段，所以**不要假设宿主比你先加载**。`stellaris_gui_client.h` 把发现过程做成了一个调用；请在**你自己的线程**里轮询（绝不要在 `DllMain` 里，它持有加载器锁）：

```c
#include "stellaris_gui_client.h"

static DWORD WINAPI ConnectThread(LPVOID) {
    const StlGuiApi* api = NULL;
    while (!(api = stl_gui_try_connect(STL_GUI_HOST_DLL, STL_GUI_API_VERSION))) Sleep(250);   // GetModuleHandleW + GetProcAddress
    // 在这里注册面板
    return 0;
}
```

实测：比宿主先注入的插件，在宿主出现后 250 毫秒内就找到了它，面板在宿主的 ImGui 启动之前就已注册好。如果宿主始终没有加载（玩家禁用了它），你的线程就一直空转，没有开销。想的话可以记一次日志然后停止轮询。

## 注册面板

```c
StlGuiPanelDesc d = { sizeof(d) };
d.flags = STL_PANEL_WINDOW;           // 或 STL_PANEL_OVERLAY
d.id    = "myplugin.status";          // 稳定且唯一："<你的插件 id>.<面板>"
d.title = "My plugin";
d.draw  = DrawStatus;                 // void DrawStatus(const StlGuiCallbackCtx* ctx, void* user)
d.user  = NULL;
int handle = api->register_panel(&d); // > 0；id 已被占用或没有回调时返回 0
```

| 标志 | 宿主的做法 |
|---|---|
| `STL_PANEL_WINDOW` | 在你的回调外面开一个标题为 `title` 的窗口，并保证关闭；你的回调只画内容。可移动、可关闭（关闭只是隐藏） |
| `STL_PANEL_OVERLAY` | 调用你的回调时不开任何东西：想画什么都行（用 `GetForegroundDrawList` 画 HUD，自己开窗口）。之后宿主会复原 ImGui 的栈 |

回调收到一个 `StlGuiCallbackCtx`：API 版本、引擎的 ImGui 上下文和分配器对、引擎的 ImGui 版本、四个 ImGui 类型的 `sizeof`（下面的绑定要用）、一张 C 绘制函数表（`ctx->ui`）、宿主的函数表（`ctx->api`），以及三种字体（`font_body`、`font_bold`、`font_numbers`，类型是 `ImFont*`）。上下文指针**每次调用都会传**：引擎可能重启 ImGui，会换一个新的。

## 三种画法

| | A：自带 ImGui | B：C 函数表（`ctx->ui`） |
|---|---|---|
| 你要带什么 | ImGui **1.85**（默认的 `imconfig.h`）和 `stellaris_gui_imgui.hpp` | 只要 `stellaris_gui_api.h` |
| 语言 | C++ | 任何能调 C 函数的语言（C、Rust、Zig、通过 ctypes 的 Python……） |
| 控件 | 整个 ImGui、自定义控件、`ImDrawList`、你自己的字体 | 文字、彩色文字、按钮、复选框、滑块、进度条、提示、同行、分隔线、光标位置和可用区域、占位，以及 `DrawList` 的线 / 矩形 / 圆 / 文字 |
| 是否绑定引擎的 ImGui 版本 | **是**：版本和配置都要一样，否则绑定拒绝绘制 | 否：接口不暴露 ImGui |

### A：自带 ImGui

把 ImGui 1.85 编进你的插件（静态链接，`/MT`），然后在每个回调的开头把它绑定到引擎的上下文：

```cpp
#include "imgui.h"
#include "stellaris_gui_api.h"
#include "stellaris_gui_imgui.hpp"

static void DrawStatus(const StlGuiCallbackCtx* ctx, void*) {
    if (!StlGuiBindImGui(ctx)) {                 // 设置分配器和当前上下文；检查布局
        static bool said = false;
        if (!said) { said = true; ctx->api->log("myplugin", StlGuiBindFailure()); }
        return;                                  // 宁可不画，也不要破坏共享的上下文
    }
    ImGui::Text("Hello from my plugin");
    if (ImGui::Button("Pause")) ctx->api->set_paused(1);
}
```

`StlGuiBindImGui` 把引擎的 `ImGuiIO`、`ImGuiStyle`、`ImDrawVert`、`ImDrawIdx` 的大小和版本号，与你编译时的这份拷贝逐一比对。实测：用 `#define ImDrawIdx unsigned int` 编译的插件会拒绝绘制，提示 *"sizeof(ImDrawIdx) differs from the engine's"*，其他面板照常绘制（构建里的 `example_imgui_badcfg` 就是这样一个插件）。

如果引擎的 ImGui 版本将来变了，这类插件必须针对新版本重新编译（在那之前它们会被拒绝，不会崩溃）。B 类插件不受影响。

### B：C 函数表

```c
static void DrawStatus(const StlGuiCallbackCtx* ctx, void* user) {
    const StlGuiUi* ui = ctx->ui;
    ui->text("Hello from a plain C plugin");
    if (ui->button("Pause")) ctx->api->set_paused(1);
}
```

`examples/c/consumer_c.c` 是一个完整的插件，里面完全没有 ImGui。

## 游戏数据和动作

全部通过 `ctx->api`，在回调里调用：

| 调用 | |
|---|---|
| `get_snapshot(&snap)` | 日期、速度、暂停、玩家国家的 id 和名字，以及最多 32 种资源（`key`、`stock`、每月 `net`、`max`，`< 0` 表示没有上限）。它在回合 tick 之间取得，所以回调读到的永远是一致的一份；`snap.tick` 每个 tick 都会变。游戏还没运行时返回 0 |
| `effect_state(key, reason, cap)` | 某个 mod 的 `common/button_effects` 条目：`1` 现在可以执行，`0` 引擎拒绝（**引擎自己的原因文字**写进 `reason`），`-1` 未知。`potential` 和 `allow` 由引擎求值 |
| `post_effect(key)` | 把这个 effect 排到下一个安全的时刻，经引擎自己的命令（`CExecuteButtonEffectCommand`），以玩家国家执行。排上队就返回 1。引擎会再检查一次。已在单人游戏中验证 |
| `set_speed(n)` / `set_paused(b)` | 游戏自己的 setter，在 tick 之间生效 |
| `log(plugin_id, line)` | 往 stellaris-guiexpand 的日志写一行，加上你的 id 作前缀 |
| `localize(key, out, cap)` | *（首个版本之后追加的：请先检查 `api->size >= offsetof(StlGuiApi, localize) + sizeof(void*)`）* 把游戏的一个本地化键（例如某个 mod 的）变成给玩家国家看的显示文字。文字里可以有 `[Root.my_variable]`、`[Root.GetName]` 或 `scripted_loc`，由引擎求值：这就是显示 mod 脚本算出来的东西的办法。每帧调用也很便宜（值在 tick 之间、游戏状态变化后刷新）。返回复制的字节数；含空格的文字原样返回 |

一个 button effect 也可以当作**问题**：写一个 `effect = { }` 为空、在 `potential` / `allow` 里写任意触发器的 effect，`effect_state` 会告诉你这些触发器是否成立。mod 就是这样让你知道"这个旗标设置了吗？""玩家有这项科技吗？"，而你不用读任何内存。

接口没有提供的东西（舰队、行星……），需要你自己带 SDK 去读引擎内存。这是允许的，宿主不会阻止；但这也意味着你的插件又和游戏版本绑在一起了。

## 故障被隔离

宿主保护游戏不被行为异常的面板拖垮。以下都在游戏里实测过：

| 你的回调…… | 宿主的做法 |
|---|---|
| 抛出异常（空指针） | 用 SEH 捕获，记录异常码，复原 ImGui 的栈，游戏继续运行；**三次故障后停用该面板** |
| 留下没配对的 ImGui 栈（窗口、分组、颜色、样式变量、字体） | 比较调用前后的栈，复原多出来的部分；你后面的面板不受影响 |
| 没有注销就被卸载 | 每次调用前检查回调地址是否仍是已提交的可执行内存（`VirtualQuery`），否则摘除该面板 |
| 使用了不同的 ImGui 配置 | 你的绑定拒绝绘制（见上） |

防不了的：破坏堆或 ImGui 内部状态、死循环、在回调里长时间阻塞。

启动器从不卸载插件，插件也不能自己卸载自己，所以 `unregister_panel` 和示例里的卸载事件只是开发时用的辅助手段。

## 版本

`StlGuiApi::version` 和 `STL_GUI_API_VERSION` 只有在删除东西或改变含义时才会变（追加成员不会改变它）。你请求一个版本；宿主返回函数表或 `NULL`，返回 `NULL` 时请安静地降级（写日志，不注册）。表里的 `game_exe_timestamp` 说明宿主是为哪个 `stellaris.exe` 构建的。

如果你的插件只用这个接口，清单里可以不写 `game.exe_timestamps`：启动器的规范说，只有"在 exe 里定位地址"的插件才只加载进它列出的版本，没有列出时不检查。这个限制由宿主替你承担。

## 试试示例

`examples/cpp_imgui/consumer_imgui.cpp` 和 `examples/c/consumer_c.c` 随主项目一起构建（`tools\build.bat`：`example_imgui.dll`、`example_c.dll`、`example_imgui_badcfg.dll`）。想不经启动器在运行的游戏里看到它们，用 `python tools/live/guiexpand_test.py` 准备并注入（细节在脚本开头的说明里）。每个示例的 DLL 旁边的 `consumer_<tag>.cmd` 文件支持 `fault`（在回调里制造一次访问违规）和 `leak`（留下没配对的 ImGui 栈），可以用来观察宿主的保护是怎么工作的。
