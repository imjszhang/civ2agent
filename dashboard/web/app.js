const link = document.querySelector("#link");
const game = document.querySelector("#game");
const runBox = document.querySelector("#run");
const prompt = document.querySelector("#prompt");
const thoughts = document.querySelector("#thoughts");
const actions = document.querySelector("#actions");
const commands = document.querySelector("#commands");

const state = { game: null, session: null, run: null, earlier: null, prompt: "", thoughts: [], actions: [], commands: [] };

const STATUS = { running: "进行中", completed: "已完成", failed: "失败", cancelled: "已取消", timed_out: "超时" };
const EFFORT = { none: "无", minimal: "最低", low: "低", medium: "中", high: "高", xhigh: "很高", max: "最大", ultra: "极高" };
const num = value => new Intl.NumberFormat("zh-CN").format(value);

const yearText = year => year === null || year === undefined ? "—" : year < 0 ? `公元前 ${-year} 年` : `公元 ${year} 年`;
const rate = value => value === undefined ? "—" : `${value * 10}%`;
const cell = value => value === undefined || value === null ? "" : String(value);

function row(cells) {
  const tr = document.createElement("tr");
  for (const value of cells) {
    const td = document.createElement("td");
    td.textContent = cell(value);
    tr.append(td);
  }
  return tr;
}

function table(headers, records, pick) {
  const el = document.createElement("table");
  const head = document.createElement("tr");
  for (const name of headers) {
    const th = document.createElement("th");
    th.textContent = name;
    head.append(th);
  }
  el.append(head);
  for (const record of records) el.append(row(pick(record)));
  return el;
}

function renderGame() {
  game.replaceChildren();
  const view = state.game;
  if (!view) return;
  const status = document.createElement("p");
  if (!view.connected) status.textContent = view.reason || "游戏没有开";
  else if (view.busy) { status.className = "busy"; status.textContent = view.reason || "游戏正忙"; }
  else status.textContent = view.inGame ? `第 ${view.turn ?? "—"} 回合，${yearText(view.year)}` : "已连接，不在对局中";
  game.append(status);
  if (!view.connected || !view.inGame) return;
  const civ = view.civ || {};
  const brief = document.createElement("p");
  brief.textContent = `${civ.name || ""} ${civ.leader || ""} ${civ.title || ""}　金钱 ${cell(civ.gold)}　税收 ${rate(civ.tax_rate)}　科学 ${rate(civ.science_rate)}　奢侈 ${rate(civ.lux_rate)}`;
  const research = document.createElement("p");
  research.textContent = `研究 ${civ.research_name || "无"}　${cell(civ.beakers)} / ${cell(civ.research_cost)}`;
  game.append(brief, research);
  if (view.dialog) {
    const box = document.createElement("div");
    const title = document.createElement("strong");
    title.textContent = view.dialog.title || "对话框";
    const text = document.createElement("pre");
    text.textContent = view.dialog.text || "";
    box.append(title, text);
    for (const option of view.dialog.options || []) {
      const line = document.createElement("div");
      line.textContent = option.text || "";
      box.append(line);
    }
    game.append(box);
  }
  if (view.cities.length) game.append(table(["城市", "人口", "位置", "生产"], view.cities, city => [city.name, city.size, `${city.x},${city.y}`, city.building_name]));
  if (view.units.length) game.append(table(["单位", "位置", "移动", "命令"], view.units, unit => [unit.name, `${unit.x},${unit.y}`, unit.moves_left, unit.orders]));
  if (view.diplomacy.length) game.append(table(["文明", "状态", "使馆"], view.diplomacy, row => [row.civ, row.status, row.embassy ? "有" : "无"]));
}

function effortText(value) {
  if (!value) return "未设置";
  return EFFORT[value] ?? value;
}

function modelText(view) {
  const reported = view.model;
  const requested = view.requestedModel;
  if (!reported && !requested) return "—";
  if (!reported) return `${requested}（尚未回报）`;
  if (!requested || requested.toLowerCase() === reported.toLowerCase()) return reported;
  return `${reported}（配置 ${requested}）`;
}

function usageText(usage) {
  if (!usage) return "还没有上报";
  const parts = [`输入 ${num(usage.input)}`, `输出 ${num(usage.output)}`];
  if (usage.cached !== null) parts.push(`缓存读 ${num(usage.cached)}`);
  if (usage.cacheWrite !== null) parts.push(`缓存写 ${num(usage.cacheWrite)}`);
  if (usage.reasoning !== null) parts.push(`推理 ${num(usage.reasoning)}`);
  return parts.join(" · ");
}

function addTokens(left, right) {
  if (!left) return right;
  if (!right) return left;
  const field = (a, b) => a === null && b === null ? null : (a ?? 0) + (b ?? 0);
  return {
    input: left.input + right.input,
    output: left.output + right.output,
    cached: field(left.cached, right.cached),
    cacheWrite: field(left.cacheWrite, right.cacheWrite),
    reasoning: field(left.reasoning, right.reasoning),
    total: left.total + right.total,
  };
}

function stat(dl, label, value, total) {
  const dt = document.createElement("dt");
  dt.textContent = label;
  const dd = document.createElement("dd");
  if (total) dd.className = "total";
  dd.textContent = value;
  dl.append(dt, dd);
}

function renderRun() {
  runBox.replaceChildren();
  const view = state.run;
  const session = state.session;
  if (!session && !view) {
    runBox.textContent = "这一段还没开始";
    return;
  }
  if (session) {
    const title = document.createElement("p");
    title.textContent = `${session.title}（${session.status || "进行中"}）`;
    runBox.append(title);
  }
  if (!view) return;
  const dl = document.createElement("dl");
  dl.className = "stats";
  stat(dl, "适配器", [view.adapter, view.target].filter(Boolean).join(" · ") || "—");
  stat(dl, "状态", STATUS[view.status] || view.status || "—");
  stat(dl, "模型", modelText(view));
  stat(dl, "推理强度", effortText(view.effort));
  stat(dl, "用量", usageText(view.usage));
  if (view.usage) stat(dl, state.earlier ? "本段合计" : "合计", num(view.usage.total), true);
  if (state.earlier) stat(dl, "本局合计", num(addTokens(state.earlier, view.usage).total), true);
  runBox.append(dl);
}

function renderThoughts() {
  thoughts.replaceChildren();
  for (const thought of state.thoughts) {
    const p = document.createElement("p");
    p.className = thought.done ? "thought" : "thought open";
    p.textContent = thought.text;
    thoughts.append(p);
  }
  thoughts.scrollTop = thoughts.scrollHeight;
}

function renderActions() {
  actions.replaceChildren();
  for (const action of state.actions) {
    const li = document.createElement("li");
    li.className = action.kind === "tool" ? "tool" : "";
    li.textContent = action.text;
    actions.append(li);
  }
  actions.scrollTop = actions.scrollHeight;
}

function renderCommands() {
  commands.replaceChildren();
  for (const command of state.commands) {
    const li = document.createElement("li");
    li.className = command.ok ? "command ok" : "command bad";
    li.textContent = `${command.time.slice(11, 19)} ${command.command}${command.ok ? "" : "　" + (command.reason || command.error)}`;
    commands.append(li);
  }
  commands.scrollTop = commands.scrollHeight;
}

function renderAll() {
  renderGame();
  renderRun();
  prompt.textContent = state.prompt;
  renderThoughts();
  renderActions();
  renderCommands();
}

function onThought(data) {
  const last = state.thoughts.at(-1);
  if (data.done) { if (last) last.done = true; }
  else if (last && !last.done) last.text += data.text;
  else state.thoughts.push({ text: data.text, done: false });
  renderThoughts();
}

const source = new EventSource("/events");
source.onopen = () => { link.textContent = "已连接观察页"; };
source.onerror = () => { link.textContent = "观察页连接中断，正在重试"; };
source.addEventListener("snapshot", event => {
  Object.assign(state, JSON.parse(event.data));
  renderAll();
});
source.addEventListener("game", event => { state.game = JSON.parse(event.data); renderGame(); });
source.addEventListener("session", event => { state.session = JSON.parse(event.data); renderRun(); });
source.addEventListener("run", event => { state.run = JSON.parse(event.data); renderRun(); });
source.addEventListener("prompt", event => { if (!state.prompt) { state.prompt = JSON.parse(event.data).text; prompt.textContent = state.prompt; } });
source.addEventListener("thought", event => onThought(JSON.parse(event.data)));
source.addEventListener("action", event => { state.actions.push(JSON.parse(event.data)); renderActions(); });
source.addEventListener("command", event => { state.commands.push(JSON.parse(event.data)); renderCommands(); });
source.addEventListener("earlier", event => { state.earlier = JSON.parse(event.data); renderRun(); });
source.addEventListener("reset", event => {
  const scope = JSON.parse(event.data).scope;
  state.run = null; state.prompt = ""; state.thoughts = []; state.actions = [];
  if (scope === "session") { state.commands = []; state.session = null; state.earlier = null; }
  renderAll();
});
