import { networkInterfaces } from "node:os";

/** IPv4 addresses of this machine that are not the loopback. */
export function lanHosts(): string[] {
  const hosts: string[] = [];
  for (const list of Object.values(networkInterfaces())) {
    for (const item of list ?? []) {
      if (item.internal || item.family !== "IPv4") continue;
      hosts.push(item.address);
    }
  }
  return hosts;
}

/** One log line per address. A wildcard bind lists the LAN addresses; the token is never included. */
export function listenLines(host: string, port: number): string[] {
  const line = (address: string) => `civ2agent dashboard http://${address}:${port}  mcp http://${address}:${port}/mcp`;
  if (host === "0.0.0.0" || host === "::") return [line("127.0.0.1"), ...lanHosts().map(line)];
  return [line(host)];
}
