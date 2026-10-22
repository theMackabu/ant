import { LOCKFILE } from 'upm/src/lock.ts';
import { findRoot } from 'upm/src/workspaces.ts';
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

export async function findProjectDir(dir: string | undefined, cwd = process.cwd()): Promise<string | undefined> {
  if (dir !== undefined) return path.resolve(cwd, dir);
  const root = await findRoot(cwd);
  return root.manifest ? root.dir : undefined;
}
