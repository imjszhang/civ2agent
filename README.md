# civ2agent

[中文](README.zh-CN.md)

Updated 2026-11-13.

civ2agent lets a program play *Sid Meier's Civilization II* Multiplayer Gold Edition the way a person does: through the game's own menus, keys, and dialogs. A small DLL is injected into `civ2.exe`. It runs each command on the game's interface thread and answers a local named pipe. The command line never reads process memory and never writes game data except by asking the game to do what a click or a key would do.

Every call prints one JSON object on one line, always with `"ok"`. The full command contract is [protocol.md](protocol.md) (written in Chinese). This file is how to build, point the project at an install, and run a match.

## Rules

1. **Actions** are only what a player can do in the interface. The DLL calls the game's own handlers. Whether an action is legal, and what it does, is the game's decision.
2. **Information** is only what the player can see on the map, in a city window, or in a dialog. Hidden memory is not reported.
3. **The screen** after a command matches the screen after the same action done by hand.

## Requirements

- Windows. The pipe, the launcher, and the mouse path are Windows-only.
- *Civilization II* MGE **5.4.0f**, the Chinese interface this tree was written against. Dialog and menu text the agent answers is Chinese (`确定`, `王国`, and so on). `civ2.exe` must be one of the builds the DLL allow-lists; any other hash returns `version_mismatch` and commands fail. The game itself still runs.
- [Civ2UIA](https://github.com/FoxAhead/Civ2-UI-Additions) in the game directory: `Civ2UIALauncher.exe` and `Civ2UIA.dll`. In `Civ2UIALauncher.ini`, leave `bUnitsLimit` at 0. If it is on, commands return `unsupported_limits`.
- Python 3 on `PATH` (or `CIV2AGENT_PYTHON`).
- Node.js 20 or newer, only for the runner and the dashboard.
- To compile: CMake 3.15 or newer and a 32-bit MSVC toolset. The game is 32-bit, so the DLL and the launcher must be built with `cmake -A Win32`.

Memory layouts follow [FoxAhead/Civ2-UI-Additions](https://github.com/FoxAhead/Civ2-UI-Additions) (MIT). Those Pascal sources are not copied into this tree.

## Layout

| Path | Role |
| --- | --- |
| `cli/civ2ctl.py` | One player action or one observation per call. |
| `cli/civ2session.py` | One match in its own folder: start, checkpoint, resume, close. |
| `dll/` | `civ2agent.dll`, injected into the game. |
| `launch/main.cpp` | `civ2agent-launch.exe`. Starts the game through Civ2UIA, or injects into a game that is already running. |
| `protocol.md` | Command reference: arguments, the state object, dialogs, errors. |
| `runner/` | Plays a stretch of turns by starting an agent CLI in the match folder. |
| `runner/guides/` | Optional strategy texts and `registry.json`. |
| `dashboard/` | Local observation page and an MCP gateway on `/mcp`. |
| `games/` | Match folders. Not in git. Override with `CIV2AGENT_GAMES_DIR`. |
| `game.example.json` | Template for the install path. Copy to `game.json` (not in git). |
| `.env.example` | Template for local environment variables. Copy to `.env` (not in git). |

## Point it at the game

The project does not have to sit inside the game directory. The install is the directory that contains `civ2.exe`, chosen in this order:

1. `CIV2AGENT_GAME` already set in the process environment.
2. `.env` in the project root. It fills only variables that are not already set, so it does not win over step 1. Copy `.env.example`. A relative path is relative to the project root.
3. `game.json` in the project root. Copy `game.example.json` and set `game` to that directory. A relative path is relative to the project root.
4. The parent of this project, when that directory already contains `civ2.exe`. A checkout that still lives inside the install needs no setting.

If none of these yields a directory with `civ2.exe`, startup stops and says so.

```json
{ "game": "D:\\Games\\Civilization II" }
```

`civ2session.py` passes the chosen directory to the launcher as `--game`. Running the launcher by itself, with no `--game`, uses `CIV2AGENT_GAME`, then `CIV2AGENT_GAME` in a `.env` found by walking up from the executable, then a `game.json` found the same way, then the same upward search for `civ2.exe`. Both file walks in the launcher binary are in `launch/main.cpp`. A launcher built before a given file was added still accepts `--game`, which is what `civ2session.py` sends. The binary reads `.env` only after it is rebuilt.

## Build

From the project root, with a 32-bit generator:

```text
cmake -S . -B build -A Win32
cmake --build build --config Release
```

Outputs, both under `build/bin/`:

- `civ2agent.dll`
- `civ2agent-launch.exe`

`build/` is not in git. After a protocol change, rebuild and inject again. A DLL and a command line on different protocol versions return `protocol_mismatch`. The current protocol is 3.

## Start the game

```text
build\bin\civ2agent-launch.exe --game <directory with civ2.exe>
python cli\civ2ctl.py wait-ready
python cli\civ2ctl.py skip-intro
```

If `civ2.exe` is already running, the launcher only injects. If the pipe `\\.\pipe\civ2agent` is already up, it exits without starting a second game. The pipe accepts any local process. There is no authentication.

## Drive a game by hand

`civ2ctl.py` is the whole tool surface. `tools` prints a JSON Schema for every command. `call` runs one command from a JSON object. Each call's exit code is 0 on success, 1 when the game refuses, 2 on bad arguments (`usage`), and 3 when the game cannot be reached (`not_connected`, `timeout`).

```text
python cli\civ2ctl.py tools
python cli\civ2ctl.py status
python cli\civ2ctl.py state --only civ,cities,units
python cli\civ2ctl.py dialog
python cli\civ2ctl.py respond --text 确定
python cli\civ2ctl.py move --unit 0 --x 12 --y 8
python cli\civ2ctl.py end-turn
```

While a dialog is open, every other action returns `modal_open` and does not run. Answer it with `respond`. A few dialogs (`via` `mouse`) are clicked with the real cursor: leave the game window in front and do not touch the mouse. Wonder stills, palace stills, and the council are reported as dialogs and answered in place.

`state` is large when `tiles` is included. Ask for `tiles` only when the map itself matters.

During a match owned by `civ2session.py`, `load-game` and `quit-game` return `session_policy`. `save-game` may write only into that match's `saves/`. Starting, loading, and quitting belong to the session.

## One match

A match is a directory under `games/`. It holds the request, the start settings, notes, a log of every call, one line per turn, snapshots, and saves. Close it and resume it later from that directory.

```text
python cli\civ2session.py guides
python cli\civ2session.py new --spec spec.json --title "Romans" --guide jin-yuan
python cli\civ2session.py new --request "罗马人，国王，大地图"
python cli\civ2session.py setup games\<id>
python cli\civ2session.py checkpoint
python cli\civ2session.py resume games\<id> --guide jin-yuan
python cli\civ2session.py close --status paused
python cli\civ2session.py list
```

`new --spec` backs up the user's `.SAV` files, `CIV2.DAT`, and `HALLFAME.DAT`, starts the game, and answers the opening dialogs. The first dialog that is a real choice (usually research) is left for the player. `new --request` only creates the folder; the next step is to write `spec.json` and run `setup`.

`checkpoint` runs once at the start of a turn, with no dialog and no city window open. It saves `saves/T0042.SAV` through the game menu, appends `turns.jsonl`, and every tenth turn writes `snapshots/`. The same turn saved again is a no-op. The first checkpoint also saves `saves/start.SAV` and reloads it, so later autosaves land in `saves/` instead of the install directory. On `close`, `CIV2.DAT` and `HALLFAME.DAT` are put back, so the agent's match does not change the user's preferences or hall of fame.

Only one match is active (`games/.active`). `new`, `setup`, and `resume` refuse while another match holds the lock.

`spec.json` uses the game's defaults for any key you omit. When the user did not ask for a map size, write `"map_size": "large"` (75×120). Keys:

| Key | Values |
| --- | --- |
| `start` | `new` (default) or `custom` |
| `map_size` | `small`, `medium`, `large`, or `{"width": 60, "height": 80}` |
| `difficulty` | `chieftain`, `warlord`, `prince`, `king`, `emperor`, `deity` |
| `civs` | 3–7, or `random` |
| `barbarians` | `villages`, `roaming`, `restless`, `raging`, or `random` |
| `rules` | `standard` |
| `gender` | `male`, `female` |
| `tribe`, `leader` | As shown in the dialog. `tribe` may be a substring (`罗马` matches `罗马人`). |
| `world` | For `custom`: `land`, `form`, `climate`, `temperature`, `age`, each a named size or `random` |
| `title`, `id` | Display name and the ASCII slug in the folder name |

Inside a match folder:

| Path | Contents |
| --- | --- |
| `session.json` | Status, spec, identity, turn, save list. |
| `brief.md`, `spec.json` | The request and the settings. |
| `agent/` | Where the agent works. `notes.md` and `strategy.md` are the agent's. `playbook.md`, `context.md`, `rule.md`, and `guide.md` are copied in for each stretch. |
| `agent/strategy/T####.md` | The previous turn's strategy, archived by `checkpoint`. An existing file for that turn is kept. |
| `saves/` | Only `.SAV` files. The game treats a save's directory as a scenario folder when `GAME.TXT` is present, so nothing else goes here. |
| `turns.jsonl`, `log.jsonl`, `runs.jsonl` | One turn, every command, every agent stretch. |
| `backup/` | The user's files from before the match, with SHA-256 hashes. |

## Let an agent play

`runner/civ2-play.mjs` starts an agent CLI in the match's `agent/` directory for a few turns at a time, then checkpoints and records the run. The agent uses the same two command lines a person uses. Harnesses: Codex, Claude Code, DeepSeek Harness, OpenCode, Cursor Agent, through [js-agent-runtime](https://github.com/imjszhang/js-agent-runtime).

```text
cd runner
npm ci
copy targets.example.json targets.json
node civ2-play.mjs targets
node civ2-play.mjs probe --target claude
node civ2-play.mjs new --request "罗马人，国王，大地图" --guide jin-yuan --turns 20 --target claude
node civ2-play.mjs resume ..\games\<id> --guide jin-yuan --turns 30
```

`targets.json` is not in git. `default` is the target used when `--target` is omitted. Each stretch can name a different one. `harness.executable` must be a real executable, not an npm `.cmd` shim. On Windows, Claude Code has no sandbox: set `permission` to `bypass`. For Cursor Agent, point `executable` at the `node.exe` in the version directory, not at `agent.cmd`, and set `useCurrentUserHomeForLogin` on the cursor-login backend.

A stretch plays `--segment-turns` turns (default 5) and stops after `--segment-minutes` (default 60). The run stops after `--turns` (default 10), when a stretch fails, or when the turn did not advance. Ctrl+C closes the match. `--keep-open` leaves the game and the lock in place.

With `--request` and no spec, the first stretch writes `spec.json` and runs `setup`, then plays. Each turn the agent rewrites `strategy.md` and checks it against this game's `rule.md` before giving orders. A new game starts from the selected rule, `basic` unless `--rule` names another. The person can edit that file from the dashboard; the previous text stays in the game's `agent/rules/`. The agent does not edit it. `--rule none` skips the file.

`--python` or `CIV2AGENT_PYTHON` selects the Python that runs the command lines.

Keep the game window on screen. Do not move the mouse while a `mouse` dialog is being answered.

## Optional guides

A guide is a Markdown file next to `runner/guides/registry.json`. Register it there; do not hard-code the name.

```json
{
  "jin-yuan": { "title": "金元帝国", "file": "jin-yuan.md" },
  "jin-yuan-rush": { "title": "金元帝国·神级快攻", "file": "jin-yuan-rush.md" }
}
```

`runner/rules/basic.md` is the only rule text in git. Copy `registry.example.json` to `registry.json` and add other markdown files beside it; those stay on this machine. The same is true of guides: `runner/guides/*.md` and `registry.json` are local. The example registry shows the shape.

## Dashboard and MCP

The dashboard is a local page for the live match, plus an MCP server at `/mcp` so another machine can start, resume, checkpoint, close, and play. The gateway only forwards. Rules stay in `civ2session.py` and `civ2ctl.py`.

```text
cd dashboard
npm ci
npm start
```

The page is `http://127.0.0.1:4732`. MCP is `http://127.0.0.1:4732/mcp`. `GET /tools` lists every MCP tool that is registered now: name, description, whether it needs a lease, and its parameters. It accepts the same login as the page. Set `DASHBOARD_PORT` to change the port. The default bind is `127.0.0.1`, and a local client needs no token. To listen on the LAN, set `DASHBOARD_HOST=0.0.0.0` and `DASHBOARD_TOKEN`. Without a token the process refuses to bind beyond loopback. Startup then prints each LAN address. Allow that port in Windows Firewall yourself; the program does not add a rule.

MCP still wants `Authorization: Bearer <token>`. The page and `/events` do not accept that header from a browser, so open `/login` and post the same token. A `?token=` query logs in the same way, then the browser is sent to the address without that parameter. The login cookie is `HttpOnly` and lasts 180 days, so that browser opens the page directly next time. `/logout` clears it. The page's main controls start games: a request, an optional guide, how many games (1–20), and how many turns each game lasts (1–500). It runs `civ2-play.mjs` that many times on the computer that has the game. "接着玩这一局" continues the match already open. Stop ends that run and gives the lease back. Step-by-step controls (answer a dialog, end the turn, checkpoint, move, production, tax) stay folded away. A dialog whose `via` is `mouse` still moves the mouse on the computer that runs the game. Leave that game window on its screen.

The page lists past games under 历史记录. `GET /pack/<id>` downloads that game's zip, and `POST /pack` uploads one in the same shape. The user's pre-game backup is left out. An existing game, and the game in progress, are not overwritten.

One match and one mutating call at a time. `claim` (with a `client` name shown on the page) returns a `lease`. The same name claimed again gets the lease it already holds. A claim that finds someone else gets `busy` plus the holder name and the current lease. Start, resume, checkpoint, close, and play commands carry that lease. `release` gives it back. With no command in progress it can name the holder instead of the lease id, and the page's release button clears whoever is idle. The lease drops after 10 idle minutes (`GATE_LEASE_IDLE_MS`). When the game is not running and no command is in progress, the next claim from someone else takes an idle lease. A second mutation returns `busy` immediately and does not queue. A command that meets a status read waits for that read to finish. While the lease is held, `games/.lease` records the dashboard process. `civ2ctl.py` and `civ2session.py` return `busy` for any pipe call unless `CIV2AGENT_LEASE` is that lease, so the local runner yields the pipe. A dead process id does not keep the lock.

`list_games`, `game_status`, and `read_game` need no lease. `status`, `state`, and `dialog` need no lease either; while the lock is held they return `busy` and the last snapshot instead of sending another command. Play commands such as `respond` and `move` need the lease. `save-game`, `load-game`, and `quit-game` are not on the gateway; use `checkpoint`, `resume_game`, and `close_game`.

## Tests

Unit tests do not need the game:

```text
python -m unittest discover -s tests -p "test_*.py"
cd runner && npm test
cd dashboard && npm test
```

`tests/smoke_hello.py` and `tests/session_check.py` start the real game. Run them only when you mean to, with no other match active. `session_check.py` quits the game itself and checks that the install's saves and `.DAT` files are unchanged.

## Environment

`.env` in the project root fills variables that are not already set. `CIV2AGENT_LEASE` is not read from `.env`; the dashboard sets it for one call.

| Variable | Meaning |
| --- | --- |
| `CIV2AGENT_GAME` | Directory that contains `civ2.exe`. The process value wins, then `.env`, then `game.json`. |
| `CIV2AGENT_GAMES_DIR` | Where match folders live. Default `games/`. |
| `CIV2AGENT_PYTHON` | Python used by the runner and the dashboard. |
| `CIV2AGENT_LEASE` | Set by the dashboard on its own commands so they may use the pipe while `games/.lease` is held. |
| `DASHBOARD_HOST` | Bind address. Default `127.0.0.1`. |
| `DASHBOARD_PORT` | Default `4732`. |
| `DASHBOARD_TOKEN` | Required when the bind is not loopback. |
| `GATE_LEASE_IDLE_MS` | How long an idle MCP lease is kept. Default 600000. |
