import { readManifest } from '../project.ts';
import { note } from '../ui/output.ts';
import { discover } from './discover.ts';
import type { Pending } from './discover.ts';
import { runLifecycle } from './run.ts';

export function reportPending(pending: Pending[]): void {
  if (pending.length === 0) return;
  const names = pending.map(pkg => pkg.name).join(', ');
  note(`lifecycle scripts not run for ${names} — run \`ant trust <name>\` or \`ant trust --all\``, 'warn');
}

export async function afterInstall(root: string, verbose: boolean): Promise<number> {
  const pending = discover(root);
  if (pending.length === 0) return 0;
  const trusted = new Set(readManifest(root)?.trustedDependencies ?? []);
  const { built, failed } = await runLifecycle(root, pending.filter(pkg => trusted.has(pkg.name)), verbose);
  if (built.length > 0) note(`ran lifecycle scripts for ${built.join(', ')}`);
  reportPending(pending.filter(pkg => !trusted.has(pkg.name)));
  return failed > 0 ? 1 : 0;
}
