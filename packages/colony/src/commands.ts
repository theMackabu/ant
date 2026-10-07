import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { basename, join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { loadColonyToml, findColonyToml, normalizeProjectName } from './config';
import { bundle, collectAssets, collectMigrations, type Asset, type Migration } from './build';
import { getProject, deployManifest, deleteProject, listProjects, missingAssets, uploadAsset } from './api';
import { sha256hex, styleText } from './utils';

function ensureDeps(): void {
  if (!existsSync('package.json')) return;
  let manifest: unknown;
  try {
    manifest = JSON.parse(readFileSync('package.json', 'utf-8'));
  } catch (error) {
    throw new Error(`could not read package.json: ${error instanceof Error ? error.message : String(error)}`);
  }
  if (typeof manifest !== 'object' || manifest === null || Array.isArray(manifest)) throw new Error('package.json must contain an object.');
  const deps = (manifest as { dependencies?: unknown }).dependencies;
  if (deps !== undefined && (typeof deps !== 'object' || deps === null || Array.isArray(deps)))
    throw new Error('package.json `dependencies` must contain an object.');
  if (!deps || Object.keys(deps).length === 0 || existsSync('node_modules')) return;

  console.log(styleText('dim', 'Installing deps from ants.land (antland install)…'));
  let result = spawnSync('antland', ['install'], { stdio: 'inherit' });
  if ((result.error as NodeJS.ErrnoException | undefined)?.code === 'ENOENT')
    result = spawnSync('npx', ['--yes', 'antland', 'install'], { stdio: 'inherit' });
  if (result.error || result.status !== 0) throw new Error('could not vendor deps — run `antland install` manually, then `colony deploy`.');
}

export async function deploy(): Promise<void> {
  const cfg = loadColonyToml();
  let script = '';
  let built = '';
  if (cfg.main === null) {
    console.log(`Deploying ${styleText('cyan', cfg.name)} ${styleText('dim', '(static site: files only, no script)')}…`);
    if (cfg.bindings.length) console.log(styleText('yellow', '  bindings are ignored: a static site runs no code'));
  } else {
    ensureDeps();
    console.log(`Building ${styleText('cyan', cfg.name)} ${styleText('dim', `(${cfg.main})`)}…`);
    const bytes = await bundle(cfg.main);
    script = new TextDecoder().decode(bytes);
    built = `  bundle ${bytes.byteLength} bytes · sha256 ${sha256hex(bytes).slice(0, 12)}`;
  }

  const migrations: Record<string, Migration[]> = {};
  for (const b of cfg.bindings) {
    if (b.kind === 'sql' && b.migrationsDir) {
      const m = collectMigrations(b.migrationsDir);
      if (m.length) migrations[b.binding] = m;
    }
  }

  let assets: Asset[] = [];
  if (cfg.assets) {
    assets = collectAssets(cfg.assets.directory);
    console.log(styleText('dim', `  ${assets.length} asset(s) from ${cfg.assets.directory}`));
  }
  if (built) console.log(styleText('dim', built));

  if (!(await getProject(cfg.name))) console.log(`Creating project ${styleText('cyan', cfg.name)}…`);
  for (const b of cfg.bindings) console.log(styleText('dim', `  bind env.${b.binding} -> ${b.kind} ${b.name}`));

  if (assets.length) await uploadAssets(assets);

  const r = await deployManifest(cfg.name, {
    script,
    observability: cfg.observability,
    vars: cfg.vars,
    bindings: cfg.main === null ? [] : cfg.bindings.map(b => ({ kind: b.kind, binding: b.binding, name: b.name })),
    migrations,
    assets: assets.map(a => ({ path: a.path, ct: a.ct, hash: a.hash })),
    assetsConfig: cfg.assets ? { notFound: cfg.assets.notFound, startAnt: cfg.assets.startAnt } : null
  });
  console.log();
  console.log(`${styleText('green', 'Deployed')} ${styleText('cyan', r.url)}`);
  console.log(styleText('dim', `  preview ${r.previewUrl}  ·  ${r.deployment.id}`));
}

// Uploads the assets the server doesn't have yet, a few at a time.
async function uploadAssets(assets: Asset[]): Promise<void> {
  const byHash = new Map(assets.map(a => [a.hash, a]));
  const missing = await missingAssets([...byHash.keys()]);
  if (!missing.length) {
    console.log(styleText('dim', `  assets unchanged, nothing to upload`));
    return;
  }
  let sent = 0;
  const raw = missing.reduce((n, h) => n + byHash.get(h)!.size, 0);
  const queue = [...missing];
  const worker = async () => {
    for (let h = queue.shift(); h; h = queue.shift()) {
      const n = await uploadAsset(h, readFileSync(byHash.get(h)!.file)); // not `sent += await`: that reads sent before the await
      sent += n;
    }
  };
  await Promise.all(Array.from({ length: Math.min(6, queue.length) }, worker));
  const kb = (n: number) => `${(n / 1024).toFixed(1)} KB`;
  console.log(styleText('dim', `  uploaded ${missing.length} of ${byHash.size} asset(s): ${kb(raw)} (${kb(sent)} sent)`));
}

export async function destroy(name?: string): Promise<void> {
  const target = name ?? (findColonyToml() ? loadColonyToml().name : undefined);
  if (!target) throw new Error('which project? Usage: colony delete <name> (or run in a dir with colony.toml).');
  await deleteProject(target);
  console.log(`${styleText('green', 'Deleted')} ${target}`);
}

export async function list(): Promise<void> {
  const projects = await listProjects();
  if (!projects.length) {
    console.log('No projects yet. Run `colony deploy` to create one.');
    return;
  }
  for (const p of projects) {
    console.log(`  ${styleText('cyan', p.name.padEnd(22))} ${p.active ?? styleText('dim', '(no deployment)')}`);
  }
}

export function init(name?: string): void {
  const p = join(process.cwd(), 'colony.toml');
  if (existsSync(p)) throw new Error('colony.toml already exists here.');
  const projName = normalizeProjectName(name ?? basename(process.cwd()));
  writeFileSync(
    p,
    `name = ${JSON.stringify(projName)}
main = "server.js"

[observability]
enabled = false

# [vars]
# GREETING = "hello"

# Bindings expose a store as env.<binding>. Stores are yours and named by
# \`name\` (default: the binding, lowercased); they're created on first deploy,
# and projects that bind the same name share the store.
# [[kv]]
# binding = "CACHE"
# name = "cache"

# [[sql]]
# binding = "DB"
# name = "app-db"
# migrations_dir = "schema"

# Files: your account's object storage (256 MB), shared by all your projects.
# env.FILES.put(key, body) / get(key) / head(key) / list({ prefix }) / delete(key)
# [[files]]
# binding = "FILES"

# A worker WITH [assets] serves static files; start_ant routes requests to your
# code (true = all, or a glob list like ["/api/*"]). Without it, it's script-only.
# [assets]
# directory = "./dist"
# not_found_handling = "single-page-application"
# start_ant = ["/api/*"]
`
  );
  console.log(`${styleText('green', 'Created')} colony.toml ${styleText('dim', `(name="${projName}")`)}`);
}
