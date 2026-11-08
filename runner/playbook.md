# 玩法手册

你是这一局《文明 II》唯一的玩家。游戏开在屏幕上，你通过两个命令行操作它，和人类玩家一样：只做玩家在界面上能做的事，只用玩家在画面上能看到的信息。任务说明里给了这两个命令行和命令说明（protocol.md）的路径。

## 规则

- 只用 `civ2ctl.py` 和 `civ2session.py` 操作游戏，每次调用输出一行 JSON。不要读写游戏进程的内存，不要读 `.SAV` 文件的内容，不要改游戏目录里的文件，也不要碰别的游戏局目录。
- 不要读档、不要离开游戏（`load-game`、`quit-game` 在游戏局进行中会被拒绝），不要 `civ2session.py close`：开局、续玩和收尾由运行你的程序负责。
- 不要用 Windows 的鼠标或键盘去点游戏窗口。少数对话框（`via` 为 `mouse`）由 `respond` 自己用真实鼠标点。
- 你的工作目录就是这局的 `agent/`。笔记写在 `notes.md`，这一回合的打法写在 `strategy.md`。这两个文件留在这个目录里，不要写到目录外面。`rule.md` 不要改。

## 开始

1. 读 `context.md`（这一段的目标和这局现在的样子）、`brief.md`（用户的要求）、`notes.md`（你之前的想法）。有 `guide.md` 就从第一行通读到最后一行。有 `rule.md` 就读完。攻略里的鼠标和按键不要照做，操作以本手册为准。有上一份 `strategy.md` 时，它只说明上次怎么定的。
2. `civ2ctl.py dialog` 看有没有等回答的对话框，`civ2ctl.py state --only civ,cities,units` 看局面。
3. 如果 `context.md` 说这局还没开局：按 `brief.md` 里用户的要求写 `../spec.json`（键和值见 protocol.md 的「开局设置」），再运行 `civ2session.py setup <这局的目录>`。用户没指定世界大小时，`map_size` 写 `large`（大，75×120）。用户的要求含糊时选最接近的设置，在 notes.md 里写下你是怎么理解的。setup 停在一个对话框上时，用 `respond` 回答它，再 `civ2session.py checkpoint`。

## 每一回合

1. 先处理对话框：`dialog` 不为 null 就用 `respond` 回答（看清 options 再选）。研究框的 `options` 只有科技，`buttons` 里才有「确定」。先 `respond` 要研究的科技名，等 `selected` 变成那一行，再 `respond --text 确定`。不要在选中之前按「确定」，那会锁住默认行。「选择现役部队」的每一行是「兵种 #单位index」，`respond --text` 选要动的那一行，不要点「确定」。建城后的命名也在对话框里。
2. 回合开始、没有对话框时运行一次 `civ2session.py checkpoint`（存这一回合）。同一回合重复运行没关系。返回 `modal_open` 就先回答对话框。这一步会把上一回合留下的 `strategy.md` 按其中的回合号抄进 `strategy/`，已有的那一份不覆盖。不要删除 `strategy/`。
3. 看局面：`state --only civ,cities,units,foreign_units,foreign_cities,diplomacy`。`tiles` 很大，只在要看地形时取。
4. 覆盖写成 `strategy.md`。文件开头写「第 N 回合」，N 是现在这一回合。再写长期目标、中期目标、短期目标，每一个都写明「到第 M 回合」要达成什么；M 按攻略和局面来定，三个期限各不相同。现在的回合还没过那个 M，而且目标仍然对得上画面，就原样抄过来，不要换一种说法；有 `guide.md` 时还要对得上攻略。违反 `rule.md` 里生效的法则，也算对不上。回合已经过了 M、目标已经做到，或对不上，才改那一个，并写上新的 M。还没有目标时，按局面补上，有攻略就同时按攻略。然后按这些目标，重写这一回合的做法：研究走哪条、城市造什么、税率怎么分、先不做的事。生产和研究用画面上的中文名。写完后按 `rule.md` 核对：生效的法则不通过，就改冲突的那个目标并再核对，最多改两轮。在文件末尾写下「法则核对」。通过之后按 `strategy.md` 做下一步。
5. 做这一回合的事：城市生产（`produce`）、单位行动（`move`、`goto`、`order`、`found-city`）、税率（`rates`）。
6. `end-turn`。返回 `modal_open` 时回答对话框；`turn_ended` 为 false 才再发一次 `end-turn`。关掉回合间消息后再 `end-turn`，返回的回合就是要玩的那一回合。

`events` 可以看到上次以来发生的事（单位损失、城市易手、研究完成）。

## 结束这一段

到了 `context.md` 里的目标回合，或者遇到解决不了的问题时：

回合推不下去时，把最后一次 `dialog` 的标题和返回的 `reason` 写进 `notes.md`，然后停。不要枚举窗口，不要改游戏进程的内存。

1. 先把对话框都回答掉，运行 `civ2session.py checkpoint`。
2. 更新 `notes.md`：战略、接下来的计划、学到的教训。下一段的你只能从这里知道现在的想法。
3. 最后用一两句中文回答：这一段做了什么，局面怎样，有什么没解决。
