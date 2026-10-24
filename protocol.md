# civ2agent 命令行接口

日期：2026-10-24，协议 3

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
- `state`：当前局面，见「局面」。`--only 部分,部分` 只返回这几部分（`civ`、`units`、`foreign_units`、`cities`、`foreign_cities`、`tiles`、`diplomacy`、`city_window`、`tax_window`、`notice`、`dialog`），`turn`、`year` 等总是返回；`tiles` 很大，不看地形时不要取。`--out 文件` 另存一份。
- `dialog`：`in_game`、`city_window`、`tax_window` 和当前对话框 `dialog`，见「对话框」；没有对话框时为 `null`。主菜单和开局设置也是对话框。
- `events`：取走并清空上次以来的事件，见「事件」。

操作，每个对应玩家的一次操作：

- `respond --option N` 或 `respond --text 文字`，可加 `--edit 文字`：回答当前对话框，相当于玩家点选一项。`--edit` 先在输入框里填字（城市名、领袖名），再选这一项；有几个输入框就按顺序给几次（自定地图尺寸的横、纵）。输入框有长度上限（城市名 15 字节、领袖名 23 字节，一个汉字算 2 字节），任何一个超长或含输入框不收的字符时返回 `illegal`，哪个框都不填。
- `respond --file 路径`：回答 Windows 的文件对话框（`kind` 为 `file`），先在文件名框里填路径；只给 `--file` 时按保存或打开。见「Windows 对话框」。
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

- `save-game --file 路径`，可加 `--overwrite`：存档。点「游戏」菜单的「储存游戏」，在 Windows 文件对话框里填文件名（绝对路径，扩展名 .SAV），按保存，再关掉游戏的「存档完毕!」。成功时返回 `file`、`bytes`。文件已存在时不加 `--overwrite` 就返回 `illegal`；加了，Windows 问是否替换时回答「是」。
- `load-game --file 路径`：读档。对局中点「游戏」菜单的「载入游戏」，在主菜单选「读取进度」；填文件名，按打开，再关掉游戏的读档消息（标题「困难程度: ...」）。成功时返回 `turn`、`year`、`civ`、`leader`。
- `quit-game`，可加 `--exit`：点「游戏」菜单的「离开游戏」，在「确认」里选「是」，回到主菜单；`--exit` 再在主菜单按取消，游戏程序退出。
- 游戏局（见「游戏局」）进行中，`load-game` 和 `quit-game` 返回 `session_policy`，`save-game` 只能存到这局的 `saves/`（可以只给文件名）。读档、离开和收尾由 `civ2session.py` 负责。

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
- `civ`：玩家自己的文明。`name`（种族名，如 罗马人）、`leader`（领袖名）、`title`（领袖的头衔，随政体变）；`gold`；`tax_rate`、`science_rate`、`lux_rate`（0 到 10）；`government`（0 无政府、1 专制、2 君主、3 共产、4 原教旨、5 共和、6 民主）；`research`、`research_name`（当前研究，-1 为没有）；`beakers`、`research_cost`（研究进度和这项的总需要量）；`future_techs`；`techs`（已有科技编号）。
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

`respond` 回答后最多等 2 秒，响应里另有 `via`（`game`、`keys`、`mouse` 或 `windows`）、`changed`（对话框是否已关闭或换成了别的，false 表示没生效）和 `dialog`（之后的对话框，没有则为 `null`）。

### Windows 对话框

存档和读档时，游戏打开的是 Windows 自己的文件对话框；替换已有文件时 Windows 还会弹出一个问题框。它们的 `via` 为 `windows`：

- `kind`：`file`（有文件名框，`file` 是框里现在的文字）或 `message`（只有文字和按钮，`text` 是问题）。
- `options`：按钮，从左到右，`button` 是 Windows 的按钮编号，任何语言下都一样（1 确定、保存或打开，2 取消，6 是，7 否）。

这种对话框开着时，游戏线程在 Windows 的对话框里等，`status` 的 `dialog_open` 为 true，`dialog` 返回它，其他命令立即返回 `modal_open`。用 `respond` 回答：`--file` 填文件名（游戏最多收 255 字节，只能有游戏代码页里的字符），`--option` 或 `--text` 按按钮。命令行通过窗口消息填字和按按钮，效果和玩家打字、点击一样。

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
- `session_policy`：游戏局进行中，这件事由 `civ2session.py` 负责（读档、离开游戏、存到别处）
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

## 游戏局

`cli/civ2session.py` 管理 agent 自己玩的游戏：按用户的要求开一局，每局一个目录 `civ2agent/games/<id>/`（不进 git），以后可以从这个目录接着玩。输出和退出码与 `civ2ctl.py` 相同。游戏里的每一步都通过 `civ2ctl.py` 的玩家操作完成：开局设置在游戏的开局对话框里选，存档走游戏菜单，所以画面上都看得到。

```text
python civ2agent\cli\civ2session.py new --spec spec.json [--title 名字] [--request 用户的原话]
python civ2agent\cli\civ2session.py new --request 用户的原话      # 之后写 spec.json，再 setup
python civ2agent\cli\civ2session.py setup 目录
python civ2agent\cli\civ2session.py checkpoint                    # 每回合一次
python civ2agent\cli\civ2session.py resume 目录
python civ2agent\cli\civ2session.py close [--status paused|ended] [--keep-running]
python civ2agent\cli\civ2session.py status [目录]
python civ2agent\cli\civ2session.py list
```

- `new --spec`：建目录，备份用户的文件，启动游戏，按 spec 回答开局对话框，进入第一回合。游戏开局时的消息（「在一开始」、初学者指导）按确定，内容记进 `session.json` 的 `setup`；第一个要做选择的对话框（如研究项）留给 agent，返回 `dialog`。spec 没写到、又要做选择的开局对话框返回 `modal_open`：用 `civ2ctl.py respond` 回答，再运行 `setup 目录` 接着开局。
- `checkpoint`：回合开始、没有对话框也没有城市窗口时运行。经游戏菜单存成 `saves/T0042.SAV`（同一回合再运行什么也不存），在 `turns.jsonl` 记一行，第 1 回合和每第 10 回合在 `snapshots/` 存一份完整局面。只保留最近 10 回合和每第 10 回合的存档。
- `resume 目录`：启动游戏（已在运行且就是这局时直接接上），读这局最新的存档，核对文明、领袖是这一局。
- `close`：存盘点，离开并退出游戏，恢复用户的文件，解锁。`--status ended` 表示这局已经结束。
- `status`、`list`：一局的 `session.json`、最近几回合和游戏状态；所有局的概要。

目录里：

- `session.json`：状态（`setup`、`starting`、`active`、`paused`、`ended`、`failed`）、用户的要求、spec、开局时每个对话框选了什么、文明和领袖、现在的回合、存档列表、agent 运行的段数。
- `brief.md`：用户的要求和开局设置。`spec.json`：开局设置。
- `agent/`：agent 的工作目录。`notes.md` 是 agent 的长期笔记；`playbook.md`、`brief.md`、`context.md` 由 runner 每段写入。
- `log.jsonl`：这局里每次 `civ2ctl.py`、`civ2session.py` 调用的请求和响应（`state` 只记概要）。`turns.jsonl`：每回合一行。`runs.jsonl`、`runs/`：每段 agent 运行的结果和记录。
- `saves/`：只放 .SAV，包括游戏自己的自动存档。游戏读档时会把存档所在目录当作覆盖目录（那里有 GAME.TXT 就用它），所以这里不能放别的。
- `backup/`：开局前用户的文件和 `manifest.json`（SHA-256）。

开局设置 `spec.json`，没写的键用游戏的默认值：

```json
{"map_size": "small", "difficulty": "king", "civs": 5, "barbarians": "roaming", "rules": "standard",
 "gender": "male", "tribe": "罗马", "leader": "测试员", "id": "romans-king"}
```

- `start`：`new`（开始新游戏，默认）或 `custom`（自定世界，可加 `world`）。
- `map_size`：`small`、`medium`、`large`，或 `{"width": 60, "height": 80}`（自定尺寸）。
- `difficulty`：`chieftain`、`warlord`、`prince`、`king`、`emperor`、`deity`。
- `civs`：3 到 7（竞争程度，对话框里的「七个文明」等），或 `random`。
- `barbarians`：`villages`、`roaming`、`restless`、`raging` 或 `random`。`rules`：`standard`（`custom` 的选项框要用鼠标点）。`gender`：`male`、`female`。
- `tribe`：种族名，与列表里的字一致或是它的一部分（`罗马` 对上 `罗马人`）；对不上时返回全部可选项。`leader`：领袖名（最多 23 字节）。`city_style`：城市风格。
- `world`（`custom` 时）：`land`（`small`、`normal`、`large`）、`form`（`archipelago`、`varied`、`continents`）、`climate`（`arid`、`normal`、`wet`）、`temperature`（`cool`、`temperate`、`warm`）、`age`（`3`、`4`、`5`，单位十亿年），各项也可以是 `random`。
- `title`、`id`：这局的名字和目录名里的英文标识。

用户的文件：

- 自动存档写在最近一次读档的存档所在的目录，新开的局写在游戏目录，文件名是领袖名的前 2 字节加 `_自动.SAV`。所以第一次 `checkpoint` 先存成 `saves/start.SAV` 再读回来，之后游戏的自动存档都写进 `saves/`。
- 读回之前（第 1 回合）游戏可能已经在游戏目录写了自动存档，覆盖同名的文件。开局前游戏目录的 .SAV、CIV2.DAT、HALLFAME.DAT 都备份在 `backup/`；读回后对比：改动过的恢复原样（agent 的那份另存为 `saves/root-*.SAV`），新出现的同前缀自动存档移进 `saves/`，其他新文件只报告。
- `close` 退出游戏后恢复 CIV2.DAT 和 HALLFAME.DAT：agent 的局不改用户的游戏设置，也不进用户的名人堂。
- 同时只有一局在进行（`games/.active`）。别的局在进行时 `new`、`setup`、`resume` 拒绝；游戏里正在进行的不是这局时，`close` 既不存它也不退出它。
- agent 只通过 `civ2ctl.py` 看局面，不读 .SAV 的内容。

## 让 agent 玩

`runner/civ2-play.mjs` 用 [js-agent-runtime](https://github.com/imjszhang/js-agent-runtime) 启动一个 agent 命令行来玩游戏局，可以选 Codex、Claude Code、DeepSeek Harness、OpenCode 或 Cursor Agent。agent 和人一样只用 `civ2ctl.py`、`civ2session.py`。

```text
cd civ2agent\runner
npm ci
copy targets.example.json targets.json          # 填上本机的可执行文件路径和登录方式
node civ2-play.mjs targets
node civ2-play.mjs probe --target claude
node civ2-play.mjs new --spec spec.json --turns 20 --target claude
node civ2-play.mjs new --request "用罗马人，国王难度，小地图" --turns 20
node civ2-play.mjs resume ..\games\<id> --turns 30 --target codex
```

- `targets.json`（不进 git）：名字到 js-agent-runtime 的 target（`harness`、`backend`），`default` 是不给 `--target` 时用的。每段都可以换一个 agent。
- 每一段：`civ2session.py context` 写好 agent 目录（`playbook.md`、`brief.md`、`context.md`、`notes.md`），agent 在 `agent/` 里运行，目标是往后玩 `--segment-turns` 个回合（默认 5）；之后在 `runs.jsonl` 记一行（状态、回合、用量、agent 的总结），运行记录在 `runs/`。玩够 `--turns`（默认 10）、某段失败或 agent 没有往前玩就停下，然后 `close`。Ctrl+C 也会停下并 `close`；`--keep-open` 不收尾。
- `new --request`：第一段的 agent 按用户的原话写 `spec.json` 并 `setup`，然后接着玩。
- agent 要运行 Python 和 `civ2ctl.py`（命名管道），少数对话框要用真实鼠标：Windows 上 Claude Code 没有沙箱，用 `permission: bypass`；别的 harness 的沙箱能不能做到要先试。js-agent-runtime 要有 Windows 进程支持（0.2.0 的 `run()` 只支持 macOS 和 Linux）。
- `--python` 或 `CIV2AGENT_PYTHON` 指定运行命令行的 Python。
- 运行时游戏窗口要留在屏幕上，不要动鼠标（`via` 为 `mouse` 的对话框会用真实鼠标点）。

## 调试

`--record 文件` 把每一对请求和响应追加成 jsonl。`replay 文件` 按顺序重发这种文件里的请求，每个请求输出一行；其中的 `respond` 同样可能点鼠标。这两个只用于排错，不在 `tools` 里。
