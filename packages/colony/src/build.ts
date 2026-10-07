import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { extname, join } from 'node:path';
import { rolldown, type Plugin } from 'rolldown';
import { sha256hex } from './utils';

const antPlugin: Plugin = {
  name: 'ant-platform',
  resolveId(source) {
    if (/^(node:|ant:)/.test(source)) return { id: source, external: true };
    return null;
  }
};

export async function bundle(entry: string): Promise<Uint8Array> {
  if (!existsSync(entry)) throw new Error(`entry not found: ${entry}`);
  const build = await rolldown({ input: entry, plugins: [antPlugin], logLevel: 'silent' });
  try {
    const result = await build.generate({ format: 'es', minify: true });
    const chunk = result.output.find(o => o.type === 'chunk');
    if (!chunk || !('code' in chunk)) throw new Error('rolldown produced no output chunk');
    return new TextEncoder().encode(chunk.code);
  } finally {
    await build.close();
  }
}

const MIME: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.webp': 'image/webp',
  '.ico': 'image/x-icon',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.txt': 'text/plain; charset=utf-8',
  '.map': 'application/json; charset=utf-8'
};

// A static file, named by the sha256 of its bytes. The bytes stay on disk until
// an upload needs them: the server is asked which hashes it's missing first.
export interface Asset {
  path: string;
  ct: string;
  hash: string;
  size: number;
  file: string;
}

const ASSET_LIMIT = 25 * 1024 * 1024; // the server's per-file cap

export function collectAssets(dir: string): Asset[] {
  if (!existsSync(dir)) throw new Error(`assets directory not found: ${dir}`);
  if (!statSync(dir).isDirectory()) throw new Error(`assets path is not a directory: ${dir}`);
  const out: Asset[] = [];
  const walk = (abs: string, rel: string) => {
    const entries = readdirSync(abs, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name));
    for (const entry of entries) {
      const assetPath = join(abs, entry.name);
      const urlPath = `${rel}/${entry.name}`;
      if (entry.isSymbolicLink()) throw new Error(`asset symlinks are not supported: ${urlPath}`);
      if (entry.isDirectory()) {
        walk(assetPath, urlPath);
        continue;
      }
      if (!entry.isFile()) continue;
      const size = statSync(assetPath).size;
      if (size > ASSET_LIMIT) throw new Error(`asset too large (>${ASSET_LIMIT} bytes): ${urlPath}`);
      out.push({
        path: urlPath,
        ct: MIME[extname(entry.name).toLowerCase()] || 'application/octet-stream',
        hash: sha256hex(readFileSync(assetPath)),
        size,
        file: assetPath
      });
    }
  };
  walk(dir, '');
  return out;
}

export interface Migration {
  tag: string;
  sql: string;
}

export function collectMigrations(dir: string): Migration[] {
  if (!existsSync(dir)) throw new Error(`migrations directory not found: ${dir}`);
  if (!statSync(dir).isDirectory()) throw new Error(`migrations path is not a directory: ${dir}`);
  return readdirSync(dir, { withFileTypes: true })
    .filter(entry => entry.isFile() && entry.name.endsWith('.sql'))
    .sort((a, b) => a.name.localeCompare(b.name))
    .map(entry => ({ tag: entry.name.replace(/\.sql$/, ''), sql: readFileSync(join(dir, entry.name), 'utf-8') }));
}
