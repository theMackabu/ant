import { setHome } from 'upm/src/util.ts';

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

function ownDir(): string | undefined {
  const own = path.join(os.homedir(), '.ant');
  return process.platform === 'win32' || fs.existsSync(own) ? own : undefined;
}

function antDir(kind: 'data' | 'cache'): string {
  const own = ownDir();
  if (own) return own;
  const set = kind === 'data' ? process.env.XDG_DATA_HOME : process.env.XDG_CACHE_HOME;
  if (set && path.isAbsolute(set)) return path.join(set, 'ant');
  const home = os.homedir();
  return kind === 'data' ? path.join(home, '.local', 'share', 'ant') : path.join(home, '.cache', 'ant');
}

export function useAntHome(): void {
  setHome(antDir);
}

export const dataPath = (...parts: string[]) => path.join(antDir('data'), ...parts);
export const cachePath = (...parts: string[]) => path.join(antDir('cache'), ...parts);
export const configPath = () => dataPath('pkg', 'config');
export const globalDir = () => dataPath('pkg', 'global');
export const binDir = () => path.join(ownDir() ?? path.join(os.homedir(), '.local'), 'bin');
