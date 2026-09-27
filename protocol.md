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
- `modal_open`：有对话框。响应里带 `dialog`。先 `act respond` 再继续
- `no_dialog`：没有可应答的对话框
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

默认只含人类玩家已知信息：回合、年份、金币、税率、研究、己方单位、己方城市、可见地形、条约、活动单位、当前对话框。`debug: true` 才返回全图和其他文明。

`events`

```json
{"op":"events"}
```

取走并清空自上次以来的事件：`dialog`、`dialog_closed`、`unit_lost`、`unit_damaged`、`city_captured`、`research`。

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
- `respond`：`button`，对话框按钮下标，从 0 起

`end_turn`

```json
{"op":"end_turn","timeout_ms":120000}
```

置位游戏自己的结束回合标志，阻塞到回合或年份变化。中途弹出确认框时返回 `modal_open`，应答后需要再发一次 `end_turn`。

## 命令行

在游戏目录旁启动：

```text
civ2agent\build\bin\civ2agent-launch.exe
```

已有 `civ2.exe` 时只注入，不另开一局。管道已在则直接退出。

```text
python civ2agent\cli\civ2ctl.py wait-ready
python civ2agent\cli\civ2ctl.py snapshot -o state.json
python civ2agent\cli\civ2ctl.py act order --unit 3 --order irrigate
python civ2agent\cli\civ2ctl.py act respond --button 0
python civ2agent\cli\civ2ctl.py end-turn
python civ2agent\cli\civ2ctl.py events
python civ2agent\cli\civ2ctl.py replay session.jsonl --record trace.jsonl
```

`--record` 把每一对请求和响应追加成 jsonl。`replay` 接受这种文件，也接受每行一个原始请求。

## Agent 工具

工具实现就是调用上面的 CLI，不要另读内存。

- `get_state`：`civ2ctl.py snapshot`。需要全图时加 `--debug`
- `act`：`civ2ctl.py act ...`。返回 `modal_open` 时先读 `dialog.buttons`，再 `act respond`
- `end_turn`：`civ2ctl.py end-turn`，然后 `events` 查看战斗和城池变化

默认遵守战争迷雾。`--debug` 会暴露未探索格和其他文明，只在排错时使用。

坐标、方向表和单位命令字节与 MGE 5.4.0f 的内存布局一致。走一格使用寻路增量（东一格 X 加 2）。`skip` 清掉该单位本回合剩余移动力，不改变睡眠或设防。灌溉、修路、开矿要等回合结算后，地形标志才会变。
