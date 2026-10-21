import fs from 'node:fs';
import path from 'node:path';

import { positionals, splitKey } from '../args.ts';
import { GROUPS, readLock, readManifest } from '../project.ts';
import { projectRoot } from '../session.ts';
import type { Session } from '../session.ts';
import { color, fail, note, out, usage } from '../ui/output.ts';

interface Dependent {
  name: string;
  version: string;
  kind: string;
}

function dependentsOf(cwd: string, name: string, version: string): Dependent[] {
  const lock = readLock(cwd)!;
  const found: Dependent[] = [];
  for (const [key, entry] of Object.entries(lock.packages)) {
    const kinds: [Record<string, string> | undefined, string][] = [
      [entry.dependencies, ''],
      [entry.optionalDependencies, 'optional'],
    ];
    for (const [deps, kind] of kinds) {
      if (deps?.[name] !== version) continue;
      found.push({ ...splitKey(key), kind: entry.peers?.[name] ? 'peer' : kind });
    }
  }
  return found;
}

function installedRange(cwd: string, parent: Dependent, name: string): string {
  const store = path.join(cwd, 'node_modules', '.ant');
  let entries: string[] = [];
  try {
    entries = fs.readdirSync(store);
  } catch {}
  const prefix = `${parent.name.replace('/', '+')}@${parent.version}-`;
  const entry = entries.find(dir => dir.startsWith(prefix));
  const manifest = entry ? readManifest(path.join(store, entry, 'node_modules', parent.name)) : undefined;
  return manifest?.dependencies?.[name] ?? manifest?.optionalDependencies?.[name] ?? manifest?.peerDependencies?.[name] ?? '';
}

export function why(args: string[], session: Session): number {
  const [name] = positionals(args);
  if (!name) return usage('why needs a package name');
  const cwd = projectRoot(session);
  const lock = readLock(cwd);
  if (!lock) {
    fail('no ant.lock here: run `ant install` first');
    return 1;
  }

  const versions = Object.keys(lock.packages).map(splitKey).filter(key => key.name === name).map(key => key.version);
  if (versions.length === 0) {
    note(`${name} is not installed`);
    return 0;
  }

  const manifest = readManifest(cwd) ?? {};
  const group = GROUPS.find(g => manifest[g]?.[name]);
  for (const version of versions) {
    out(`${color('cyan', name)}@${version}\n`);
    const lines: string[] = [];
    if (lock.root.dependencies[name] === version) lines.push(`package.json ${color('gray', group ?? 'dependencies')}`);
    for (const parent of dependentsOf(cwd, name, version)) {
      const kind = parent.kind ? `${color('gray', parent.kind)} ` : '';
      const range = installedRange(cwd, parent, name);
      lines.push(`${kind}${parent.name}@${parent.version}${range ? ` ${color('gray', range)}` : ''}`);
    }
    if (lines.length === 0) lines.push(color('gray', 'nothing depends on it'));
    for (const line of lines) out(`  ${line}\n`);
  }
  return 0;
}
