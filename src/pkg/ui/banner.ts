import { setScriptBanner } from 'upm/src/cli.ts';
import type { ScriptStart } from 'upm/src/api.ts';

import { color, write } from './output.ts';

export function printBanner({ line, workspace }: ScriptStart): void {
  const at = workspace === undefined ? '' : `${workspace}: `;
  write('stderr', `${color('magenta', '$', 'stderr')} ${at}${color('bold', line, 'stderr')}\n`);
}

export function installRunBanner(): void {
  setScriptBanner(printBanner);
}
