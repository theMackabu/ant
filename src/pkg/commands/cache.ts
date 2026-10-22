import { metadataDir } from 'upm/src/metadata.ts';
import { SHARED_DIR } from 'upm/src/state.ts';
import { storeDir } from 'upm/src/store.ts';

import fs from 'node:fs';
import path from 'node:path';

import { upm } from '../session.ts';
import type { Session } from '../session.ts';
import { color, out, size, usage } from '../ui/output.ts';

function walkSize(dir: string): { files: number; bytes: number } {
  let files = 0;
  let bytes = 0;
  const stack = [dir];
  while (stack.length) {
    const current = stack.pop()!;
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(current, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const entry of entries) {
      const full = path.join(current, entry.name);
      if (entry.isDirectory()) stack.push(full);
      else if (entry.isFile()) {
        files++;
        try { bytes += fs.statSync(full).size; } catch {}
      }
    }
  }
  return { files, bytes };
}

function sharedCount(dir: string): number {
  try {
    const entries = fs.readdirSync(dir, { withFileTypes: true });
    return entries.filter(entry => entry.isDirectory() && !/\.(tmp|old)-/.test(entry.name)).length;
  } catch {
    return 0;
  }
}

export async function cache(args: string[], session: Session): Promise<number> {
  const [op, ...rest] = args;
  if (op === 'prune') return await upm(['prune', ...rest], session);
  if (op !== 'info') return usage('cache takes info or prune');

  const store = storeDir(process.env.UPM_STORE);
  const rows: [string, string][] = [
    ['store', store],
    ['packages', String(walkSize(path.join(store, 'index')).files)],
    ['shared', String(sharedCount(path.join(store, SHARED_DIR)))],
    ['files', size(walkSize(path.join(store, 'files')).bytes)],
    ['metadata', size(walkSize(metadataDir(store)).bytes)],
  ];
  for (const [label, value] of rows) out(`${color('gray', label.padEnd(8))}  ${value}\n`);
  return 0;
}
