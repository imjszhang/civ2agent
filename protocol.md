# civ2agent 命令行接口

日期：2026-10-24，协议 2

agent 通过 `cli/civ2ctl.py` 观察和操作《文明 II》。注入游戏的 `civ2agent.dll` 在游戏的界面线程执行命令，命令行经命名管道 `\\.\pipe\civ2agent` 和它通信。agent 只调用命令行，不读进程内存，也不直接连管道。

## 三条规则

每个命令都按这三条实现：

1. 操作：只做玩家在游戏界面里能做的事。走游戏处理这次输入的路径（菜单命令、按键、对话框和窗口自己的处理函数），能不能做、结果是什么都由游戏决定，不改内存里的游戏数据。
2. 信息：玩家在画面、对话框和窗口里能看到的都给，看不到的不给，哪怕内存里有。
3. 画面：每次操作之后，画面和玩家亲手操作之后一样。

## 调用

```text
python civ2agent\cli\civ2ctl.py <命令> [参数]
python civ2agent\cli\civ2ctl.py call <命令> '<JSON 参数>'
python civ2agent\cli\civ2ctl.py tools
```

- 每次调用在 stdout 输出一行 JSON，一定有 `ok`。失败时还有 `error`（错误码，见下文）和 `reason`（中文说明）。
- 退出码：0 成功；1 游戏拒绝或失败；2 参数错误（`usage`）；3 连不上游戏或超时（`not_connected`、`timeout`）。
- `tools` 输出全部命令的 JSON Schema（`name`、`kind`、`description`、`input_schema`），可以直接作为 agent 的工具定义。`call` 按同一份定义校验参数；参数写 `-` 时从 stdin 读。
- `--timeout 秒`：等多久连上游戏，默认 10 秒，`status` 为 2 秒。连上以后等多久由命令自己决定。
- 先运行 `wait-ready`。它会检查游戏里的 DLL 和命令行是不是同一个协议版本，不是就返回 `protocol_mismatch`。
- 旧的命令名（`hello`、`snapshot`、`menu`、`act ...`）返回 `usage`，并说明新名字。

## 命令

查看，不改变游戏：

- `status`：`protocol`、`exe_hash`、`uia_loaded`、`in_game`、`limits_ok`、`multi_type`、`dialog_open`、`city_window`、`tax_window`。
- `state`：当前局面，见「局面」。`--out 文件` 另存一份。
- `dialog`：`in_game`、`city_window`、`tax_window` 和当前对话框 `dialog`，见「对话框」；没有对话框时为 `null`。主菜单和开局设置也是对话框。
- `events`：取走并清空上次以来的事件，见「事件」。

操作，每个对应玩家的一次操作：

- `respond --option N` 或 `respond --text 文字`，可加 `--edit 文字`：回答当前对话框，相当于玩家点选一项。`--edit` 先在输入框里填字（城市名、领袖名），再选这一项。输入框有长度上限（城市名 15 字节、领袖名 23 字节，一个汉字算 2 字节），超长或含输入框不收的字符时返回 `illegal`，什么也不填。
- `activate --unit N`：选中一个己方单位，相当于在地图上点它：睡眠或设防的单位会被叫醒，地图移到这一格。这一格有好几个己方单位时，游戏弹出单位列表，返回 `modal_open`；用 `respond --text` 选，选项的 `id` 就是单位的 `index`。本回合已经没有移动力的单位返回 `illegal`，不点它。走完一步后游戏会自动选下一个单位；这时 `order` 也要等下一回合。
- `move`、`goto`、`order`、`found-city` 会先像玩家一样点选这个单位（`activate` 的效果）。单位和别的单位在同一格时，先用 `activate` 选中它。
- `move --unit N --x X --y Y`：朝相邻一格走一步，相当于按一次方向键。可能引发战斗或对话框。
- `goto --unit N --x X --y Y`：这一回合朝目标一步步走，每一步是一次方向键，走不动就停。
- `order --unit N --order O`：单位菜单里的命令，`O` 为 `fortify`、`sleep`、`wait`、`skip`、`irrigate`、`road`、`mine`、`clean`。菜单里没有或是灰的就返回 `illegal`。
- `found-city --unit N`：用拓荒者或工程师建城，相当于点「建立新城市」。随后弹出的命名框用 `respond --text 确定`（可加 `--edit 城市名`）回答。
- `produce --city N`，可加 `--item 名字` 或 `--item-id ID`：改城市的生产，步骤和玩家一样：打开城市窗口，按「更改」，从游戏给的列表里选，再关上窗口（窗口本来就开着时不关）。
  - 不给 `--item` 和 `--item-id` 时只看列表：按取消，不改生产，返回 `options`。每项有 `id`、`building`（与城市的 `building` 同一编号）、`name`、`text`（列表里显示的整行，含造价和回合数）。
  - 成功时返回 `building`、`building_name`、`shields`、`changed`。要造的就是现在在造的，`changed` 为 false。
  - 列表里没有这一项时按取消，返回 `illegal` 和 `options`。
  - 能不能造、换生产要不要扣盾，都由游戏决定。在单位、建筑、奇迹之间换类别且已有盾时，游戏弹出「文明规则: 改变生产品」（损失 50% 的盾），这时返回 `modal_open` 和 `dialog`，城市窗口留着不关。对话框里有两行可选：「不改变……的制造」和「损失50％的原料来生产……」，用 `respond --text` 选一行（只按「确定」等于选高亮的那行），然后 `close-city`。
- `close-city`：关上城市窗口，相当于按窗口里的「退出」。
- `rates --tax T --science S`：设税收和科学比例，各 0 到 10（即 0% 到 100%），奢侈是剩下的部分。和玩家一样从「王国」菜单打开税率窗口，把三个滑块拖到位，按确定。
  - 成功时返回 `tax_rate`、`science_rate`、`lux_rate`、`max_rate`、`changed`。
  - 任一项超过政体允许的上限（`max_rate`：无政府和专制 6，君主 7，共产、原教旨和共和 8，民主 10）时直接按确定，返回 `illegal` 和 `max_rate`，比例不变。
  - 锁定的滑块可能让某组比例设不出来，这时拖回原来的比例再确定，返回 `illegal`。
  - 窗口没有取消：确定、回车和 Esc 都保留窗口里的比例。游戏开局的前几个回合，确定会写一次 CIV2.DAT。
  - 游戏自己打开税率窗口时（换政体后比例超限），`tax_window` 不为 null，其他操作返回 `modal_open`；用 `rates` 设好并确定，游戏接着往下走。
- `end-turn`，可加 `--timeout-ms MS`（默认 120000）：结束回合，等到再次轮到玩家。中途出现对话框时返回 `modal_open`，带 `turn` 和 `turn_ended`。`turn_ended` 为 false 时回合还没结束，回答对话框后再发一次 `end-turn`；为 true 时已经是新回合，回答后直接操作，不要再发 `end-turn`，否则会把新回合也结束掉。

研究项没有单独的命令。玩家只能在游戏弹出的研究对话框里选，agent 也一样用 `respond` 回答。

会话：

- `wait-ready`，可加 `--in-game`、`--wait-s S`（默认 60）：等游戏启动、civ2agent 就绪；加 `--in-game` 时还要等到进入对局。
- `skip-intro`，可加 `--timeout-ms MS`（默认 60000）：跳过开场动画，直到主菜单出现。已经在主菜单或对局里时直接返回，`skipped` 为 false。

有对话框时发出的其他操作立即返回 `modal_open` 和 `dialog`，不会执行，也不会替你回答。先用 `respond` 回答。

游戏有时自己打开城市窗口，并停下来等它关上（建城、攻下城市、回合结束时的报告）。这时 `city_window.modal` 为 true，除 `produce`（只能是这座城市）和 `close-city` 外的操作都返回 `modal_open`，带 `city_window`。和玩家一样，改完生产或看完后 `close-city`，游戏接着往下走。税率窗口同理，见 `rates`。

## 局面

`state` 只给玩家在画面上能看到的东西。不在对局中但有对话框时（主菜单、开局设置），返回 `in_game: false` 和 `dialog`，与 `dialog` 命令相同。

- `turn`、`year`：回合和年份，公元前为负数。
- `human`：玩家的文明编号；`owner`、`civ` 字段用的就是这个编号。`difficulty`：难度 0（酋长）到 5（神）。
- `active_unit`：当前选中的单位 index，-1 表示没有。
- `civ`：玩家自己的文明。`gold`；`tax_rate`、`science_rate`、`lux_rate`（0 到 10）；`government`（0 无政府、1 专制、2 君主、3 共产、4 原教旨、5 共和、6 民主）；`research`、`research_name`（当前研究，-1 为没有）；`beakers`、`research_cost`（研究进度和这项的总需要量）；`future_techs`；`techs`（已有科技编号）。
- `move_unit`：一格移动力合多少点（道路倍数），`moves_left` 除以它就是单位面板上显示的剩余移动。
- `units`：己方单位。`index`（命令里用的编号）、`id`、`x`、`y`、`type`、`name`、`role`（5 为拓荒者类）、`moves_left`（本回合剩余移动力，单位为点）、`orders`（-1 无命令，1 设防中，2 已设防，3 睡眠，6 灌溉等）、`hp_lost`、`veteran`、`att`、`def`、`home`（所属城市 index，255 为无）。
- `foreign_units`：地图上画出来的外国单位，和游戏的判断一样：玩家探索过这一格，且这一格的单位自上次移动后被玩家发现过；城市里的单位不画。一格只给画出来的那一个，只有它的图上看得出的东西：`civ`、`x`、`y`、`type`、`name`、`orders`（盾上的命令字母，1 到 10；前往和无命令为 -1）、`automated`、`stacked`（这一格不止一个单位，图上多一面盾）、`health`（血条，0 到 100，以 10 为单位；只在游戏选项打开受伤显示时才有）。
- `cities`：己方城市。`index`、`id`、`name`、`x`、`y`、`size`、`building`（≥0 为单位类型，<0 为负的建筑编号）、`building_name`、`food`、`shields`、`improvements`（建筑位图）。
- `foreign_cities`：地图上画出来的外国城市（玩家发现过、探索过那一格）：`owner`、`name`、`x`、`y`、`size`（玩家上次看到的人口）。
- `tiles`：玩家探索过的地块，按玩家上次看到的样子。`x`、`y`；`t` 地形字节（低 4 位为地形，0x40 无特产，0x80 河流）；`f` 地形改造（0x02 城市，0x04 灌溉，0x08 矿山，0x0C 农田，0x10 道路，0x30 铁路，0x40 要塞，0x80 污染）。坐标满足 x + y 为偶数。
- `diplomacy`：已接触的文明，与外交部长界面一致：`civ`、`status`（`war`、`ceasefire`、`peace`、`alliance`、`none`）、`embassy`。
- `city_window`：打开着的城市窗口，没有则为 `null`。`city`（城市 index）、`name`、`modal`（游戏是否在等它关上）。`status`、`dialog` 和 `modal_open` 响应里也有。
- `tax_window`：打开着的税率窗口，没有则为 `null`。`max_rate`、`tax_rate`、`science_rate`、`lux_rate`（窗口里现在的值）。游戏一直等它关上。
- `notice`：画面上不等回答的建议框（如内政部的建议），没有则为 `null`。`title`、`text`。它不挡住游戏，不用 `respond`。`dialog` 和 `modal_open` 只指游戏在等回答的对话框。
- `dialog`：当前对话框，没有则为 `null`。

坐标、方向和单位命令字节与 MGE 5.4.0f 一致。走一格使用寻路增量（东一格 x 加 2）。灌溉、修路、开矿要等回合结算后，`f` 才会变。

## 对话框

`dialog` 命令、`state` 的 `dialog`、`modal_open` 响应和 `dialog` 事件用同一种对象：

- `title`、`text`：标题和正文。
- `kind`：`list` 表示有可选的行（列表行或选项行，如主菜单、开局设置、选研究），`controls` 表示只有按钮和输入框（城市命名、提示框）。
- `default`：只有 `list` 有，打开时高亮的行。
- `via`：`game` 表示 `respond` 调用对话框自己的处理函数，`mouse` 表示要用真实鼠标点（复选框、图标行、双列表）。
- `options`：可选项。`index` 从 0 起，`text` 是显示的文字，列表行另有游戏内部的 `id`，灰色不可选的行带 `disabled: true`。顺序是列表行或选项行，然后是按钮。
- `edits`：有输入框时才有，每个输入框当前的文字。

`via` 为 `game` 的对话框，`respond` 在游戏线程里调用对话框处理这次点击的函数，效果和玩家点击一样，不动鼠标，游戏窗口也不用在前台。灰色的行返回 `illegal`。

`via` 为 `mouse` 的对话框，控件不接受投递的鼠标消息。DLL 返回屏幕坐标，由 `civ2ctl.py` 把游戏窗口切到前台，用真实鼠标点击，期间不要动鼠标。要点的位置被别的窗口挡住时返回 `window_covered`，不点击。

`respond` 回答后最多等 2 秒，响应里另有 `via`（`game`、`keys` 或 `mouse`）、`changed`（对话框是否已关闭或换成了别的，false 表示没生效）和 `dialog`（之后的对话框，没有则为 `null`）。

## 事件

`events` 只报告玩家会知道的事：

- `dialog`（带 `dialog` 对象）、`dialog_closed`：对话框出现和关闭。游戏告诉玩家的事（发现科技、外交、灾害、远方的消息）都以对话框出现。
- `notice`（带 `notice` 对象）：出现了不等回答的建议框。
- `unit_lost`：己方单位没了（战死、解散、建城或并入城市）。`index`、`id`。
- `unit_damaged`：己方单位受伤。`index`、`id`、`hp_lost`。
- `city_captured`：玩家攻下或得到一座城。`index`、`id`、`name`、`x`、`y`、`previous_owner`。
- `city_lost`：玩家失去一座城。`name`、`x`、`y`、`owner`。
- `research`：当前研究项变了（研究完成后选了新的）。`tech`、`name`。

## 错误码

- `usage`：参数错误或命令名已改，看 `reason`
- `not_connected`：连不上游戏，或连接中断
- `timeout`：游戏线程在时限内没有完成
- `protocol_mismatch`：游戏里的 DLL 和命令行协议版本不同，重新编译并注入
- `version_mismatch`：`civ2.exe` 哈希不在允许列表
- `unsupported_limits`：`Civ2UIALauncher.ini` 里 `bUnitsLimit` 不是 0
- `unsupported`：不是单人游戏，或这个命令在当前版本还不能用
- `not_in_game`：还在主菜单，或地图尚未建立
- `not_your_turn`：现在不是玩家行动的时候
- `illegal`：界面上不能这样做（单位、坐标、选项、比例不合法，菜单项是灰的）
- `modal_open`：有对话框，带 `dialog`。先 `respond`
- `no_dialog`：没有可回答的对话框
- `window_covered`：要点的位置被别的窗口挡住，没有点击
- `game_fault`：游戏内部函数调用异常，游戏进程仍在
- `cli_fault`：命令行自身出错

哈希不匹配或单位上限被打开时，游戏照常可玩，只是命令会失败。

## 启动

在游戏目录旁启动：

```text
civ2agent\build\bin\civ2agent-launch.exe
python civ2agent\cli\civ2ctl.py wait-ready
python civ2agent\cli\civ2ctl.py skip-intro
```

已有 `civ2.exe` 时只注入，不另开一局。管道已在则直接退出。

管道只接受本机连接，但没有身份校验：本机任何进程都能连上来操作游戏。

## 调试

`--record 文件` 把每一对请求和响应追加成 jsonl。`replay 文件` 按顺序重发这种文件里的请求，每个请求输出一行；其中的 `respond` 同样可能点鼠标。这两个只用于排错，不在 `tools` 里。
