import { STATE_FILE } from 'upm/src/state.ts';

import fs from 'node:fs';
import path from 'node:path';

import type { Lockfile } from 'upm/src/lock.ts';

import { lockedEntry, storeEntries } from '../entries.ts';
import { readJson, readLock, writeJson } from '../project.ts';
import type { Manifest } from '../project.ts';

export const MARKER = '.postinstall';
const MARKER_HEADER = 'ant lifecycle v1';
const CACHE = '.ant-lifecycle.json';
const CACHE_VERSION = 2;
const SCRIPTS = ['preinstall', 'install', 'postinstall'] as const;
const SKIPPED = new Set(['esbuild']);

export interface Pending {
  name: string;
  version: string;
  dir: string;
  modules: string;
  scripts: [string, string][];
}

interface Cached {
  version: number;
  hash: string;
  root: string;
  candidates: Pending[];
}

export function buildScripts(manifest: Manifest, hasGyp: () => boolean): [string, string][] {
  const scripts = manifest.scripts ?? {};
  const found = SCRIPTS.filter(name => scripts[name]).map(name => [name, scripts[name]!] as [string, string]);
  if (!scripts.install && !scripts.preinstall && hasGyp()) {
    const at = found.findIndex(([name]) => name === 'postinstall');
    found.splice(at < 0 ? found.length : at, 0, ['install', 'node-gyp rebuild']);
  }
  return found;
}

export function markerText(scripts: [string, string][]): string {
  return [MARKER_HEADER, ...scripts.map(([name]) => name)].join('\n') + '\n';
}

function ran(pkg: Pending): boolean {
  try {
    return fs.readFileSync(path.join(pkg.dir, MARKER), 'utf8') === markerText(pkg.scripts);
  } catch {
    return false;
  }
}

function installOrder(lock: Lockfile): string[] {
  const order: string[] = [];
  const seen = new Set<string>();
  const visit = (key: string) => {
    if (seen.has(key)) return;
    seen.add(key);
    const entry = lock.packages[key];
    for (const [dep, version] of Object.entries({ ...entry?.dependencies, ...entry?.optionalDependencies })) {
      visit(`${dep}@${version}`);
    }
    order.push(key);
  };
  for (const [name, version] of Object.entries(lock.root.dependencies).sort()) visit(`${name}@${version}`);
  for (const key of Object.keys(lock.packages).sort()) visit(key);
  return order;
}

function scan(root: string): Pending[] {
  const lock = readLock(root);
  if (!lock) return [];
  const entries = storeEntries(root);
  const candidates: Pending[] = [];
  for (const key of installOrder(lock)) {
    const { name, version } = lockedEntry(lock, key);
    if (SKIPPED.has(name)) continue;
    for (const entry of entries.get(`${name}@${version}`) ?? []) {
      const manifest = readJson<Manifest>(path.join(entry.dir, 'package.json'));
      if (!manifest) continue;
      const scripts = buildScripts(manifest, () => fs.existsSync(path.join(entry.dir, 'binding.gyp')));
      if (scripts.length > 0) candidates.push({ name, version, dir: entry.dir, modules: entry.modules, scripts });
    }
  }
  return candidates;
}

export function discover(root: string): Pending[] {
  const modules = path.join(root, 'node_modules');
  const hash = readJson<{ hash?: string }>(path.join(modules, STATE_FILE))?.hash;
  const cached = readJson<Cached>(path.join(modules, CACHE));
  let candidates = hash && cached?.version === CACHE_VERSION && cached.hash === hash && cached.root === root ? cached.candidates : undefined;
  if (!candidates) {
    candidates = scan(root);
    if (hash) writeJson(path.join(modules, CACHE), { version: CACHE_VERSION, hash, root, candidates } satisfies Cached);
  }
  return candidates.filter(pkg => !ran(pkg));
}

export function scriptLine(pkg: Pending): string {
  return pkg.scripts.map(([name, command]) => `${name}: ${command}`).join(' && ');
}
