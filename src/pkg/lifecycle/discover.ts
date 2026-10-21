import fs from 'node:fs';
import path from 'node:path';

import { splitKey } from '../args.ts';
import { readJson, readLock } from '../project.ts';
import type { Manifest } from '../project.ts';

export const MARKER = '.postinstall';
const MARKER_HEADER = 'ant lifecycle v1';
const SCRIPTS = ['preinstall', 'install', 'postinstall'] as const;
const SKIPPED = new Set(['esbuild']);

export interface Pending {
  name: string;
  version: string;
  dir: string;
  scripts: [string, string][];
}

function entryDirs(root: string): Map<string, string> {
  const store = path.join(root, 'node_modules', '.ant');
  const out = new Map<string, string>();
  let entries: string[];
  try {
    entries = fs.readdirSync(store);
  } catch {
    return out;
  }
  for (const entry of entries) {
    if (entry.startsWith('.') || entry === 'node_modules') continue;
    for (let dash = entry.indexOf('-'); dash > 0; dash = entry.indexOf('-', dash + 1)) {
      out.set(entry.slice(0, dash), path.join(store, entry));
    }
  }
  return out;
}

function packageDir(entries: Map<string, string>, name: string, version: string): string | undefined {
  const entry = entries.get(`${name.replace('/', '+')}@${version}`);
  return entry && path.join(entry, 'node_modules', name);
}

function scriptsOf(dir: string, manifest: Manifest): [string, string][] {
  const scripts = manifest.scripts ?? {};
  const found = SCRIPTS.filter(name => scripts[name]).map(name => [name, scripts[name]!] as [string, string]);
  if (!scripts.install && !scripts.preinstall && fs.existsSync(path.join(dir, 'binding.gyp'))) {
    const at = found.findIndex(([name]) => name === 'postinstall');
    found.splice(at < 0 ? found.length : at, 0, ['install', 'node-gyp rebuild']);
  }
  return found;
}

export function markerText(scripts: [string, string][]): string {
  return [MARKER_HEADER, ...scripts.map(([name]) => name)].join('\n') + '\n';
}

function ran(dir: string, scripts: [string, string][]): boolean {
  try {
    return fs.readFileSync(path.join(dir, MARKER), 'utf8') === markerText(scripts);
  } catch {
    return false;
  }
}

export function discover(root: string): Pending[] {
  const lock = readLock(root);
  if (!lock) return [];
  const entries = entryDirs(root);
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

  const pending: Pending[] = [];
  for (const key of order) {
    const { name, version } = splitKey(key);
    if (SKIPPED.has(name)) continue;
    const dir = packageDir(entries, name, version);
    if (!dir) continue;
    const manifest = readJson<Manifest>(path.join(dir, 'package.json'));
    if (!manifest) continue;
    const scripts = scriptsOf(dir, manifest);
    if (scripts.length === 0 || ran(dir, scripts)) continue;
    pending.push({ name, version, dir, scripts });
  }
  return pending;
}

export function scriptLine(pkg: Pending): string {
  return pkg.scripts.map(([name, command]) => `${name}: ${command}`).join(' && ');
}
