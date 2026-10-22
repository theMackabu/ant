import { LOCKFILE } from 'upm/src/lock.ts';
import { TREE_LOCK } from 'upm/src/state.ts';

import fs from 'node:fs';
import path from 'node:path';

import { nameOfSpec, parseArgs } from '../args.ts';
import type { Args } from '../args.ts';
import { GROUPS, readManifest } from '../project.ts';
import { useSource } from '../registry.ts';
import { upm } from '../session.ts';
import type { Session } from '../session.ts';
import { afterInstall, migrate } from '../steps.ts';
import { fail, usage } from '../ui/output.ts';

type Group = (typeof GROUPS)[number];

const GROUP_FLAGS: Record<Group, string[]> = {
  dependencies: [],
  devDependencies: ['--dev'],
  optionalDependencies: ['--optional'],
};

async function refreshAll(root: string | undefined, flags: string[], session: Session): Promise<number> {
  if (!root) {
    const code = await upm(['install', ...flags], session);
    return code !== 0 ? code : await afterInstall(session, flags);
  }

  const lockFile = path.join(root, LOCKFILE);
  const saved = fs.existsSync(lockFile) ? fs.readFileSync(lockFile) : undefined;
  fs.rmSync(lockFile, { force: true });
  fs.rmSync(path.join(root, 'node_modules', TREE_LOCK), { force: true });

  const code = await upm(['install', ...flags], session);
  const written = fs.existsSync(lockFile);
  if (code === 0 && written) return await afterInstall(session, flags);
  if (saved && !written) fs.writeFileSync(lockFile, saved);
  if (code !== 0) return code;
  fail(`${LOCKFILE} was not rewritten: another package manager's lockfile was read instead`);
  return 1;
}

export async function update(args: string[], session: Session): Promise<number> {
  let parsed: Args;
  try {
    parsed = parseArgs(args);
  } catch (error) {
    return usage((error as Error).message);
  }
  if (parsed.source) useSource(parsed.source);
  await migrate(session);

  if (parsed.specs.length === 0) return await refreshAll(session.dir, parsed.flags, session);

  const manifest = session.dir ? readManifest(session.dir) ?? {} : {};
  const groups = new Map<Group, string[]>();
  for (const spec of parsed.specs) {
    const name = nameOfSpec(spec);
    const group = GROUPS.find(g => manifest[g]?.[name] !== undefined);
    if (!group) {
      fail(`${name} is not a dependency in package.json`);
      return 1;
    }
    const wanted = spec === name ? `${name}@${manifest[group]![name]}` : spec;
    groups.set(group, [...(groups.get(group) ?? []), wanted]);
  }

  for (const [group, specs] of groups) {
    const code = await upm(['add', ...GROUP_FLAGS[group], ...specs, ...parsed.flags], session);
    if (code !== 0) return code;
  }
  return await afterInstall(session, parsed.flags);
}
