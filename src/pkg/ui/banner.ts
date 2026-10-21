import { setScriptBanner } from 'upm/src/cli.ts';

import { c } from './ansi.ts';
import { out } from './output.ts';

export function installRunBanner(): void {
  setScriptBanner(({ line, workspace }) => {
    const at = workspace === undefined ? '' : `${workspace}: `;
    out(`${c.magenta}$${c.reset} ${at}${c.bold}${line}${c.reset}\n`);
  });
}
