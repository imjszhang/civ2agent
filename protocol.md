# civ2agent 协议

日期：2026-09-27

进程内 DLL `civ2agent.dll` 在游戏 UI 线程执行命令。外部只通过命名管道 `\\.\pipe\civ2agent` 收发 UTF-8 JSON，一行一条。Agent 不读进程内存，只调用 `cli/civ2ctl.py`。

管道一问一答。响应里的 `id` 与请求相同。`ok` 为 false 时看 `error`。

## 错误码

- `version_mismatch`：`civ2.exe` 哈希不在允许列表
- `unsupported_limits`：`Civ2UIALauncher.ini` 里 `bUnitsLimit` 不是 0
- `not_in_game`：还在主菜单，或地图尚未建立
- `unsupported`：不是单人游戏
- `not_your_turn`：当前行动文明不是人类玩家
- `illegal`：单位、坐标、税率或按钮不合法
- `modal_open`：有对话框。响应里带 `dialog`，见下文「对话框」。先 `act respond` 再继续
- `no_dialog`：没有可应答的对话框
- `window_covered`：要点的位置被别的窗口挡住，`civ2ctl.py` 没有点击
- `game_fault`：内部函数调用异常，游戏进程仍在
- `timeout`：游戏线程在时限内没有完成

哈希不匹配或单位上限被打开时，游戏照常可玩，只是命令会失败。

## 请求

`hello`

```json
{"op":"hello"}
```

返回 `protocol`、`exe_hash`、`uia_loaded`、`in_game`、`limits_ok`、`multi_type`。

`snapshot`

```json
{"op":"snapshot","debug":false}
```

默认只含人类玩家已知信息：回合、年份、金币、税率、研究、己方单位、己方城市、可见地形、条约、活动单位、当前对话框。`debug: true` 才返回全图和其他文明。不在对局中但有对话框（主菜单、开局设置）时，返回 `in_game: false` 和 `menu`，与 `menu` 请求相同。

`events`

```json
{"op":"events"}
```

取走并清空自上次以来的事件：`dialog`（带 `dialog` 对象）、`dialog_closed`、`unit_lost`、`unit_damaged`、`city_captured`、`research`。

`act`

```json
{"op":"act","name":"move","unit":0,"x":4,"y":2}
```

`name` 取值：

- `activate`：`unit`
- `move`：`unit`、`x`、`y`，目标必须是相邻一格
- `goto`：`unit`、`x`、`y`
- `order`：`unit`、`order` 为 `fortify`、`sleep`、`wait`、`skip`、`irrigate`、`road`、`mine`、`clean`
- `found_city`：`unit`，角色必须是拓荒者或工程师。若弹出命名框，本调用返回 `modal_open`
- `produce`：`city`、`kind` 为 `unit` 或 `improvement`、`item` 为编号。建筑编号从 1 起，写入游戏时取负号
- `research`：`tech`，`-1` 表示清空
- `rates`：`tax`、`science`，各自 0 到 10，合计不超过 10。奢侈为剩余部分
- `respond`：`button` 为 `dialog.options` 的下标，或 `text` 为选项文字。见下文「对话框」

`end_turn`

```json
{"op":"end_turn","timeout_ms":120000}
```

结束回合走游戏自己的界面路径，调用返回时画面上的回合和年份已经更新。不能把 `0x628044` 清零，那个字节为 0 时游戏会退出整局回到主菜单。中途弹出对话框时返回 `modal_open`，带 `turn` 和 `turn_ended`。`turn_ended` 为 false 时回合还没结束，应答后再发一次 `end_turn` 继续等。为 true 时已经进入新回合，应答后直接操作，不要再发 `end_turn`，否则会把新回合也结束掉。

`skip_intro`

```json
{"op":"skip_intro","timeout_ms":60000}
```

启动后跳过开场动画，直到主菜单出现。已经在主菜单或对局里时直接返回，`skipped` 为 false。开场影片卡住界面线程时，命令会向游戏窗口发 Escape 和点击。

## 对话框

`menu`

```json
{"op":"menu"}
```

返回 `in_game` 和 `menu`，`menu` 是当前对话框。主菜单、开局设置和对局里都能用。没有对话框时返回 `no_dialog`。

`menu`、`modal_open` 响应、`snapshot` 的 `dialog` 和 `dialog` 事件用同一种对象：

- `title`、`text`：标题和正文
- `kind`：`list` 表示有可选的行（列表行或选项行，如主菜单、开局设置、选研究），`controls` 表示只有按钮和输入框（城市命名、提示框）
- `default`：只有 `list` 有，打开时高亮的行。高亮移动后它不变
- `via`：`game` 表示 `act respond` 调用对话框自己的处理函数，`mouse` 表示要用真实鼠标点（复选框、图标行、双列表）
- `options`：可选项，`index` 从 0 起，`text` 是显示的文字，列表行另有游戏内部的 `id`，灰色不可选的行带 `disabled: true`。顺序是列表行或选项行，然后是按钮
- `edits`：有输入框时才有，每个输入框当前的文字（如城市名、领袖名）

选一项：

```json
{"op":"act","name":"respond","text":"确定"}
```

`button` 填 `options` 的下标，`text` 填选项文字，两者都给时按 `text`。`act menu` 与 `act respond` 相同。

`via` 为 `game` 的对话框，DLL 在游戏线程里调用对话框自己处理这次点击的函数，效果和玩家点击相同，不移动鼠标，游戏窗口也不用在前台。灰色的行返回 `illegal`。

`via` 为 `mouse` 的对话框，控件不接受投递的鼠标消息。DLL 只返回屏幕坐标 `click`，由 `civ2ctl.py` 把游戏窗口切到前台，用真实鼠标点击。列表行双击，选中并确认。所以应答必须经过 `civ2ctl.py`，期间不要动鼠标。要点的位置被别的窗口挡住时返回 `window_covered`，不点击。列表行的控件还没画出来时，只能确认 `default` 那一行，其他行返回 `illegal`，稍后再发一次即可。

`civ2ctl.py` 应答后最多等 2 秒，另外返回：

- `via`：`game`、`keys` 或 `mouse`
- `changed`：对话框是否已关闭或换成了别的。`false` 表示没生效，可以重试
- `dialog`：之后的对话框，没有则为 `null`

有对话框时发出的其他 `act` 和 `end_turn` 立即返回 `modal_open`，不会执行，也不会替你应答。

## 命令行

在游戏目录旁启动：

```text
civ2agent\build\bin\civ2agent-launch.exe
```

已有 `civ2.exe` 时只注入，不另开一局。管道已在则直接退出。

```text
python civ2agent\cli\civ2ctl.py wait-ready
python civ2agent\cli\civ2ctl.py skip-intro
python civ2agent\cli\civ2ctl.py snapshot -o state.json
python civ2agent\cli\civ2ctl.py act order --unit 3 --order irrigate
python civ2agent\cli\civ2ctl.py menu
python civ2agent\cli\civ2ctl.py act respond --text 开始新游戏
python civ2agent\cli\civ2ctl.py act respond --button 0
python civ2agent\cli\civ2ctl.py end-turn
python civ2agent\cli\civ2ctl.py events
python civ2agent\cli\civ2ctl.py --record trace.jsonl replay session.jsonl
```

`--record` 和 `--timeout` 要写在子命令前面。`--record` 把每一对请求和响应追加成 jsonl。`replay` 接受这种文件，也接受每行一个原始请求，其中的 `respond` 同样会点鼠标。

## Agent 工具

工具实现就是调用上面的 CLI，不要另读内存。

- `get_state`：`civ2ctl.py snapshot`。需要全图时加 `--debug`
- `skip_intro`：`civ2ctl.py skip-intro`。启动后先跳过开场动画，再开局或读档
- `act`：`civ2ctl.py act ...`。返回 `modal_open` 时先读 `dialog.options`，再 `act respond --text ...`。只有 `via` 为 `mouse` 的对话框会移动真实鼠标
- `menu`：`civ2ctl.py menu`。主菜单和开局设置都是对话框，用它看选项，再 `act respond`
- `end_turn`：`civ2ctl.py end-turn`，然后 `events` 查看战斗和城池变化

默认遵守战争迷雾。`--debug` 会暴露未探索格和其他文明，只在排错时使用。

坐标、方向表和单位命令字节与 MGE 5.4.0f 的内存布局一致。走一格使用寻路增量（东一格 X 加 2）。`skip` 清掉该单位本回合剩余移动力，不改变睡眠或设防。灌溉、修路、开矿要等回合结算后，地形标志才会变。
