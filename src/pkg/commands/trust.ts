import path from 'node:path';

import { positionals } from '../args.ts';
import { discover, scriptLine } from '../lifecycle/discover.ts';
import { runLifecycle } from '../lifecycle/run.ts';
import { readManifest, writeJson } from '../project.ts';
import { projectRoot } from '../session.ts';
import type { Session } from '../session.ts';
import { duration, list, note, out, plural, summary } from '../ui/output.ts';

export async function trust(args: string[], session: Session): Promise<number> {
  const root = projectRoot(session);
  const names = positionals(args);
  const all = args.includes('-a') || args.includes('--all');
  const verbose = session.verbose;
  const start = performance.now();
  const pending = discover(root);
  if (pending.length === 0) {
    note('no package has lifecycle scripts waiting to run');
    return 0;
  }

  if (!all && names.length === 0) {
    out(list(pending.map(pkg => [pkg.name, scriptLine(pkg)])));
    note('run `ant trust <name>` or `ant trust --all` to run them');
    return 0;
  }

  const byName = new Map(pending.map(pkg => [pkg.name, pkg]));
  const chosen = all ? pending : [];
  if (!all) for (const name of names) {
    if (byName.has(name)) chosen.push(byName.get(name)!);
    else note(`${name} has no lifecycle scripts waiting to run`, 'warn');
  }
  if (chosen.length === 0) return 0;

  const manifest = readManifest(root);
  if (manifest) {
    const trusted = new Set(manifest.trustedDependencies ?? []);
    const added = chosen.map(pkg => pkg.name).filter(name => !trusted.has(name));
    if (added.length > 0) {
      manifest.trustedDependencies = [...trusted, ...added];
      writeJson(path.join(root, 'package.json'), manifest);
      note(`+ ${added.join(', ')} in trustedDependencies`);
    }
  }

  const { built, failed } = await runLifecycle(root, chosen, verbose);
  if (built.length > 0) summary('trusted', [plural(built.length, 'pkg'), duration(performance.now() - start)]);
  return failed > 0 ? 1 : 0;
}
