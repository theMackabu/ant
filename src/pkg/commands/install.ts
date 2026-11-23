import { parseArgs } from '../args.ts';
import { addGlobal, removeGlobal } from '../global/packages.ts';
import { pinSpecs } from '../land.ts';
import { afterInstall } from '../lifecycle/report.ts';
import { migrateLockb } from '../lockb/migrate.ts';
import { projectRoot, upm } from '../session.ts';
import type { Session } from '../session.ts';
import { usage } from '../ui/output.ts';

export async function install(command: string, args: string[], session: Session): Promise<number> {
  const { specs, flags, global } = parseArgs(args);
  const names = specs.map(({ spec }) => spec);

  if (global) {
    if (command === 'remove') return await removeGlobal(names, flags, session);
    if (specs.length === 0) return usage('add -g needs at least one spec');
    return await addGlobal(specs, flags, session);
  }

  await migrateLockb(session);
  const adds = command === 'add' || (command === 'install' && specs.length > 0);
  const wanted = adds ? await pinSpecs(specs, projectRoot(session)) : names;
  const code = await upm([command, ...wanted, ...flags], session);
  return code !== 0 ? code : await afterInstall(session, flags);
}
