import fs from 'node:fs';
import path from 'node:path';

import { positionals, splitKey } from '../args.ts';
import { GROUPS, readLock, readManifest } from '../project.ts';
import { projectRoot } from '../session.ts';
import type { Session } from '../session.ts';
import { c } from '../ui/ansi.ts';
import { out, usage, write } from '../ui/output.ts';

const err = (text: string) => write('stderr', text);

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
    err("Error: No lockfile found. Run 'ant install' first.\n");
    return 1;
  }

  const versions = Object.keys(lock.packages).map(splitKey).filter(key => key.name === name).map(key => key.version);
  if (versions.length === 0) {
    out(`\n${c.bold}${name}${c.reset} is not installed\n\n`);
    return 0;
  }

  const manifest = readManifest(cwd) ?? {};
  const group = GROUPS.find(g => manifest[g]?.[name]);
  for (const version of versions) {
    const label = group === 'devDependencies' ? ' dev' : '';
    out(`\n${c.bold}${name}${c.reset}@${c.dim}${version}${c.reset}${c.yellow}${label}${c.reset}\n`);

    let count = 0;
    if (lock.root.dependencies[name] === version) {
      const type = group === 'devDependencies' ? 'devDependencies' : 'dependencies';
      out(`  ${c.dim}└${c.reset} ${c.green}package.json${c.reset} ${c.dim}(${type})${c.reset}\n`);
      count++;
    }
    for (const parent of dependentsOf(cwd, name, version)) {
      const kind = parent.kind ? `${parent.kind} ` : '';
      const range = installedRange(cwd, parent, name);
      out(`  ${c.dim}└${c.reset} ${kind}${c.bold}${parent.name}${c.reset}@${c.dim}${parent.version}${c.reset} ${c.cyan}"${range}"${c.reset}\n`);
      count++;
    }
    if (count === 0) out(`  ${c.dim}(no dependents)${c.reset}\n`);
  }
  out('\n');
  return 0;
}
