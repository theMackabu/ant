import { describe } from 'upm/src/util.ts';

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { nameOfSpec, parseSource } from '../args.ts';
import { globalBin } from '../global/bins.ts';
import { useSource } from '../registry.ts';
import { upm } from '../session.ts';
import type { Session } from '../session.ts';
import { nodeShim } from '../tools.ts';
import { fail, usage } from '../ui/output.ts';

function runGlobal(bin: string, args: string[]): number {
  const ran = spawnSync(bin, args, { stdio: 'inherit', shell: process.platform === 'win32' });
  if (ran.error) return fail(describe(ran.error));
  if (ran.signal) return 128 + (os.constants.signals[ran.signal] ?? 0);
  return ran.status ?? 1;
}

export async function exec(args: string[], session: Session): Promise<number> {
  let rest = args;
  if (rest[0] === '--ant') {
    rest = rest.slice(1);
    if (rest.length === 0) return usage('--ant needs a command');
    process.env.PATH = `${nodeShim()}${path.delimiter}${process.env.PATH ?? ''}`;
  }

  const [command, ...commandArgs] = rest;
  if (command && !command.startsWith('-')) {
    const { spec, source } = parseSource(command);
    if (source) useSource(source, session.settings);

    const name = nameOfSpec(spec);
    const local = session.dir && fs.existsSync(path.join(session.dir, 'node_modules', '.bin', name));
    const global = spec !== name || local ? undefined : globalBin(name);
    if (global) return runGlobal(global, commandArgs);
    rest = [spec, ...commandArgs];
  }

  return await upm(['exec', ...rest], session);
}
