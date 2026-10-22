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
  if (process.platform === 'win32') {
    fs.writeFileSync(path.join(dir, `${name}.cmd`), cmd);
    return;
  }
  const file = path.join(dir, name);
  fs.writeFileSync(file, posix, { mode: 0o755 });
  fs.chmodSync(file, 0o755);
}

export function nodeShim(): string {
  const dir = toolDir('node-shim');
  const ant = process.execPath;
  writeTool(dir, 'node', `#!/bin/sh\nexec "${ant}" "$@"\n`, `@"${ant}" %*\r\n`);
  return dir;
}

export function nodeGypShim(): string {
  const dir = toolDir('node-gyp-shim');
  const ant = process.execPath;
  writeTool(
    dir,
    'node-gyp',
    `#!/bin/sh\nexec "${ant}" x -p ${NODE_GYP} node-gyp "$@"\n`,
    `@"${ant}" x -p ${NODE_GYP} node-gyp %*\r\n`,
  );
  return dir;
}
