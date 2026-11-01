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

const GOVERNMENT = ["无政府", "专制", "君主", "共产", "原教旨", "共和", "民主"];
const ORDERS = { [-1]: "无命令", 0: "无命令", 1: "设防中", 2: "已设防", 3: "睡眠", 6: "灌溉" };
const TERRAIN = ["沙漠", "平原", "草原", "森林", "丘陵", "山脉", "冻土", "冰川", "沼泽", "丛林", "海洋"];
const SPECIAL = ["绿洲", "小麦", "资源", "野味", "煤", "金", "野味", "石油", "香料", "宝石", "鱼"];
const TERRAIN_COLOR = ["#c2a15a", "#d2c48a", "#7cb342", "#2e7d32", "#9e9a78", "#757069", "#d5ddd8", "#eef3f6", "#6d7a55", "#1b5e40", "#3d7ea6"];
// East is x + 2, south is y + 2. A tile is the same 2:1 diamond the game draws.
const MAP_W = 32;
const MAP_H = 16;
const MAP_PAD = 12;
const mapGeom = { pad: MAP_PAD, hw: MAP_W / 2, hh: MAP_H / 2, tiles: new Map() };
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

function orderText(orders) {
  if (orders === undefined || orders === null) return "";
  return ORDERS[orders] || `命令 ${orders}`;
}

function movesText(left, moveUnit) {
  if (left === undefined || left === null) return "";
  if (!moveUnit) return String(left);
  const whole = Math.floor(left / moveUnit);
  const rem = left % moveUnit;
  if (!rem) return String(whole);
  return whole ? `${whole} ${rem}/${moveUnit}` : `${rem}/${moveUnit}`;
}

function builtCount(bits) {
  if (!Array.isArray(bits)) return 0;
  let count = 0;
  for (const byte of bits) {
    let value = byte || 0;
    while (value) { count += value & 1; value >>= 1; }
  }
  return count;
}

function unitBits(unit, view, active) {
  const bits = [];
  if (active) bits.push("选中");
  const order = orderText(unit.orders);
  if (order) bits.push(order);
  if (unit.veteran) bits.push("老兵");
  if (unit.hp_lost) bits.push(`伤 ${unit.hp_lost}`);
  if (unit.health !== undefined) bits.push(`血 ${unit.health}`);
  if (unit.att !== undefined || unit.def !== undefined) bits.push(`攻 ${cell(unit.att)} 防 ${cell(unit.def)}`);
  const moves = movesText(unit.moves_left, view?.moveUnit);
  if (moves) bits.push(`移动 ${moves}`);
  if (unit.stacked) bits.push("叠放");
  return bits;
}

function occupants(x, y, view) {
  const here = item => item.x === x && item.y === y;
  const names = [];
  for (const city of view.cities || []) if (here(city)) names.push(`${city.name || "城市"}（人口 ${cell(city.size)}，粮 ${cell(city.food)}，盾 ${cell(city.shields)}，建筑 ${builtCount(city.improvements)}）`);
  for (const city of view.foreignCities || []) if (here(city)) names.push(`${city.name || "外国城市"}（文明 ${cell(city.owner)}，人口 ${cell(city.size)}）`);
  for (const unit of view.units || []) if (here(unit)) names.push(`${unit.name || "单位"}（${unitBits(unit, view, unit.index === view.activeUnit).join("，")}）`);
  for (const unit of view.foreignUnits || []) if (here(unit)) names.push(`${unit.name || "外国单位"}（文明 ${cell(unit.civ)}，${unitBits(unit, view, false).join("，")}）`);
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

function tileCenter(x, y) {
  return [mapGeom.pad + x * mapGeom.hw, mapGeom.pad + y * mapGeom.hh];
}

function fillDiamond(ctx, x, y) {
  const [cx, cy] = tileCenter(x, y);
  const hw = mapGeom.hw;
  const hh = mapGeom.hh;
  ctx.beginPath();
  ctx.moveTo(cx, cy - hh);
  ctx.lineTo(cx + hw, cy);
  ctx.lineTo(cx, cy + hh);
  ctx.lineTo(cx - hw, cy);
  ctx.closePath();
  ctx.fill();
}

function tileFromEvent(ev) {
  const rect = mapCanvas.getBoundingClientRect();
  const localX = ev.clientX - rect.left;
  const localY = ev.clientY - rect.top;
  let x = Math.round((localX - mapGeom.pad) / mapGeom.hw);
  let y = Math.round((localY - mapGeom.pad) / mapGeom.hh);
  if (((x + y) & 1) !== 0) {
    const [cx, cy] = tileCenter(x, y);
    if (Math.abs(localX - cx) <= Math.abs(localY - cy) * (mapGeom.hw / mapGeom.hh)) x += localX > cx ? 1 : -1;
    else y += localY > cy ? 1 : -1;
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
  const width = MAP_PAD + maxX * mapGeom.hw + mapGeom.hw + MAP_PAD;
  const height = MAP_PAD + maxY * mapGeom.hh + mapGeom.hh + MAP_PAD;
  const ratio = window.devicePixelRatio || 1;
  mapCanvas.style.width = `${width}px`;
  mapCanvas.style.height = `${height}px`;
  mapCanvas.width = Math.floor(width * ratio);
  mapCanvas.height = Math.floor(height * ratio);
  const ctx = mapCanvas.getContext("2d");
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, height);
  const ordered = [...tiles].sort((a, b) => a.y - b.y || a.x - b.x);
  for (const tile of ordered) {
    const parts = tileParts(tile);
    ctx.fillStyle = parts.color;
    fillDiamond(ctx, tile.x, tile.y);
    const [cx, cy] = tileCenter(tile.x, tile.y);
    if (parts.river) {
      ctx.strokeStyle = "#8ec8e8";
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(cx - mapGeom.hw / 2, cy);
      ctx.lineTo(cx + mapGeom.hw / 2, cy);
      ctx.stroke();
    }
    if (parts.works.includes("农田")) {
      ctx.fillStyle = "#d6e35a";
      ctx.fillRect(cx - 4, cy - 2, 8, 4);
    } else if (parts.works.includes("灌溉")) {
      ctx.fillStyle = "#3aa0c8";
      ctx.fillRect(cx - 5, cy - 1, 4, 3);
    }
    if (parts.works.includes("矿山")) {
      ctx.fillStyle = "#3a342c";
      ctx.fillRect(cx + 1, cy - 2, 4, 4);
    }
    if (parts.works.includes("要塞")) {
      ctx.strokeStyle = "#f7f4ee";
      ctx.lineWidth = 1;
      ctx.strokeRect(cx - 3, cy - 3, 6, 6);
    }
    if (parts.works.includes("污染")) {
      ctx.fillStyle = "#c43b3b";
      ctx.beginPath();
      ctx.arc(cx, cy - 3, 2, 0, Math.PI * 2);
      ctx.fill();
    }
    if (parts.works.includes("铁路") || parts.works.includes("道路")) {
      ctx.strokeStyle = parts.works.includes("铁路") ? "#3a3530" : "#6b5344";
      ctx.lineWidth = parts.works.includes("铁路") ? 2 : 1;
      ctx.beginPath();
      ctx.moveTo(cx - mapGeom.hw + 3, cy);
      ctx.lineTo(cx + mapGeom.hw - 3, cy);
      ctx.stroke();
    }
  }
  const stamp = (x, y, text, fill) => {
    const [cx, cy] = tileCenter(x, y);
    ctx.font = "12px Microsoft YaHei, Segoe UI, sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.lineWidth = 3;
    ctx.strokeStyle = "#f7f4ee";
    ctx.strokeText(text, cx, cy + mapGeom.hh + 7);
    ctx.fillStyle = fill;
    ctx.fillText(text, cx, cy + mapGeom.hh + 7);
    ctx.fillRect(cx - 3, cy - 3, 6, 6);
  };
  const stacks = new Map();
  const addStack = (x, y, name) => {
    const key = `${x},${y}`;
    if (!stacks.has(key)) stacks.set(key, []);
    stacks.get(key).push(name || "单位");
  };
  for (const unit of view.foreignUnits || []) if (unit.x !== undefined) addStack(unit.x, unit.y, unit.name);
  for (const unit of view.units || []) if (unit.x !== undefined) addStack(unit.x, unit.y, unit.name);
  for (const [key, names] of stacks) {
    const [x, y] = key.split(",").map(Number);
    const counts = new Map();
    for (const name of names) counts.set(name, (counts.get(name) || 0) + 1);
    const text = [...counts].map(([name, count]) => count > 1 ? `${name}×${count}` : name).join(" ");
    stamp(x, y, text, "#8d2a2a");
  }
  for (const city of view.foreignCities || []) {
    if (city.x === undefined) continue;
    stamp(city.x, city.y, `${city.name || "城市"} ${cell(city.size)}`, "#1a4f86");
  }
  for (const city of view.cities || []) {
    if (city.x === undefined) continue;
    stamp(city.x, city.y, `${city.name || "城市"} ${cell(city.size)}`, "#1c1915");
  }
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
  const government = GOVERNMENT[civ.government] || "";
  const techs = Array.isArray(civ.techs) ? civ.techs.length : null;
  brief.textContent = `${civ.name || ""} ${civ.leader || ""} ${civ.title || ""}　${government}　金钱 ${cell(civ.gold)}　税收 ${rate(civ.tax_rate)}　科学 ${rate(civ.science_rate)}　奢侈 ${rate(civ.lux_rate)}${techs === null ? "" : `　科技 ${techs}`}`;
  const research = document.createElement("p");
  research.textContent = `研究 ${civ.research_name || "无"}　${cell(civ.beakers)} / ${cell(civ.research_cost)}`;
  game.append(brief, research);
  if (view.notice && (view.notice.title || view.notice.text)) {
    const note = document.createElement("p");
    note.textContent = `${view.notice.title || "提示"}　${view.notice.text || ""}`;
    game.append(note);
  }
  if (view.cityWindow) {
    const windowLine = document.createElement("p");
    windowLine.textContent = `城市窗口 ${view.cityWindow.name || ""}${view.cityWindow.modal ? "　等着关上" : ""}`;
    game.append(windowLine);
  }
  if (view.taxWindow) {
    const tax = view.taxWindow;
    const windowLine = document.createElement("p");
    windowLine.textContent = `税率窗口　税收 ${rate(tax.tax_rate)}　科学 ${rate(tax.science_rate)}　奢侈 ${rate(tax.lux_rate)}`;
    game.append(windowLine);
  }
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
  if (view.cities.length) game.append(table(["城市", "人口", "粮", "盾", "建筑", "生产"], view.cities, city => [city.name, city.size, city.food, city.shields, builtCount(city.improvements), city.building_name]));
  if (view.units.length) game.append(table(["单位", "位置", "移动", "状态"], view.units, unit => [
    unit.index === view.activeUnit ? `▶ ${unit.name || ""}` : unit.name,
    `${unit.x},${unit.y}`,
    movesText(unit.moves_left, view.moveUnit),
    unitBits(unit, view, unit.index === view.activeUnit).join(" "),
  ]));
  if (view.diplomacy.length) game.append(table(["文明", "状态", "使馆"], view.diplomacy, row => [row.name || row.civ, row.status, row.embassy ? "有" : "无"]));
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
