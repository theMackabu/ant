import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

function antDir(kind: 'data' | 'cache'): string {
  const home = os.homedir();
  const own = path.join(home, '.ant');
  if (process.platform === 'win32' || fs.existsSync(own)) return own;
  const set = kind === 'data' ? process.env.XDG_DATA_HOME : process.env.XDG_CACHE_HOME;
  if (set && path.isAbsolute(set)) return path.join(set, 'ant');
  return kind === 'data' ? path.join(home, '.local', 'share', 'ant') : path.join(home, '.cache', 'ant');
}

export const dataPath = (...parts: string[]) => path.join(antDir('data'), ...parts);
export const cachePath = (...parts: string[]) => path.join(antDir('cache'), ...parts);
export const configPath = () => dataPath('pkg', 'config');
export const globalDir = () => dataPath('pkg', 'global');

export function binDir(): string {
  const home = os.homedir();
  const own = path.join(home, '.ant');
  if (process.platform === 'win32' || fs.existsSync(own)) return path.join(own, 'bin');
  return path.join(home, '.local', 'bin');
}
