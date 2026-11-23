import fs from 'node:fs';
import path from 'node:path';

import { cachePath } from './paths.ts';

const NODE_GYP = 'node-gyp@12.2.0';

function toolDir(name: string): string {
  const dir = cachePath('tools', name);
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}

function writeTool(dir: string, name: string, posix: string, cmd: string): void {
  const win = process.platform === 'win32';
  const file = path.join(dir, win ? `${name}.cmd` : name);
  const text = win ? cmd : posix;
  try {
    if (fs.readFileSync(file, 'utf8') === text) return;
  } catch {}

  const temp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(temp, text, { mode: 0o755 });
  if (!win) fs.chmodSync(temp, 0o755);
  fs.renameSync(temp, file);
}

function shim(name: string, command: string): string {
  const dir = toolDir(`${name}-shim`);
  writeTool(dir, name, `#!/bin/sh\nexec ${command} "$@"\n`, `@${command} %*\r\n`);
  return dir;
}

export const nodeShim = () => shim('node', `"${process.execPath}"`);
export const nodeGypShim = () => shim('node-gyp', `"${process.execPath}" x -p ${NODE_GYP} node-gyp`);
