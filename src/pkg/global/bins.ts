import fs from 'node:fs';
import path from 'node:path';

import { globalDir } from '../paths.ts';
import { readJson } from '../project.ts';
import type { Manifest } from '../project.ts';
import { note } from '../ui/output.ts';

export function binsOf(pkgDir: string, name: string): Record<string, string> {
  const manifest = readJson<Manifest & { bin?: string | Record<string, string> }>(path.join(pkgDir, 'package.json'));
  const bin = manifest?.bin;
  if (!bin) return {};
  if (typeof bin === 'string') return { [name.slice(name.lastIndexOf('/') + 1)]: bin };
  return bin;
}

export function ownsLink(link: string, dir: string): boolean {
  try {
    if (!fs.lstatSync(link).isSymbolicLink()) return false;
    return path.resolve(path.dirname(link), fs.readlinkSync(link)).startsWith(`${dir}${path.sep}`);
  } catch {
    return false;
  }
}

function isDanglingLink(file: string): boolean {
  try {
    return fs.lstatSync(file).isSymbolicLink();
  } catch {
    return false;
  }
}

export function linkBin(target: string, link: string, dir: string): boolean {
  if (fs.existsSync(link) || isDanglingLink(link)) {
    if (!ownsLink(link, dir)) {
      note(`${link} exists and was not linked by ant; left as is`, 'warn');
      return false;
    }
    fs.unlinkSync(link);
  }
  if (process.platform === 'win32') {
    fs.writeFileSync(`${link}.cmd`, `@"${process.execPath}" "${target}" %*\r\n`);
    return true;
  }
  fs.symlinkSync(target, link);
  return true;
}

export function globalBin(command: string): string | undefined {
  const file = path.join(globalDir(), 'node_modules', '.bin', command);
  return fs.existsSync(file) ? file : undefined;
}
