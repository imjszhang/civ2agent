const link = document.querySelector("#link");
const gateway = document.querySelector("#gateway");
const game = document.querySelector("#game");
const mapCanvas = document.querySelector("#map-canvas");
const mapDetail = document.querySelector("#map-detail");
const runBox = document.querySelector("#run");
const prompt = document.querySelector("#prompt");
const thoughts = document.querySelector("#thoughts");
const actions = document.querySelector("#actions");
const commands = document.querySelector("#commands");

const state = { game: null, gateway: null, session: null, run: null, earlier: null, prompt: "", thoughts: [], actions: [], commands: [] };

const KEEP_COMMANDS = 80;
const KEEP_ACTIONS = 80;
const KEEP_THOUGHTS = 40;
const THOUGHT_CHARS = 4000;

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

function renderGateway() {
  gateway.replaceChildren();
  const view = state.gateway;
  if (!view) return;
  const lines = [
    view.lan ? "局域网已开放" : "仅本机",
    view.holder ? `持有者 ${view.holder}` : "无人持有租约",
  ];
  if (view.command) lines.push(`远程操作进行中：${view.command}${view.since ? "　自 " + view.since.slice(11, 19) : ""}`);
  else lines.push("空闲");
  if (view.last) lines.push(`最近 ${view.last.command}${view.last.ok ? " 完成" : "　" + (view.last.reason || "失败")}`);
  for (const text of lines) {
    const line = document.createElement("p");
    if (view.command && text.startsWith("远程操作进行中")) line.className = "busy";
    line.textContent = text;
    gateway.append(line);
  }
}

const TERRAIN = ["沙漠", "平原", "草原", "森林", "丘陵", "山脉", "冻土", "冰川", "沼泽", "丛林", "海洋"];
const SPECIAL = ["绿洲", "小麦", "资源", "野味", "煤", "金", "野味", "石油", "香料", "宝石", "鱼"];
const TERRAIN_COLOR = ["#c2a15a", "#d2c48a", "#7cb342", "#2e7d32", "#9e9a78", "#757069", "#d5ddd8", "#eef3f6", "#6d7a55", "#1b5e40", "#3d7ea6"];
const MAP_CELL = 12;
const MAP_PAD = 6;
const mapGeom = { pad: MAP_PAD, cell: MAP_CELL, tiles: new Map() };
let pickedKey = "";

function tileParts(tile) {
  const t = tile.t ?? 0;
  const f = tile.f ?? 0;
  const kind = t & 0x0f;
  const works = [];
  if (f & 0x02) works.push("城市");
  if ((f & 0x30) === 0x30) works.push("铁路");
  else if (f & 0x10) works.push("道路");
  if ((f & 0x0c) === 0x0c) works.push("农田");
  else {
    if (f & 0x04) works.push("灌溉");
    if (f & 0x08) works.push("矿山");
  }
  if (f & 0x40) works.push("要塞");
  if (f & 0x80) works.push("污染");
  return {
    name: TERRAIN[kind] || `地形 ${kind}`,
    river: (t & 0x80) !== 0,
    special: (t & 0x40) === 0 ? (SPECIAL[kind] || "特产") : "",
    works,
    color: TERRAIN_COLOR[kind] || "#8a8478",
  };
}

function occupants(x, y, view) {
  const here = item => item.x === x && item.y === y;
  const names = [];
  for (const city of view.cities || []) if (here(city)) names.push(`${city.name || "城市"}（人口 ${cell(city.size)}）`);
  for (const city of view.foreignCities || []) if (here(city)) names.push(`${city.name || "外国城市"}（文明 ${cell(city.owner)}，人口 ${cell(city.size)}）`);
  for (const unit of view.units || []) if (here(unit)) names.push(unit.name || "单位");
  for (const unit of view.foreignUnits || []) if (here(unit)) names.push(`${unit.name || "外国单位"}（文明 ${cell(unit.civ)}）`);
  return names;
}

function tileLine(tile, view) {
  const parts = tileParts(tile);
  const bits = [`${tile.x},${tile.y}`, parts.name];
  if (parts.river) bits.push("河流");
  if (parts.special) bits.push(parts.special);
  if (parts.works.length) bits.push(parts.works.join("、"));
  const who = occupants(tile.x, tile.y, view);
  if (who.length) bits.push(who.join("、"));
  return bits.join("　");
}

function tileFromEvent(ev) {
  const rect = mapCanvas.getBoundingClientRect();
  const localX = ev.clientX - rect.left - mapGeom.pad;
  const localY = ev.clientY - rect.top - mapGeom.pad;
  const half = mapGeom.cell / 2;
  let x = Math.round(localX / half);
  let y = Math.round(localY / half);
  if (((x + y) & 1) !== 0) {
    if (Math.abs(localX - x * half) <= Math.abs(localY - y * half)) x += localX > x * half ? 1 : -1;
    else y += localY > y * half ? 1 : -1;
  }
  return mapGeom.tiles.get(`${x},${y}`) || null;
}

function renderMap() {
  const view = state.game;
  const tiles = view?.tiles || [];
  mapGeom.tiles = new Map(tiles.map(tile => [`${tile.x},${tile.y}`, tile]));
  if (!view?.connected || !view.inGame || !tiles.length) {
    mapCanvas.width = 0;
    mapCanvas.height = 0;
    mapCanvas.style.width = "0";
    mapCanvas.style.height = "0";
    mapDetail.textContent = view?.connected && view.inGame ? "还没有探索到的格子" : "还没有探索到的格子";
    return;
  }
  let maxX = 0;
  let maxY = 0;
  for (const tile of tiles) {
    if (tile.x > maxX) maxX = tile.x;
    if (tile.y > maxY) maxY = tile.y;
  }
  const width = Math.ceil((maxX + 2) * (MAP_CELL / 2)) + MAP_PAD * 2;
  const height = Math.ceil((maxY + 2) * (MAP_CELL / 2)) + MAP_PAD * 2;
  const ratio = window.devicePixelRatio || 1;
  mapCanvas.style.width = `${width}px`;
  mapCanvas.style.height = `${height}px`;
  mapCanvas.width = Math.floor(width * ratio);
  mapCanvas.height = Math.floor(height * ratio);
  const ctx = mapCanvas.getContext("2d");
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, height);
  for (const tile of tiles) {
    const parts = tileParts(tile);
    const px = MAP_PAD + tile.x * (MAP_CELL / 2);
    const py = MAP_PAD + tile.y * (MAP_CELL / 2);
    ctx.fillStyle = parts.color;
    ctx.fillRect(px, py, MAP_CELL - 1, MAP_CELL - 1);
    if (parts.river) {
      ctx.fillStyle = "#8ec8e8";
      ctx.fillRect(px, py + MAP_CELL - 4, MAP_CELL - 1, 2);
    }
    if (parts.works.includes("铁路") || parts.works.includes("道路")) {
      ctx.fillStyle = "#5a4030";
      ctx.fillRect(px + 2, py + MAP_CELL / 2 - 1, MAP_CELL - 5, 2);
    }
  }
  const mark = (x, y, color, inset) => {
    ctx.fillStyle = color;
    const px = MAP_PAD + x * (MAP_CELL / 2);
    const py = MAP_PAD + y * (MAP_CELL / 2);
    ctx.fillRect(px + inset, py + inset, MAP_CELL - 1 - inset * 2, MAP_CELL - 1 - inset * 2);
  };
  for (const city of view.foreignCities || []) if (city.x !== undefined) mark(city.x, city.y, "#f4e2b0", 3);
  for (const city of view.cities || []) if (city.x !== undefined) mark(city.x, city.y, "#1c1915", 3);
  for (const unit of view.foreignUnits || []) if (unit.x !== undefined) mark(unit.x, unit.y, "#8d2a2a", 4);
  for (const unit of view.units || []) if (unit.x !== undefined) mark(unit.x, unit.y, "#f7f4ee", 4);
  const picked = mapGeom.tiles.get(pickedKey);
  mapDetail.textContent = picked ? tileLine(picked, view) : `已探索 ${tiles.length} 格。点一格看地形。`;
}

mapCanvas.addEventListener("mousemove", ev => {
  const view = state.game;
  const tile = view ? tileFromEvent(ev) : null;
  if (tile && view) mapDetail.textContent = tileLine(tile, view);
});
mapCanvas.addEventListener("click", ev => {
  const tile = tileFromEvent(ev);
  pickedKey = tile ? `${tile.x},${tile.y}` : "";
  if (tile && state.game) mapDetail.textContent = tileLine(tile, state.game);
});
mapCanvas.addEventListener("mouseleave", () => {
  const picked = mapGeom.tiles.get(pickedKey);
  if (picked && state.game) mapDetail.textContent = tileLine(picked, state.game);
  else if (state.game?.tiles?.length) mapDetail.textContent = `已探索 ${state.game.tiles.length} 格。点一格看地形。`;
});

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

function nearEnd(el) {
  return el.scrollHeight - el.scrollTop - el.clientHeight < 32;
}

function trimChildren(el, limit) {
  while (el.children.length > limit) el.firstElementChild.remove();
}

function keepText(text) {
  return text.length > THOUGHT_CHARS ? text.slice(-THOUGHT_CHARS) : text;
}

function thoughtNode(thought) {
  const p = document.createElement("p");
  p.className = thought.done ? "thought" : "thought open";
  p.textContent = thought.text;
  return p;
}

function actionNode(action) {
  const li = document.createElement("li");
  li.className = action.kind === "tool" ? "tool" : "";
  li.textContent = action.text;
  return li;
}

function commandNode(command) {
  const li = document.createElement("li");
  li.className = command.ok ? "command ok" : "command bad";
  li.textContent = `${command.time.slice(11, 19)} ${command.command}${command.ok ? "" : "　" + (command.reason || command.error)}`;
  return li;
}

function renderThoughts() {
  thoughts.replaceChildren();
  for (const thought of state.thoughts) thoughts.append(thoughtNode(thought));
  thoughts.scrollTop = thoughts.scrollHeight;
}

function renderActions() {
  actions.replaceChildren();
  for (const action of state.actions) actions.append(actionNode(action));
  actions.scrollTop = actions.scrollHeight;
}

function renderCommands() {
  commands.replaceChildren();
  for (const command of state.commands) commands.append(commandNode(command));
  commands.scrollTop = commands.scrollHeight;
}

function renderAll() {
  renderGateway();
  renderMap();
  renderGame();
  renderRun();
  prompt.textContent = state.prompt;
  renderThoughts();
  renderActions();
  renderCommands();
}

function onThought(data) {
  const follow = nearEnd(thoughts);
  const last = state.thoughts.at(-1);
  const node = thoughts.lastElementChild;
  if (data.done) {
    if (last) last.done = true;
    if (node) node.className = "thought";
  } else if (last && !last.done && node) {
    last.text = keepText(last.text + data.text);
    node.textContent = last.text;
  } else {
    const thought = { text: keepText(data.text), done: false };
    state.thoughts.push(thought);
    if (state.thoughts.length > KEEP_THOUGHTS) state.thoughts.shift();
    thoughts.append(thoughtNode(thought));
    trimChildren(thoughts, KEEP_THOUGHTS);
  }
  if (follow) thoughts.scrollTop = thoughts.scrollHeight;
}

const source = new EventSource("/events");
source.onopen = () => { link.textContent = "已连接观察页"; };
source.onerror = () => { link.textContent = "观察页连接中断，正在重试"; };
source.addEventListener("snapshot", event => {
  Object.assign(state, JSON.parse(event.data));
  renderAll();
});
source.addEventListener("gateway", event => { state.gateway = JSON.parse(event.data); renderGateway(); });
source.addEventListener("game", event => { state.game = JSON.parse(event.data); renderMap(); renderGame(); });
source.addEventListener("session", event => { state.session = JSON.parse(event.data); renderRun(); });
source.addEventListener("run", event => { state.run = JSON.parse(event.data); renderRun(); });
source.addEventListener("prompt", event => { if (!state.prompt) { state.prompt = JSON.parse(event.data).text; prompt.textContent = state.prompt; } });
source.addEventListener("thought", event => onThought(JSON.parse(event.data)));
source.addEventListener("action", event => {
  const follow = nearEnd(actions);
  state.actions.push(JSON.parse(event.data));
  if (state.actions.length > KEEP_ACTIONS) state.actions.shift();
  actions.append(actionNode(state.actions.at(-1)));
  trimChildren(actions, KEEP_ACTIONS);
  if (follow) actions.scrollTop = actions.scrollHeight;
});
source.addEventListener("command", event => {
  const follow = nearEnd(commands);
  state.commands.push(JSON.parse(event.data));
  if (state.commands.length > KEEP_COMMANDS) state.commands.shift();
  commands.append(commandNode(state.commands.at(-1)));
  trimChildren(commands, KEEP_COMMANDS);
  if (follow) commands.scrollTop = commands.scrollHeight;
});
source.addEventListener("earlier", event => { state.earlier = JSON.parse(event.data); renderRun(); });
source.addEventListener("reset", event => {
  const scope = JSON.parse(event.data).scope;
  state.run = null; state.prompt = ""; state.thoughts = []; state.actions = [];
  if (scope === "session") { state.commands = []; state.session = null; state.earlier = null; }
  renderAll();
});
