import { runInstallScripts } from './lifecycle/report.ts';
import { migrateLockb } from './lockb/migrate.ts';
import type { Session } from './session.ts';
import { note } from './ui/output.ts';

export async function migrate(session: Session): Promise<void> {
  if (session.dir) await migrateLockb(session.dir, message => note(message));
}

export async function afterInstall(session: Session, flags: string[]): Promise<number> {
  if (!session.dir || flags.includes('--ignore-scripts')) return 0;
  return await runInstallScripts(session.dir, session.verbose);
}
