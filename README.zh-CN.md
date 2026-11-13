# civ2agent

[English](README.md)

更新日期：2026-11-13。

civ2agent 让程序像人一样玩《席德·梅尔的文明 II》多人黄金版：只走游戏自己的菜单、按键和对话框。一个小 DLL 注入 `civ2.exe`，在游戏的界面线程里执行命令，经本机命名管道回答。命令行不读进程内存，也不改游戏数据，只请游戏去做一次点击或按键会做的事。

每次调用在 stdout 打一行 JSON，一定有 `"ok"`。命令的完整约定在 [protocol.md](protocol.md)。本文件说明怎么编译、怎么指向一份安装，以及怎么开一局。

## 规则

1. **操作**只做玩家在界面上能做的事。DLL 调用游戏自己的处理函数。能不能做、做成什么样，由游戏决定。
2. **信息**只给玩家在地图、城市窗口和对话框里看得到的。内存里有、画面上没有的，不报。
3. **画面**在命令之后，和玩家亲手做完同一件事之后一样。

## 环境

- Windows。管道、启动器和真实鼠标都只在 Windows 上。
- 《文明 II》多人黄金版 **5.4.0f**，本仓库按中文界面编写。agent 要回答的对话框和菜单是中文（「确定」「王国」等）。`civ2.exe` 必须是 DLL 允许的那几份；其他哈希返回 `version_mismatch`，命令失败，游戏本身仍可玩。
- 游戏目录里要有 [Civ2UIA](https://github.com/FoxAhead/Civ2-UI-Additions)：`Civ2UIALauncher.exe` 和 `Civ2UIA.dll`。`Civ2UIALauncher.ini` 里的 `bUnitsLimit` 保持 0。打开后命令返回 `unsupported_limits`。
- `PATH` 上的 Python 3（或用 `CIV2AGENT_PYTHON` 指定）。
- 只要跑 runner 或观察页，才需要 Node.js 20 或更新。
- 编译需要 CMake 3.15 或更新，以及 32 位的 MSVC。游戏是 32 位的，DLL 和启动器必须用 `cmake -A Win32` 生成。

内存布局依据 [FoxAhead/Civ2-UI-Additions](https://github.com/FoxAhead/Civ2-UI-Additions)（MIT）。那些 Pascal 源码没有抄进本仓库。

## 目录

| 路径 | 作用 |
| --- | --- |
| `cli/civ2ctl.py` | 一次调用做一件玩家的事，或看一次局面。 |
| `cli/civ2session.py` | 一局一个目录：开局、存盘点、续玩、收尾。 |
| `dll/` | 注入游戏的 `civ2agent.dll`。 |
| `launch/main.cpp` | `civ2agent-launch.exe`。经 Civ2UIA 启动游戏，或注入已经在跑的游戏。 |
| `protocol.md` | 命令说明：参数、局面、对话框、错误码。 |
| `runner/` | 在一局的目录里启动 agent 命令行，一段一段往后玩。 |
| `runner/guides/` | 可选攻略和 `registry.json`。 |
| `dashboard/` | 本机观察页，以及 `/mcp` 上的 MCP 网关。 |
| `games/` | 各局的目录。不进 git。可用 `CIV2AGENT_GAMES_DIR` 换地方。 |
| `game.example.json` | 安装路径的样例。复制成 `game.json`（不进 git）。 |
| `.env.example` | 本地环境变量的样例。复制成 `.env`（不进 git）。 |

## 指定游戏位置

本项目不必放在游戏目录里面。安装目录就是含有 `civ2.exe` 的那个目录，按这个顺序找：

1. 已经设在进程里的环境变量 `CIV2AGENT_GAME`。
2. 项目根目录的 `.env`。只填上还没设置的变量，所以盖不过第 1 条。从 `.env.example` 复制。相对路径相对项目根目录。
3. 项目根目录的 `game.json`。从 `game.example.json` 复制，把 `game` 写成那个目录。相对路径相对项目根目录。
4. 本项目的上一级目录，前提是那里已经有 `civ2.exe`。仓库还放在安装目录里时，不用设置。

都找不到带 `civ2.exe` 的目录，启动会停下来并说明原因。

```json
{ "game": "D:\\Games\\Civilization II" }
```

`civ2session.py` 把决定好的目录传给启动器的 `--game`。单独运行启动器、又不写 `--game` 时，先看 `CIV2AGENT_GAME`，再从可执行文件向上找 `.env` 里的同一项，然后同样向上找 `game.json`，再向上找 `civ2.exe`。启动器里读这两个文件的逻辑在 `launch/main.cpp`。比这更早编译的启动器仍然接受 `--game`，`civ2session.py` 传的就是这个参数。读 `.env` 的那一版启动器要重新编译后才认这个文件。

## 编译

在项目根目录，用 32 位生成器：

```text
cmake -S . -B build -A Win32
cmake --build build --config Release
```

产物都在 `build/bin/`：

- `civ2agent.dll`
- `civ2agent-launch.exe`

`build/` 不进 git。协议变了就要重新编译再注入。DLL 和命令行的协议版本不一致时返回 `protocol_mismatch`。当前协议是 3。

## 启动游戏

```text
build\bin\civ2agent-launch.exe --game <含有 civ2.exe 的目录>
python cli\civ2ctl.py wait-ready
python cli\civ2ctl.py skip-intro
```

`civ2.exe` 已经在跑时，启动器只注入。管道 `\\.\pipe\civ2agent` 已经在时，它直接退出，不开第二局。管道接受本机任何进程，没有身份校验。

## 自己操作

`civ2ctl.py` 就是全部工具。`tools` 打出每个命令的 JSON Schema。`call` 用一个 JSON 对象跑一条命令。退出码：0 成功，1 游戏拒绝，2 参数错误（`usage`），3 连不上游戏或超时（`not_connected`、`timeout`）。

```text
python cli\civ2ctl.py tools
python cli\civ2ctl.py status
python cli\civ2ctl.py state --only civ,cities,units
python cli\civ2ctl.py dialog
python cli\civ2ctl.py respond --text 确定
python cli\civ2ctl.py move --unit 0 --x 12 --y 8
python cli\civ2ctl.py end-turn
```

有对话框时，其他操作立刻返回 `modal_open`，并不执行。先用 `respond` 回答。少数对话框（`via` 为 `mouse`）用真实鼠标点：游戏窗口留在前面，不要动鼠标。奇观静帧、皇宫静帧和最高议会也报成对话框，就地回答。

带上 `tiles` 时 `state` 很大。只有要看地图本身时才取。

一局由 `civ2session.py` 管着时，`load-game` 和 `quit-game` 返回 `session_policy`。`save-game` 只能写进这一局的 `saves/`。开局、读档和退出归会话管。

## 一局

一局是 `games/` 下的一个目录。里面有用户的要求、开局设置、笔记、每次调用的记录、每回合一行、快照和存档。收尾之后可以从这个目录接着玩。

```text
python cli\civ2session.py guides
python cli\civ2session.py new --spec spec.json --title "罗马" --guide jin-yuan
python cli\civ2session.py new --request "罗马人，国王，大地图"
python cli\civ2session.py setup games\<id>
python cli\civ2session.py checkpoint
python cli\civ2session.py resume games\<id> --guide jin-yuan
python cli\civ2session.py close --status paused
python cli\civ2session.py list
```

`new --spec` 会备份用户的 `.SAV`、`CIV2.DAT` 和 `HALLFAME.DAT`，启动游戏，并回答开局对话框。第一个真正要选择的对话框（通常是研究项）留给玩家。`new --request` 只建目录；下一步是写 `spec.json`，再运行 `setup`。

`checkpoint` 在回合开始、没有对话框、也没有城市窗口时运行一次。它经游戏菜单存成 `saves/T0042.SAV`，在 `turns.jsonl` 加一行，每第 10 回合在 `snapshots/` 存一份局面。同一回合再存一次什么也不做。第一次还会先存 `saves/start.SAV` 再读回来，让之后的自动存档写进 `saves/`，而不是安装目录。`close` 会把 `CIV2.DAT` 和 `HALLFAME.DAT` 放回去，agent 的这一局不改用户的设置，也不进名人堂。

同时只有一局在进行（`games/.active`）。锁还在别的局上时，`new`、`setup`、`resume` 会拒绝。

`spec.json` 里没写的键用游戏的默认值。用户没指定世界大小时写 `"map_size": "large"`（75×120）。

| 键 | 取值 |
| --- | --- |
| `start` | `new`（默认）或 `custom` |
| `map_size` | `small`、`medium`、`large`，或 `{"width": 60, "height": 80}` |
| `difficulty` | `chieftain`、`warlord`、`prince`、`king`、`emperor`、`deity` |
| `civs` | 3 到 7，或 `random` |
| `barbarians` | `villages`、`roaming`、`restless`、`raging` 或 `random` |
| `rules` | `standard` |
| `gender` | `male`、`female` |
| `tribe`、`leader` | 与对话框里的文字一致。`tribe` 可以是其中一段（`罗马` 对上 `罗马人`）。 |
| `world` | `custom` 时用：`land`、`form`、`climate`、`temperature`、`age`，每项是一个名字或 `random` |
| `title`、`id` | 显示名，以及目录名里的英文标识 |

一局目录里：

| 路径 | 内容 |
| --- | --- |
| `session.json` | 状态、spec、文明和领袖、回合、存档列表。 |
| `brief.md`、`spec.json` | 用户的要求和开局设置。 |
| `agent/` | agent 的工作目录。`notes.md` 和 `strategy.md` 由 agent 写。`playbook.md`、`context.md`、`rule.md`、`guide.md` 每一段由 runner 抄入。 |
| `agent/strategy/T####.md` | 上一回合的策略，由 `checkpoint` 归档。这一回合已经有文件就不覆盖。 |
| `saves/` | 只放 `.SAV`。游戏会把存档所在目录当成场景目录（里面有 `GAME.TXT` 就用它），所以这里不能放别的。 |
| `turns.jsonl`、`log.jsonl`、`runs.jsonl` | 每回合一行、每次命令、每一段 agent。 |
| `backup/` | 开局前用户的文件，带 SHA-256。 |

## 让 agent 玩

`runner/civ2-play.mjs` 在这一局的 `agent/` 里启动一个 agent 命令行，一次玩若干回合，然后存盘并记下这一段。agent 用的就是人用的那两个命令行。可选 Codex、Claude Code、DeepSeek Harness、OpenCode、Cursor Agent，经 [js-agent-runtime](https://github.com/imjszhang/js-agent-runtime) 启动。

```text
cd runner
npm ci
copy targets.example.json targets.json
node civ2-play.mjs targets
node civ2-play.mjs probe --target claude
node civ2-play.mjs new --request "罗马人，国王，大地图" --guide jin-yuan --turns 20 --target claude
node civ2-play.mjs resume ..\games\<id> --guide jin-yuan --turns 30
```

`targets.json` 不进 git。不写 `--target` 时用里面的 `default`。每一段都可以换一个。`harness.executable` 必须是真正的可执行文件，不能是 npm 的 `.cmd`。Windows 上的 Claude Code 没有沙箱，`permission` 写 `bypass`。Cursor Agent 的 `executable` 填版本目录里的 `node.exe`，不要填 `agent.cmd`，并在 cursor-login 的 backend 上加 `useCurrentUserHomeForLogin`。

一段默认玩 `--segment-turns` 个回合（默认 5），最长 `--segment-minutes` 分钟（默认 60）。玩满 `--turns`（默认 10）、某一段失败，或回合没有往前走，就停下。Ctrl+C 会收尾。`--keep-open` 留下游戏和锁。

只有 `--request`、没有 spec 时，第一段先写 `spec.json` 并 `setup`，再接着玩。每一回合 agent 重写 `strategy.md`，再按这一局的 `rule.md` 核对后才下令。新局默认用法则 `basic`，`--rule` 可以换一套，`--rule none` 不用。人在观察页里改这一局的法则，上一份留在该局的 `agent/rules/`。agent 不改它。

`--python` 或 `CIV2AGENT_PYTHON` 指定跑命令行的 Python。

游戏窗口留在屏幕上。回答 `mouse` 对话框时不要动鼠标。

## 可选攻略

一份攻略是 `runner/guides/registry.json` 旁边的一个 Markdown 文件。在注册表里登记名字，不要写死在代码里。

```json
{
  "jin-yuan": { "title": "金元帝国", "file": "jin-yuan.md" },
  "jin-yuan-rush": { "title": "金元帝国·神级快攻", "file": "jin-yuan-rush.md" }
}
```

仓库里只提交 `runner/rules/basic.md` 这一份最基本的法则。把 `registry.example.json` 复制成 `registry.json`，再把别的 md 放在旁边，这些都留在本机，不进 git。攻略也一样：`runner/guides/*.md` 和 `registry.json` 只在本机。示例注册表只说明格式。

## 观察页和 MCP

观察页看正在进行的这一局，并在 `/mcp` 提供 MCP，让另一台机器开局、续玩、存盘、收尾和代打。网关只转发。规则仍在 `civ2session.py` 和 `civ2ctl.py` 里。

```text
cd dashboard
npm ci
npm start
```

页面是 `http://127.0.0.1:4732`。MCP 是 `http://127.0.0.1:4732/mcp`。端口用 `DASHBOARD_PORT` 改。默认只听 `127.0.0.1`，本机客户端不用令牌。要对局域网开放，设置 `DASHBOARD_HOST=0.0.0.0` 和 `DASHBOARD_TOKEN`。没有令牌时，进程拒绝绑到回环以外的地址。启动后会打出每个局域网地址。Windows 防火墙要自己放行这个端口，程序不会写规则。

MCP 仍然要 `Authorization: Bearer <令牌>`。页面和 `/events` 用浏览器带不上这个头，所以打开 `/login`，提交同一个令牌。网址带 `?token=` 时同样登录，随后地址栏不再留这个参数。登录 cookie 是 `HttpOnly`，保存 180 天，这台浏览器下次打开直接进入。`/logout` 清掉它。页面的主操作是开局：写下怎么开、可选攻略、局数（1 到 20）和每局回合（1 到 500），然后「开局并玩」。它在跑游戏的电脑上按这个次数启动 `civ2-play.mjs`，一局结束再开下一局。正在进行的局用「接着玩这一局」。停下会结束这次安排并交回租约。回答对话框、结束回合、存盘、走一步、改产、设定税率收在「单步操作」里。`via` 为 `mouse` 的对话框仍会移动跑游戏那台电脑的鼠标，游戏窗口留在那块屏幕上。

同一时刻只有一局、一次会改局面的调用。`claim`（`client` 是显示在观察页上的名字）返回 `lease`。同一名字再领一次，交还它已经持有的那把；别人持有时返回 `busy`，并带上持有者名字和当前租约号。开局、续玩、存盘、收尾和代打都带上它。`release` 交回。没有命令在跑时，可以不带租约号，用持有者名字交回；观察页的「交回」直接清掉当前空闲持有者。空闲 10 分钟后租约放开（`GATE_LEASE_IDLE_MS`）。游戏没在跑、也没有命令在执行时，下一次别人的 `claim` 直接接过空闲租约。第二条改动立刻返回 `busy`，不排队。碰上正在读取局面时，会等这一次读完再执行。租约在手里时，`games/.lease` 记下观察页进程。`civ2ctl.py` 和 `civ2session.py` 在碰管道之前，除非环境变量 `CIV2AGENT_LEASE` 就是这张租约，否则返回 `busy`。本机 runner 因此把管道让出来。进程已经不在时，这把锁不作数。

`list_games`、`game_status`、`read_game` 不需要租约。`status`、`state`、`dialog` 也不需要；锁被占用时它们返回 `busy` 和上一份快照，不再发命令。`respond`、`move` 这类代打需要租约。网关上没有 `save-game`、`load-game`、`quit-game`，用 `checkpoint`、`resume_game`、`close_game`。

## 测试

单测不需要游戏：

```text
python -m unittest discover -s tests -p "test_*.py"
cd runner && npm test
cd dashboard && npm test
```

`tests/smoke_hello.py` 和 `tests/session_check.py` 会启动真正的游戏。只在打算这么做时运行，并且不要有另一局还开着。`session_check.py` 自己会退出游戏，并核对安装目录里的存档和 `.DAT` 与开局前一致。

## 环境变量

项目根目录的 `.env` 会填上还没设置的变量。`CIV2AGENT_LEASE` 不从 `.env` 读，它只由观察页在一次调用上设置。

| 变量 | 含义 |
| --- | --- |
| `CIV2AGENT_GAME` | 含有 `civ2.exe` 的目录。进程里的值优先，然后是 `.env`，然后是 `game.json`。 |
| `CIV2AGENT_GAMES_DIR` | 各局目录放在哪里。默认 `games/`。 |
| `CIV2AGENT_PYTHON` | runner 和观察页用来跑命令行的 Python。 |
| `CIV2AGENT_LEASE` | 观察页给自己拉起的命令带上，这样 `games/.lease` 还在时它们仍能用管道。 |
| `DASHBOARD_HOST` | 监听地址。默认 `127.0.0.1`。 |
| `DASHBOARD_PORT` | 默认 `4732`。 |
| `DASHBOARD_TOKEN` | 监听地址不是回环时必须设置。 |
| `GATE_LEASE_IDLE_MS` | 空闲的 MCP 租约保留多久。默认 600000。 |
