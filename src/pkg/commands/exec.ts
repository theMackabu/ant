import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

import { nameOfSpec } from '../args.ts';
import { globalBin } from '../global/bins.ts';
import { cachePath } from '../paths.ts';
import { applyRegistry } from '../registry.ts';
import { upm } from '../session.ts';
import type { Session } from '../session.ts';
import { stripSourcePrefixes } from '../sources.ts';
import { usage } from '../ui/output.ts';

function nodeShim(): string {
  const dir = cachePath('tools', 'node-shim');
  fs.mkdirSync(dir, { recursive: true });
  if (process.platform === 'win32') {
    fs.writeFileSync(path.join(dir, 'node.cmd'), `@"${process.execPath}" %*\r\n`);
    return dir;
  }
  const link = path.join(dir, 'node');
  let current: string | undefined;
  try {
    current = fs.readlinkSync(link);
  } catch {}
  if (current !== process.execPath) {
    fs.rmSync(link, { force: true });
    fs.symlinkSync(process.execPath, link);
  }
  return dir;
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
    let spec = command;
    try {
      const { specs, source } = stripSourcePrefixes([command]);
      spec = specs[0]!;
      if (source) applyRegistry(session.settings, source);
    } catch (error) {
      return usage((error as Error).message);
    }

    const local = session.dir && fs.existsSync(path.join(session.dir, 'node_modules', '.bin', nameOfSpec(spec)));
    const global = local ? undefined : globalBin(nameOfSpec(spec));
    if (global) {
      const ran = spawnSync(global, commandArgs, { stdio: 'inherit' });
      return ran.status ?? (ran.signal ? 128 : 1);
    }
    rest = [spec, ...commandArgs];
  }

  return await upm(['exec', ...rest], session);
}
