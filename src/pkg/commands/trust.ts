import path from 'node:path';

import { parseArgs } from '../args.ts';
import { discover, scriptLine, trustLabel } from '../lifecycle/discover.ts';
import { runLifecycle } from '../lifecycle/run.ts';
import { readManifest, writeJson } from '../project.ts';
import { projectRoot } from '../session.ts';
import type { Session } from '../session.ts';
import { duration, list, note, out, plural, summary } from '../ui/output.ts';

export async function trust(args: string[], session: Session): Promise<number> {
  const { specs, all } = parseArgs(args);
  const names = specs.map(({ spec }) => spec);
  const root = projectRoot(session);
  const start = performance.now();
  const pending = discover(root);
  if (pending.length === 0) {
    note('no package has lifecycle scripts waiting to run');
    return 0;
  }

  if (!all && names.length === 0) {
    out(list(pending.map(pkg => [trustLabel(pkg), scriptLine(pkg)])));
    note('run `ant trust <name>` or `ant trust --all` to run them');
    return 0;
  }

  const wanted = new Set(names);
  const chosen = all ? pending : pending.filter(pkg => wanted.has(pkg.trust));
  const found = new Set(chosen.map(pkg => pkg.trust));
  for (const name of names) {
    if (!found.has(name)) note(`${name} has no lifecycle scripts waiting to run`, 'warn');
  }
  if (chosen.length === 0) return 0;

  const manifest = readManifest(root);
  if (manifest) {
    const trusted = new Set(manifest.trustedDependencies ?? []);
    const added = [...found].filter(name => !trusted.has(name));
    if (added.length > 0) {
      manifest.trustedDependencies = [...trusted, ...added];
      writeJson(path.join(root, 'package.json'), manifest);
      note(`+ ${added.join(', ')} in trustedDependencies`);
    }
  }

  const { built, failed } = await runLifecycle(root, chosen, session.verbose);
  if (built.length > 0) summary('trusted', [plural(built.length, 'pkg'), duration(performance.now() - start)]);
  return failed > 0 ? 1 : 0;
}
