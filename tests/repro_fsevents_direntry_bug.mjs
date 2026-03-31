// Repro attempt for the watcher-side `dirObj.has(...)` failure without Vite.
//
// Run with:
//   ant tests/repro_fsevents_direntry_bug.mjs
//
// This uses only `fsevents` plus a tiny local watcher shim that mirrors the
// chokidar/Vite code path around:
//   const dirObj = this.fsw._getWatchedDir(dirname(pp));
//   if (dirObj.has(base)) return;

import fs from 'node:fs';
import { EventEmitter } from 'node:events';
import path from 'node:path';
import fsevents from '/Users/themackabu/.ant/pkg/exec/vite/node_modules/fsevents/fsevents.js';

class DirEntry {
  constructor(dir) {
    this.path = dir;
    this.items = new Set();
  }

  add(item) {
    const { items } = this;
    if (!items) return;
    if (item !== '.' && item !== '..') items.add(item);
  }

  has(item) {
    const { items } = this;
    if (!items) return;
    return items.has(item);
  }
}

class MiniWatcher {
  constructor(root) {
    this.root = path.resolve(root);
    this._watched = new Map();
  }

  _getWatchedDir(directory) {
    const dir = path.resolve(directory);
    if (!this._watched.has(dir)) {
      this._watched.set(dir, new DirEntry(dir));
    }
    return this._watched.get(dir);
  }

  emitAdd(newPath, stats) {
    const pp = path.resolve(newPath);
    const isDir = stats.isDirectory();
    const dirObj = this._getWatchedDir(path.dirname(pp));
    const base = path.basename(pp);

    if (isDir) this._getWatchedDir(pp);

    console.log('[emitAdd]', {
      pp,
      base,
      dir: dirObj.path,
      hasType: typeof dirObj.has,
      addType: typeof dirObj.add,
      itemsType: typeof dirObj.items,
      itemsHasType: typeof dirObj.items?.has,
    });

    if (dirObj.has(base)) {
      console.log('[emitAdd] already present', base);
      return;
    }

    dirObj.add(base);
    console.log('[emitAdd] added', base);
  }
}

function statSafe(file) {
  try {
    return fs.statSync(file);
  } catch (error) {
    console.log('[stat error]', file, error?.name, error?.message);
    return null;
  }
}

async function walkEntries(root, emitEntry) {
  const pending = [root];
  const seen = new Set();

  while (pending.length > 0) {
    const dir = pending.pop();
    const resolvedDir = path.resolve(dir);
    if (seen.has(resolvedDir)) continue;
    seen.add(resolvedDir);
    let entries;

    try {
      entries = await fs.promises.readdir(resolvedDir);
    } catch (error) {
      console.log('[readdir error]', resolvedDir, error?.name, error?.message);
      continue;
    }

    for (const name of entries) {
      const fullPath = path.join(resolvedDir, name);
      let stats;

      try {
        stats = await fs.promises.lstat(fullPath);
      } catch (error) {
        console.log('[lstat error]', fullPath, error?.name, error?.message);
        continue;
      }

      await emitEntry({
        path: path.relative(root, fullPath),
        fullPath,
        stats,
      });

      if (stats.isDirectory()) pending.push(fullPath);
    }
  }
}

function makeReaddirpLikeStream(root) {
  const stream = new EventEmitter();

  queueMicrotask(async () => {
    try {
      await walkEntries(root, async (entry) => {
        stream.emit('data', entry);
      });
      stream.emit('end');
    } catch (error) {
      stream.emit('error', error);
    }
  });

  return stream;
}

const root = process.cwd();
const watcher = new MiniWatcher(root);
const probeFile = path.join(root, 'tmp', 'fsevents-direntry-probe.txt');

process.on('unhandledRejection', (reason) => {
  console.log('[unhandledRejection]', reason?.name, reason?.message);
  if (reason?.stack) console.log(reason.stack);
});

process.on('uncaughtException', (error) => {
  console.log('[uncaughtException]', error?.name, error?.message);
  if (error?.stack) console.log(error.stack);
  process.exitCode = 1;
});

const stop = fsevents.watch(root, (fullPath, flags, id) => {
  const info = fsevents.getInfo(fullPath, flags, id);
  const target = path.resolve(fullPath);

  console.log('[fsevent]', {
    fullPath: target,
    event: info.event,
    type: info.type,
  });

  const stats = statSafe(target);
  if (!stats) return;

  try {
    watcher.emitAdd(target, stats);
  } catch (error) {
    console.log('[emitAdd throw]', error?.name, error?.message);
    if (error?.stack) console.log(error.stack);
  }
});

console.log('watching', root);

fs.mkdirSync(path.dirname(probeFile), { recursive: true });
fs.writeFileSync(probeFile, `probe ${Date.now()}\n`);
console.log('wrote probe file', probeFile);

const stream = makeReaddirpLikeStream(root);
stream.on('data', (entry) => {
  console.log('[stream:data]', entry.path);

  try {
    watcher.emitAdd(entry.fullPath, entry.stats);
  } catch (error) {
    console.log('[stream emitAdd throw]', error?.name, error?.message);
    if (error?.stack) console.log(error.stack);
  }
});
stream.on('error', (error) => {
  console.log('[stream:error]', error?.name, error?.message);
  if (error?.stack) console.log(error.stack);
});
stream.on('end', () => {
  console.log('[stream:end]');
});

setTimeout(() => {
  try {
    fs.appendFileSync(probeFile, 'update\n');
    console.log('updated probe file', probeFile);
  } catch (error) {
    console.log('[appendFileSync throw]', error?.name, error?.message);
    if (error?.stack) console.log(error.stack);
  }
}, 250);

await new Promise((resolve) => setTimeout(resolve, 1500));
await stop();
console.log('stopped');
