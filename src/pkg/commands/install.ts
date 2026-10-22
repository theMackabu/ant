import { parseArgs } from '../args.ts';
import type { Args } from '../args.ts';
import { addGlobal, removeGlobal } from '../global/packages.ts';
import { pinSpecs } from '../land.ts';
import { projectRoot, upm } from '../session.ts';
import type { Session } from '../session.ts';
import { afterInstall, migrate } from '../steps.ts';
import { usage } from '../ui/output.ts';

export async function install(command: string, args: string[], session: Session): Promise<number> {
  let parsed: Args;
  try {
    parsed = parseArgs(args);
  } catch (error) {
    return usage((error as Error).message);
  }

  if (parsed.global) {
    if (command === 'remove') return await removeGlobal(parsed.specs, parsed.flags, session);
    if (parsed.specs.length === 0) return usage('add -g needs at least one spec');
    return await addGlobal(parsed.sourced, parsed.flags, session);
  }

  await migrate(session);
  const adds = command === 'add' || (command === 'install' && parsed.specs.length > 0);
  const specs = adds ? await pinSpecs(parsed.sourced, projectRoot(session)) : parsed.specs;
  const code = await upm([command, ...specs, ...parsed.flags], session);
  return code !== 0 ? code : await afterInstall(session, parsed.flags);
}
