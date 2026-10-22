import type { Lockfile } from 'upm/src/lock.ts';
import { ENTRIES_DIR } from 'upm/src/state.ts';

import fs from 'node:fs';
import path from 'node:path';

import { splitKey } from './args.ts';

const HASH_LENGTH = 22;

export interface StoreEntry {
  key: string;
  name: string;
  version: string;
  modules: string;
  dir: string;
}

export function parseEntryKey(key: string): { name: string; version: string } | undefined {
  if (key.length <= HASH_LENGTH + 1 || key[key.length - HASH_LENGTH - 1] !== '-') return undefined;
  const id = key.slice(0, key.length - HASH_LENGTH - 1);
  const at = id.indexOf('@', 1);
  if (at < 0) return undefined;
  return { name: id.slice(0, at).replaceAll('+', '/'), version: id.slice(at + 1) };
}

export function storeEntries(root: string): Map<string, StoreEntry[]> {
  const store = path.join(root, 'node_modules', ENTRIES_DIR);
  const out = new Map<string, StoreEntry[]>();
  let keys: string[];
  try {
    keys = fs.readdirSync(store);
  } catch {
    return out;
  }
  for (const key of keys) {
    const parsed = parseEntryKey(key);
    if (!parsed) continue;
    const modules = path.join(store, key, 'node_modules');
    const entry = { key, ...parsed, modules, dir: path.join(modules, parsed.name) };
    const id = `${parsed.name}@${parsed.version}`;
    out.set(id, [...(out.get(id) ?? []), entry]);
  }
  return out;
}

export function lockedEntry(lock: Lockfile, key: string): { name: string; version: string } {
  const { name, version } = splitKey(key);
  return { name, version: lock.packages[key]?.version ?? version };
}
