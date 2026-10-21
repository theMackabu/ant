import { LOCKFILE } from 'upm/src/lock.ts';
import type { Lockfile } from 'upm/src/lock.ts';

import fs from 'node:fs';
import path from 'node:path';

export type Manifest = Record<string, unknown> & {
  name?: string;
  version?: string;
  scripts?: Record<string, string>;
  dependencies?: Record<string, string>;
  devDependencies?: Record<string, string>;
  optionalDependencies?: Record<string, string>;
  peerDependencies?: Record<string, string>;
  trustedDependencies?: string[];
};

export const GROUPS = ['dependencies', 'devDependencies', 'optionalDependencies'] as const;

export function readJson<T>(file: string): T | undefined {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8')) as T;
  } catch {
    return undefined;
  }
}

export function writeJson(file: string, value: unknown): void {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`);
}

export function readManifest(dir: string): Manifest | undefined {
  return readJson<Manifest>(path.join(dir, 'package.json'));
}

export function readLock(dir: string): Lockfile | undefined {
  return readJson<Lockfile>(path.join(dir, LOCKFILE));
}

export function findProjectDir(argv: string[], cwd = process.cwd()): string | undefined {
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i]!;
    if (arg === '--dir' && argv[i + 1]) return path.resolve(cwd, argv[i + 1]!);
    if (arg.startsWith('--dir=')) return path.resolve(cwd, arg.slice('--dir='.length));
  }
  for (let dir = cwd; ; ) {
    if (fs.existsSync(path.join(dir, 'package.json'))) return dir;
    const up = path.dirname(dir);
    if (up === dir) return undefined;
    dir = up;
  }
}
