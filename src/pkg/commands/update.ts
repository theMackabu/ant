import { LOCKFILE } from 'upm/src/lock.ts';
import { TREE_LOCK } from 'upm/src/state.ts';

import fs from 'node:fs';
import path from 'node:path';

import { nameOfSpec, parseArgs } from '../args.ts';
import type { Args, SourcedSpec } from '../args.ts';
import { pinSpecs } from '../land.ts';
import { GROUPS, readManifest } from '../project.ts';
import { sourceOfTarball } from '../registry.ts';
import { projectRoot, upm } from '../session.ts';
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
  
  await migrate(session);
  if (parsed.specs.length === 0) return await refreshAll(session.dir, parsed.flags, session);

  const manifest = session.dir ? readManifest(session.dir) ?? {} : {};
  const groups = new Map<Group, SourcedSpec[]>();
  for (const { spec, source } of parsed.sourced) {
    const name = nameOfSpec(spec);
    const group = GROUPS.find(g => manifest[g]?.[name] !== undefined);
    
    if (!group) {
      fail(`${name} is not a dependency in package.json`);
      return 1;
    }
    
    const declared = String(manifest[group]![name]);
    const pinned = spec === name ? sourceOfTarball(declared) : undefined;
    const wanted = spec !== name ? spec : pinned ? name : `${name}@${declared}`;
    
    groups.set(group, [...(groups.get(group) ?? []), { spec: wanted, source: source ?? pinned }]);
  }

  for (const [group, sourced] of groups) {
    const specs = await pinSpecs(sourced, projectRoot(session));
    const code = await upm(['add', ...GROUP_FLAGS[group], ...specs, ...parsed.flags], session);
    if (code !== 0) return code;
  }
  return await afterInstall(session, parsed.flags);
}
