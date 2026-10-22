import { setShareFilter } from 'upm/src/link.ts';
import type { FileEntry } from 'upm/src/store.ts';

import { buildScripts } from './lifecycle/discover.ts';
import type { Manifest } from './project.ts';

const SCRIPT = /\.[cm]?js$/;
const NATIVE = /\.node$/;
const SPECIFIER = /\b(?:require\s*\(\s*|import\s*\(\s*|from\s*|import\s+)(['"])([^'"\s]+)\1/g;
const PACKAGE_NAME = /^(?:@[a-z0-9~-][a-z0-9._~-]*\/)?[a-z0-9~-][a-z0-9._~-]*$/;

const NODE_BUILTINS = new Set([
  'assert', 'async_hooks', 'buffer', 'child_process', 'cluster', 'console', 'constants', 'crypto',
  'dgram', 'diagnostics_channel', 'dns', 'domain', 'events', 'fs', 'http', 'http2', 'https',
  'inspector', 'module', 'net', 'os', 'path', 'perf_hooks', 'process', 'punycode', 'querystring',
  'readline', 'repl', 'stream', 'string_decoder', 'sys', 'timers', 'tls', 'trace_events', 'tty',
  'url', 'util', 'v8', 'vm', 'wasi', 'worker_threads', 'zlib',
]);

function packageName(specifier: string): string | undefined {
  const parts = specifier.split('/');
  const name = specifier.startsWith('@') ? parts.slice(0, 2).join('/') : parts[0]!;
  return PACKAGE_NAME.test(name) && name !== 'node_modules' ? name : undefined;
}

function importsOthers(manifest: Manifest, name: string, files: readonly FileEntry[], read: (file: FileEntry) => string): boolean {
  const dev = manifest.devDependencies ?? {};
  for (const file of files) {
    if (!SCRIPT.test(file.path)) continue;
    for (const match of read(file).matchAll(SPECIFIER)) {
      const imported = packageName(match[2]!);
      if (!imported || imported === name || NODE_BUILTINS.has(imported) || Object.hasOwn(dev, imported)) continue;
      return true;
    }
  }
  return false;
}

function platformBound(manifest: Manifest, files: readonly FileEntry[]): boolean {
  const { os, cpu } = manifest as { os?: unknown; cpu?: unknown };
  return os !== undefined || cpu !== undefined || files.some(file => NATIVE.test(file.path));
}

export function shareSelfContained(): void {
  setShareFilter((pkg, raw, files, read) => {
    const manifest = raw as Manifest;
    const hasGyp = () => files.some(file => file.path === 'binding.gyp');
    if (buildScripts(manifest, hasGyp).length > 0) return false;
    if (platformBound(manifest, files)) return false;
    return !importsOthers(manifest, pkg.name, files, read);
  });
}
