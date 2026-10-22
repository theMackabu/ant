import { normalizeBin } from 'upm/src/normalize-bin.ts';

import fs from 'node:fs';
import path from 'node:path';

import { globalDir } from '../paths.ts';
import { readJson } from '../project.ts';
import { note } from '../ui/output.ts';

const WIN = process.platform === 'win32';

export function binsOf(pkgDir: string, name: string): Record<string, string> {
  const manifest = readJson<{ name?: string; bin?: unknown }>(path.join(pkgDir, 'package.json'));
  return manifest ? normalizeBin({ name: manifest.name ?? name, bin: manifest.bin }) : {};
}

function pointsInto(link: string, dir: string): boolean {
  try {
    if (WIN) return fs.readFileSync(`${link}.cmd`, 'utf8').includes(`${dir}${path.sep}`);
    if (!fs.lstatSync(link).isSymbolicLink()) return false;
    return path.resolve(path.dirname(link), fs.readlinkSync(link)).startsWith(`${dir}${path.sep}`);
  } catch {
    return false;
  }
}

function occupied(link: string): boolean {
  try {
    fs.lstatSync(WIN ? `${link}.cmd` : link);
    return true;
  } catch {
    return false;
  }
}

export function linkBin(target: string, link: string, dir: string): boolean {
  if (occupied(link)) {
    if (!pointsInto(link, dir)) {
      note(`${link} exists and was not linked by ant; left as is`, 'warn');
      return false;
    }
    fs.rmSync(WIN ? `${link}.cmd` : link, { force: true });
  }
  if (WIN) fs.writeFileSync(`${link}.cmd`, `@"${process.execPath}" "${target}" %*\r\n`);
  else fs.symlinkSync(target, link);
  return true;
}

export function unlinkBin(link: string, dir: string): boolean {
  if (!pointsInto(link, dir)) return false;
  fs.rmSync(WIN ? `${link}.cmd` : link, { force: true });
  return true;
}

export function globalBin(command: string): string | undefined {
  const file = path.join(globalDir(), 'node_modules', '.bin', WIN ? `${command}.cmd` : command);
  return fs.existsSync(file) ? file : undefined;
}
