import { PeerClient, type Inventory } from "./peer.js";

export type PeerSelector = string | string[] | "auto";

export interface ResolvedPeer {
  client: PeerClient;
  inventory?: Inventory;
}

export interface FanOutResult<T> {
  peer: string;
  ok: boolean;
  value?: T;
  error?: string;
}

export function isPeerSelector(value: unknown): value is PeerSelector {
  if (value === "auto") return true;
  if (typeof value === "string") return value.length > 0;
  if (Array.isArray(value)) {
    return value.every(
      (v) => typeof v === "string" && v.length > 0,
    );
  }
  return false;
}

function normalizeSelector(selector: PeerSelector): string[] {
  if (selector === "auto") return ["auto"];
  if (typeof selector === "string") return [selector];
  return selector;
}

export async function resolvePeers(
  selector: PeerSelector,
  clients: PeerClient[],
): Promise<ResolvedPeer[]> {
  const wanted = normalizeSelector(selector);
  if (wanted.length === 1 && wanted[0] === "auto") {
    const sampled = await Promise.all(
      clients.map(async (client) => {
        try {
          const inventory = await client.inventory();
          return { client, inventory };
        } catch {
          return { client, inventory: undefined };
        }
      }),
    );
    const reachable = sampled.filter((s) => s.inventory);
    if (reachable.length === 0) {
      throw new Error("no reachable peers");
    }
    reachable.sort((a, b) => {
      const ai = a.inventory!;
      const bi = b.inventory!;
      const scoreA =
        ai.runningJobs * 1000 +
        ai.queuedJobs * 100 +
        ai.cpuLoadPct;
      const scoreB =
        bi.runningJobs * 1000 +
        bi.queuedJobs * 100 +
        bi.cpuLoadPct;
      return scoreA - scoreB;
    });
    return [reachable[0]];
  }

  const byName = new Map<string, PeerClient>(
    clients.map((c) => [c.name.toLowerCase(), c]),
  );
  const resolved: ResolvedPeer[] = [];
  for (const name of wanted) {
    const client = byName.get(name.toLowerCase());
    if (!client) {
      throw new Error(`unknown peer: ${name}`);
    }
    resolved.push({ client });
  }
  return resolved;
}

export async function fanOut<T>(
  clients: PeerClient[],
  fn: (client: PeerClient) => Promise<T>,
): Promise<FanOutResult<T>[]> {
  const results = await Promise.all(
    clients.map(async (client) => {
      try {
        const value = await fn(client);
        return { peer: client.name, ok: true, value } as FanOutResult<T>;
      } catch (e) {
        const message = e instanceof Error ? e.message : String(e);
        return { peer: client.name, ok: false, error: message } as FanOutResult<T>;
      }
    }),
  );
  return results;
}
