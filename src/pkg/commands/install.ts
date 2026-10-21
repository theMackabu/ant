import { flagsOf, isGlobal, withoutGlobal } from '../args.ts';
import { addGlobal, removeGlobal } from '../global/packages.ts';
import { landCompatibility } from '../land.ts';
import { applyRegistry } from '../registry.ts';
import { finish, migrate, upm } from '../session.ts';
import type { Session } from '../session.ts';
import { stripSourcePrefixes } from '../sources.ts';
import { usage } from '../ui/output.ts';

export async function install(command: string, args: string[], session: Session): Promise<number> {
  let prefixed;
  try {
    prefixed = stripSourcePrefixes(withoutGlobal(args));
  } catch (error) {
    return usage((error as Error).message);
  }
  const { args: rest, specs, source } = prefixed;
  if (source) applyRegistry(session.settings, source);

  if (isGlobal(args)) {
    if (command === 'remove') return await removeGlobal(specs, flagsOf(rest), session);
    if (specs.length === 0) return usage('add -g needs at least one spec');
    return await addGlobal(specs, flagsOf(rest), session);
  }

  await migrate(session);
  const code = await upm([command, ...rest], session);
  if (code !== 0) return code;

  const adds = command === 'add' || (command === 'install' && specs.length > 0);
  const land = (source ?? session.settings.defaultRegistry) === 'land';
  if (adds && land && session.dir && landCompatibility(session.dir, specs)) {
    const again = await upm(['install', '--silent'], session);
    if (again !== 0) return again;
  }
  return await finish(code, session);
}
