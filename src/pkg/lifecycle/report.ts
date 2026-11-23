import { readManifest } from '../project.ts';
import type { Session } from '../session.ts';
import { note } from '../ui/output.ts';
import { discover, trustLabel } from './discover.ts';
import { runLifecycle } from './run.ts';

export async function afterInstall(session: Session, flags: string[]): Promise<number> {
  const root = session.dir;
  if (!root || flags.includes('--ignore-scripts')) return 0;
  const pending = discover(root);
  if (pending.length === 0) return 0;

  const trusted = new Set(readManifest(root)?.trustedDependencies ?? []);
  const { built, failed } = await runLifecycle(root, pending.filter(pkg => trusted.has(pkg.trust)), session.verbose);
  if (built.length > 0) note(`ran lifecycle scripts for ${built.join(', ')}`);

  const untrusted = [...new Set(pending.filter(pkg => !trusted.has(pkg.trust)).map(trustLabel))];
  if (untrusted.length > 0) {
    note(`lifecycle scripts not run for ${untrusted.join(', ')} — run \`ant trust <name>\` or \`ant trust --all\``, 'warn');
  }
  return failed > 0 ? 1 : 0;
}
