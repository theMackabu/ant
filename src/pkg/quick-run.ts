import { setProgramName } from 'upm/src/cli.ts';
import { readConfig } from 'upm/src/config.ts';
import { treeStanding } from 'upm/src/link.ts';
import { LOCKFILE } from 'upm/src/lock.ts';
import { parseManifest } from 'upm/src/package-json.ts';
import { currentPlatform } from 'upm/src/resolve.ts';
import { readScripts, runScript, shellLine } from 'upm/src/run.ts';
import { readState, sameStamp, settingsOf, stampOf } from 'upm/src/state.ts';
import type { InstallState, Stamp, TarballStamp } from 'upm/src/state.ts';
import { findRoot, workspacesOf } from 'upm/src/workspaces.ts';
import type { Root } from 'upm/src/workspaces.ts';

import fs from 'node:fs';
import path from 'node:path';

import { useAntHome } from './paths.ts';
import { useDefaultRegistry } from './registry.ts';
import { loadConfig } from './settings.ts';
import { printBanner } from './ui/banner.ts';
import { note } from './ui/output.ts';

const PNPM_WORKSPACE = 'pnpm-workspace.yaml';

function sameFiles(dir: string, files: Record<string, TarballStamp | null>): boolean {
  return Object.entries(files).every(([source, stamp]) => {
    const at = path.resolve(dir, source.slice('file:'.length));
    return stamp?.[4] !== undefined && sameStamp(stampOf(at), stamp.slice(0, 4) as Stamp);
  });
}

function sameLinks(dir: string, links: InstallState['links'] = {}): boolean {
  return Object.entries(links).every(([at, stamp]) => {
    const now = stampOf(path.join(dir, at, 'package.json'));
    return stamp === null ? now === undefined : sameStamp(now, stamp);
  });
}

function declares(manifest: NonNullable<Root['manifest']>): boolean {
  const groups = [manifest.dependencies, manifest.devDependencies, manifest.optionalDependencies];
  return groups.some(group => group && Object.keys(group).length > 0);
}

async function installed(root: Root): Promise<boolean> {
  const { dir, manifest } = root;
  if (!manifest || fs.existsSync(path.join(dir, PNPM_WORKSPACE))) return false;

  const state = await readState(dir);
  if (!state) return !declares(manifest) && manifest.workspaces === undefined;

  const { stamps, tarballs } = state;
  if (state.inputs === undefined || !stamps || !tarballs || stamps.pnpm !== undefined) return false;
  if (!sameStamp(stampOf(path.join(dir, LOCKFILE)), stamps.lock)) return false;
  if (!sameStamp(stampOf(path.join(dir, 'package.json')), stamps.manifest)) return false;

  const { registry, scopes, fallback, hoist } = readConfig(dir);
  const hosts = fallback ? [registry, scopes, fallback] : [registry, scopes];
  const production = state.production === true;
  if (settingsOf({ production, store: state.store, hoist, hosts, platform: currentPlatform() }) !== stamps.settings) return false;

  const listed = root.listed ?? (await workspacesOf(dir, manifest, true));
  if (!listed.proven || listed.learned || !state.summary) return false;
  if (!sameFiles(dir, tarballs) || !sameLinks(dir, state.links) || !treeStanding(dir, state, hoist)) return false;
  for (const warning of state.summary.warnings) note(warning, 'warn');
  return true;
}

export async function quickRun(argv: string[]): Promise<number | undefined> {
  const [verb, name, ...rest] = argv;
  if (verb !== 'run' || !name || name.startsWith('-')) return undefined;

  const dir = process.cwd();
  const file = path.join(dir, 'package.json');
  let manifest: ReturnType<typeof parseManifest>;
  let scripts: Record<string, string>;
  try {
    manifest = parseManifest(fs.readFileSync(file, 'utf8'), file);
    scripts = readScripts(manifest, file);
  } catch {
    return undefined;
  }
  if (!Object.hasOwn(scripts, name)) return undefined;

  process.env.npm_config_user_agent = `ant/${process.versions.ant}`;
  setProgramName('ant', 'magenta');
  useAntHome();
  useDefaultRegistry(loadConfig());

  const root = await findRoot(dir).catch(() => undefined);
  if (!root || !(await installed(root).catch(() => false))) return undefined;

  const command = scripts[name]!;
  const scriptArgs = rest[0] === '--' ? rest.slice(1) : rest;
  printBanner({ script: name, line: shellLine(command, scriptArgs), workspace: undefined });
  return await runScript({ dir, file, name, command, args: scriptArgs, pkg: manifest });
}
