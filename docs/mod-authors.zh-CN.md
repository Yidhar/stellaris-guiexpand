# 给你的 mod 加一个面板

[English](mod-authors.md) | [简体中文](mod-authors.zh-CN.md)

写给 **mod 作者**：不需要 DLL，不需要编程。你在自己 mod 里写一个文本文件，用游戏本来就在用的脚本语法，stellaris-guiexpand 会把它画成游戏里的一个窗口。玩家需要装了 stellaris-guiexpand 插件；没有它，你的这个文件会被忽略（游戏从不读这个文件夹）。

一个完整可运行的 mod 在 [stellaris-guiexpand-test-mod](https://github.com/Yidhar/stellaris-guiexpand-test-mod) 仓库里：照它的目录结构写就行。

```
my_mod/
  descriptor.mod
  common/button_effects/my_mod.txt          你的按钮执行的动作（普通的游戏脚本）
  interface/stl_gui/my_mod_panels.txt       面板声明（本指南讲的）
  localisation/english/my_mod_l_english.yml  文字
```

## 为什么是 `interface/stl_gui/`

引擎不读这个文件夹（实测：`error.log`、`game.log`、`setup.log`、`debug.log` 里一行都没有），而且 `interface/` 不参与多人校验：玩家的界面可以各不相同，不会失同步。你的**动作**仍然放在 `common/button_effects` 里，那里是参与校验的，所以每个客户端运行同一份脚本，引擎在每个客户端上重新检查 `potential` / `allow`。

## 文件

Paradox 脚本：`key = value`、`key = { ... }`、`#` 注释、带引号的字符串；一个块里键可以重复（多个 `button`），顺序保留。

```
stl_gui_version = 1                      # 必须有。没有，或者不是 1，整个文件被忽略（并写日志）

panel = {
    id    = overview                     # 在你的 mod 里唯一。在宿主里它是 "<mod 名>:<id>"
    title = MYMOD_TITLE                  # 本地化键；含空格的文字按原样显示
    size  = { 380 440 }                  # 可选：第一次打开时的大小（玩家可以改大小、拖动）

    content = {
        text      = { text = MYMOD_INTRO }
        separator = yes
        spacer    = 6                                                    # 像素
        date      = { label = MYMOD_DATE }
        value     = { label = MYMOD_ENERGY  resource = energy  show = stock }   # stock | net | income | expense | max
        gauge     = { label = MYMOD_MINERALS  resource = minerals }              # 库存对上限，画成进度条
        stat      = { label = MYMOD_COLONIES  stat = colonies }                  # colonies pops empire_size military_power tech_power economy_power
        badge     = { probe = my_probe_effect  yes = MYMOD_ON  no = MYMOD_OFF }  # 把一个 button_effect 当作是 / 否的问题
        row = {                                                          # 并排
            button = { text = MYMOD_SET    effect = my_set_effect }
            button = { text = MYMOD_CLEAR  effect = my_clear_effect }
        }
        button    = { text = MYMOD_GRANT  effect = my_grant_effect }
    }
}
```

一个文件里可以有多个 `panel`，一个 mod 里可以有多个文件。同一个 mod 里两个面板的 id 相同：保留先出现的，后面的被拒绝并写日志。

`panel` 还有这些键：

| 键 | |
|---|---|
| `kind = hud` | 不是窗口而是 **HUD**：没有标题栏、没有背景、不能拖动，由 `anchor` 和 `offset` 定位、`size` 定大小（默认 `kind = window`） |
| `anchor = ...` | HUD 用：`top_left`、`top_center`、`top_right`、`left_center`、`center`、`right_center`、`bottom_left`、`bottom_center`、`bottom_right` |
| `offset = { x y }` | HUD 用：离所贴边缘向内的像素距离（居中时是离中线的距离） |
| `hotkey = "ctrl+shift+g"` | 游戏窗口有焦点时，按这个键显示 / 隐藏面板：`ctrl`、`shift`、`alt` 加一个字母、数字、`f1` .. `f12`、`space`、`tab`、`enter`、`esc`。如果机器用 `ctrl+shift` 切换键盘布局，不要用这个组合 |
| `open = no` | 面板一开始是隐藏的（默认 `yes`） |

文件里可以在 `stl_gui_version` 旁边写 `stl_gui_requires = { 某个插件 id }`：声明这个文件用到哪些插件的元素（见下）。它只是在缺少插件时让提示更清楚。

### 元素

| 元素 | 显示什么 | 字段 |
|---|---|---|
| `text` | 一段文字 | `text`（本地化键） |
| `separator` | 一条分隔线 | `yes` |
| `spacer` | 垂直空白 | 像素数 |
| `date` | 游戏日期 | `label` |
| `value` | 一种资源的一个数 | `label`、`resource`（`common/strategic_resources` 里的键，例如 `energy`）、`show` = `stock`（默认）、`net`（每月）、`income`、`expense`、`max` |
| `gauge` | 库存对上限 | `label`、`resource` |
| `stat` | 国家的一项数据 | `label`、`stat` = `colonies`、`pops`、`empire_size`、`military_power`、`tech_power`、`economy_power` |
| `badge` | 带颜色的是 / 否 | `probe`（一个 button effect）、`yes` / `no`（本地化键） |
| `button` | 执行一个 effect 的按钮 | `text`（本地化键）、`effect`（你的 `common/button_effects` 里的键） |
| `row` | 让里面的 `button` 并排 | 各元素 |
| *其他任何名字* | **插件的元素**（见下） | 由插件的文档说明 |

数字是**玩家国家**的，在回合 tick 之间取得。

### 插件的元素：组件

插件可以注册更多元素。组件库（一个注册了 `card`、`tabs`、`ring`、`chart`……的插件）的用法和内置元素一样，语法相同，所以面板可以不只是一列标准控件。插件会说明它的元素和插件 id；你把这个 id 写进 `stl_gui_requires`，缺少时玩家就能知道缺什么：

```
stl_gui_version = 1
stl_gui_requires = { some-component-plugin }

panel = {
    id = status
    title = MYMOD_TITLE
    content = {
        card = {                                          # 插件的一个元素
            title = MYMOD_CARD
            content = {                                   # 卡片里显示的内容：由宿主画，内置元素和插件的元素都行
                ring = { resource = energy }              # 同一个插件的另一个元素
                value = { label = MYMOD_ENERGY  resource = energy  show = net }
            }
        }
    }
}
```

插件没装时，这个元素被一条暗淡的 `[card: needs some-component-plugin]` 取代，日志里写一次；周围的元素照常工作。元素崩溃三次后同样被停用（`[card: disabled]`）。

### 按钮和引擎自己的检查

`button` 的 `effect` 是你的 `common/button_effects` 里一个条目的名字。只有当**引擎**认为这个 effect 现在可以执行时，按钮才可点：`potential` 和 `allow` 由引擎自己求值，它拒绝时按钮变灰，提示文字就是引擎自己给出的原因。点击后，effect 经游戏自己的命令路径、以玩家国家执行（`This` 和 `From` 都是玩家国家），和游戏自己窗口里的按钮完全一样。

```
# common/button_effects/my_mod.txt
my_set_effect = {
    potential = { always = yes }
    allow     = { NOT = { has_country_flag = my_flag } }
    effect    = { set_country_flag = my_flag }
}
```

### 显示任何触发器能判断的东西：`badge` 的用法

`badge` 不读变量或旗标。它问引擎某个 `button_effect` 现在是否允许，然后显示 `yes` 或 `no` 那句话。所以要显示"旗标已设置"，就声明一个 `allow` 是你想要的条件、`effect` 为空的 effect，让 badge 指向它：

```
my_flag_is_set = { potential = { always = yes }  allow = { has_country_flag = my_flag }  effect = { } }
```
```
badge = { probe = my_flag_is_set  yes = MYMOD_FLAG_ON  no = MYMOD_FLAG_OFF }
```

任何触发器都可以这样用（旗标、科技、伦理、资源数量、事件目标……）。

## 显示脚本算出来的值

声明面板里的每一段文字（`text`、每个 `label`、按钮文字、`badge` 的两句话）都是本地化键，而本地化文字里可以写游戏自己的 `[...]` 命令。stellaris-guiexpand 让引擎对它们求值，scope 是**玩家国家**（`This`、`From`、`Root` 都指玩家国家），所以面板可以显示你的脚本算出来的东西：

![一个声明面板，显示国家名、存下来的变量、scripted_loc 和脚本值](images/scoped_panel.png)

| `.yml` 里写 | 显示 |
|---|---|
| `"Empire: [Root.GetName]"` | 国家名（以及游戏对国家支持的其他文字命令） |
| `"Counter: [Root.my_counter]"` | 国家的一个**存下来的变量**；脚本设置它之前是空的 |
| `"[Root.MyFlagText]"` | 一个 **scripted_loc**：`common/scripted_loc` 里的 `defined_text = { name = MyFlagText ... }`，由触发器选择文字 |
| `"Value: [Root.MyValue]"` | 一个**脚本值**，经过 scripted_loc：`defined_text = { name = MyValue  value = value:my_script_value }` |

下面是测试 mod（`stellaris-guiexpand-test-mod`）的面板，四行都在游戏里测过：

```
# common/script_values/guiexpand_test.txt
guiexpand_test_value = { base = 10  modifier = { add = 5  has_country_flag = guiexpand_test_marked } }

# common/scripted_loc/guiexpand_test.txt
defined_text = { name = GuiexpandTestValue  value = value:guiexpand_test_value }
defined_text = {
    name = GuiexpandTestFlag
    text = { trigger = { has_country_flag = guiexpand_test_marked }  localization_key = guiexpand_test_flag_on }
    default = guiexpand_test_flag_off
}

# localisation/simp_chinese/guiexpand_test_l_simp_chinese.yml
 GUIEXPAND_TEST_SC_COUNTER:0 "计数器（存下来的变量）：[Root.guiexpand_test_counter]"
 GUIEXPAND_TEST_SC_FLAG:0 "旗标（scripted_loc）：[Root.GuiexpandTestFlag]"
 GUIEXPAND_TEST_SC_VALUE:0 "脚本值：[Root.GuiexpandTestValue]"
```

要知道的事：

- 值在**回合 tick 之间**取得，游戏状态变化后会再取一次，所以它跟着你的脚本走（改变量的按钮 effect 执行后，过一小会儿就能看到，暂停时也一样）。
- 得到的是**文字**，不是数字：想要什么格式，在 `.yml` 或 `scripted_loc` 里自己写。
- scope 只有玩家国家。选中的行星或舰队暂时用不了。
- 文字里没有 `[` 的键不会有额外开销。求一次值大约一微秒。
- 这是引擎自己的文字处理器，所以游戏的本地化在国家 scope 下能做的事这里都能做，不能做的这里也不能。引擎对命令里**写错**（`[Root.nonsense]`）有什么反应，还没有测过。

## 本地化

标题、文字和标签都是你的 `localisation/<语言>/*.yml` 里的键，由游戏自己的本地化查找，所以会跟随语言设置，你的翻译和任何 mod 的一样起作用。别忘了引擎的规定：本地化文件必须是 **UTF-8 带 BOM**。

含空格的值，或者游戏不认识的键，按原样显示。

**已知限制：字符。** 引擎的字体图集在 ImGui 启动时一次建成，之后不能再加字形。stellaris-guiexpand 的图集里有常用汉字、拉丁字母和它自己界面用到的字符；你的文字里的生僻字可能显示成 `?`。修复办法（在建图集之前扫描已启用 mod 的本地化文件里的字符）已经列入计划。

## 出错了会怎样

文件级别的错误写进 stellaris-guiexpand 的日志（插件文件夹里的 `logs\stellaris_guiexpand.log`），不会显示在游戏里：

| 错误 | 结果 |
|---|---|
| 语法错误（括号不配对……） | 整个文件被忽略；日志里写出文件名 |
| 没有 `stl_gui_version = 1` | 文件被忽略；写日志 |
| 面板没有 `id`，或者 mod 里已有同 id 的面板 | 该面板被忽略；写日志 |
| 不认识的元素（没人注册的块） | 面板里一条暗淡的 `[name: needs ...]`，日志里写一行；请检查拼写 |
| 不认识的普通 `key = value` | 悄悄跳过（它可能是外面那个条目的参数） |
| 不认识的 `effect`（按钮） | 按钮显示为不可用 |
| 不认识的 `stat` 或 `resource` | 数值显示为 `?` |

不在游戏里时，声明面板显示 *not in a game*。

开发时，`config\stellaris_guiexpand.ini` 里有 `extra_mod_dirs`（在当前播放集的 mod 之外额外扫描的文件夹）；设置 `dev_commands=1` 后，往 `logs\stellaris_guiexpand.cmd` 里写一行 `scan`，就能不重启游戏重新读取所有声明文件。重新扫描后窗口的位置和状态保持不变。

测试 mod 的 `tools/check_mod.py` 检查那些拼错了也不会报错的东西：用到的每个本地化键在每种语言里都存在、每个被引用的 effect 都存在、scripted_loc 里的键和脚本值存在、`[Root.名字]` 引用的 scripted_loc 存在。

## 多人

声明文件不参与校验，所以每个玩家的界面互相独立。动作是游戏自己的命令，由每个客户端校验。还没有在多人游戏里测过。
