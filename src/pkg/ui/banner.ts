import { setScriptBanner } from 'upm/src/cli.ts';
import type { ScriptStart } from 'upm/src/api.ts';

import { c } from './ansi.ts';
import { out } from './output.ts';

export function printBanner({ line, workspace }: ScriptStart): void {
  const at = workspace === undefined ? '' : `${workspace}: `;
  out(`${c.magenta}$${c.reset} ${at}${c.bold}${line}${c.reset}\n`);
}

export function installRunBanner(): void {
  setScriptBanner(printBanner);
}
