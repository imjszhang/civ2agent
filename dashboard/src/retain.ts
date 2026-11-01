import type { ActionView, CommandView, ThoughtView } from "./types.js";

export const KEEP_COMMANDS = 80;
export const KEEP_ACTIONS = 80;
export const KEEP_THOUGHTS = 40;
export const THOUGHT_CHARS = 4000;

/** Drop the oldest items. The live page only shows the tail; the files keep the rest. */
export function retainTail<T>(items: T[], limit: number): void {
  if (items.length > limit) items.splice(0, items.length - limit);
}

export function retainThoughtText(text: string): string {
  return text.length > THOUGHT_CHARS ? text.slice(-THOUGHT_CHARS) : text;
}

export function retainThoughts(thoughts: ThoughtView[]): void {
  const last = thoughts.at(-1);
  if (last) last.text = retainThoughtText(last.text);
  retainTail(thoughts, KEEP_THOUGHTS);
}

export function retainActions(actions: ActionView[]): void {
  retainTail(actions, KEEP_ACTIONS);
}

export function retainCommands(commands: CommandView[]): void {
  retainTail(commands, KEEP_COMMANDS);
}
