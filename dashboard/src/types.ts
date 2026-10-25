/** Fields this page reads from a js-agent-runtime RuntimeEvent. */
export type RuntimeEvent = {
  schemaVersion: 1;
  runId: string;
  sequence: number;
  time: string;
  type: "started" | "message" | "tool" | "usage" | "raw" | "diagnostic" | "completed" | "failed" | "cancelled" | "timed_out";
  data: unknown;
};

export type GameView = {
  connected: boolean;
  busy: boolean;
  reason: string;
  inGame: boolean;
  turn: number | null;
  year: number | null;
  civ: CivBrief | null;
  cities: CityBrief[];
  units: UnitBrief[];
  diplomacy: DiplomacyBrief[];
  dialog: DialogBrief | null;
  cityWindow: { name?: string; modal?: boolean } | null;
  taxWindow: { tax_rate?: number; science_rate?: number; lux_rate?: number } | null;
};

export type CivBrief = {
  name?: string;
  leader?: string;
  title?: string;
  gold?: number;
  tax_rate?: number;
  science_rate?: number;
  lux_rate?: number;
  research_name?: string;
  beakers?: number;
  research_cost?: number;
  government?: number;
};

export type CityBrief = {
  name?: string;
  x?: number;
  y?: number;
  size?: number;
  building_name?: string;
  food?: number;
  shields?: number;
};

export type UnitBrief = {
  name?: string;
  x?: number;
  y?: number;
  moves_left?: number;
  orders?: number;
};

export type DiplomacyBrief = {
  civ?: number;
  status?: string;
  embassy?: boolean;
};

export type DialogBrief = {
  title?: string;
  text?: string;
  options?: { text?: string; disabled?: boolean }[];
};

export type SessionView = {
  id: string;
  dir: string;
  since: string;
  title: string;
  status: string;
} | null;

export type TokenCount = {
  input: number;
  output: number;
  cached: number | null;
  cacheWrite: number | null;
  reasoning: number | null;
  total: number;
};

export type RunView = {
  runId: string;
  status: "running" | "completed" | "failed" | "cancelled" | "timed_out";
  adapter: string;
  target: string;
  /** Model the harness reported. Empty until an init or response event names one. */
  model: string;
  /** Model asked for in the run plan. May be a sentinel such as auto or configured. */
  requestedModel: string;
  /** Reasoning effort from the plan or a harness event. Empty means the harness did not set one. */
  effort: string;
  usage: TokenCount | null;
};

export type RunPatch = {
  runId?: string;
  status?: RunView["status"];
  adapter?: string;
  target?: string;
  model?: string;
  requestedModel?: string;
  effort?: string;
};

export type ActionView = { kind: "tool" | "say"; text: string; time: string };
export type CommandView = { time: string; command: string; ok: boolean; error: string; reason: string };
export type ThoughtView = { text: string; done: boolean };

export type Snapshot = {
  game: GameView;
  session: SessionView;
  run: RunView | null;
  /** Token totals from earlier segments of this session, excluding the open run. */
  earlier: TokenCount | null;
  prompt: string;
  thoughts: ThoughtView[];
  actions: ActionView[];
  commands: CommandView[];
};

export type Piece =
  | { event: "thought"; data: { text: string; done: boolean } }
  | { event: "action"; data: ActionView }
  | { event: "prompt"; data: { text: string } }
  | { event: "run"; data: RunPatch }
  | { event: "usage"; data: TokenCount }
  | { event: "command"; data: CommandView };
