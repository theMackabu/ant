import { readManifest } from '../project.ts';
import { note } from '../ui/output.ts';
import { discover } from './discover.ts';
import { runLifecycle } from './run.ts';

export async function runInstallScripts(root: string, verbose: boolean): Promise<number> {
  const pending = discover(root);
  if (pending.length === 0) return 0;
  const trusted = new Set(readManifest(root)?.trustedDependencies ?? []);
  const { built, failed } = await runLifecycle(root, pending.filter(pkg => trusted.has(pkg.name)), verbose);
  if (built.length > 0) note(`ran lifecycle scripts for ${built.join(', ')}`);
  const untrusted = [...new Set(pending.filter(pkg => !trusted.has(pkg.name)).map(pkg => pkg.name))];
  if (untrusted.length > 0) {
    note(`lifecycle scripts not run for ${untrusted.join(', ')} — run \`ant trust <name>\` or \`ant trust --all\``, 'warn');
  }
  return failed > 0 ? 1 : 0;
}
