import * as path from 'node:path';
import * as fs from 'node:fs';
import * as util from 'node:util';
import { spawn } from 'node:child_process';
import type { PkgManagerName } from './pkg_manager';

export let DEBUG = false;

export function setDebug(enabled: boolean) {
  DEBUG = enabled;
}

export function logDebug(msg: string) {
  if (DEBUG) console.log(msg);
}

const SCOPED = /^@([a-z0-9][a-z0-9-]*)\/([a-z0-9][a-z0-9-]*)(?:@(.+))?$/;
const BARE = /^([a-z0-9][a-z0-9-]*)(?:@(.+))?$/;

export class AntPackageNameError extends Error {}

export class AntPackage {
  scope: string; // '' when unscoped
  name: string;
  version: string | null;

  private constructor(scope: string, name: string, version: string | null) {
    this.scope = scope;
    this.name = name;
    this.version = version;
  }

  static from(input: string): AntPackage {
    const scoped = input.match(SCOPED);
    if (scoped) return new AntPackage(scoped[1], scoped[2], scoped[3] ?? null);

    const bare = input.match(BARE);
    if (bare) return new AntPackage('', bare[1], bare[2] ?? null);

    throw new AntPackageNameError(`Invalid package name: expected "@scope/name" or "name" (optionally "@version"), got "${input}"`);
  }

  get scoped(): boolean {
    return this.scope !== '';
  }

  get id(): string {
    return this.scope ? `@${this.scope}/${this.name}` : this.name;
  }

  toString(): string {
    return this.id + (this.version !== null ? `@${this.version}` : '');
  }
}

export async function fileExists(file: string): Promise<boolean> {
  try {
    return (await fs.promises.stat(file)).isFile();
  } catch {
    return false;
  }
}

export interface PkgJson {
  name?: string;
  version?: string;
  workspaces?: string[];
  scripts?: Record<string, string>;
}

export async function readJson<T>(file: string): Promise<T> {
  return JSON.parse(await fs.promises.readFile(file, 'utf-8')) as T;
}

export interface ProjectInfo {
  projectDir: string;
  pkgManagerName: PkgManagerName | null;
  pkgJsonPath: string | null;
  root: string | null;
}

export async function findProjectDir(
  cwd: string,
  dir: string = cwd,
  result: ProjectInfo = { projectDir: cwd, pkgManagerName: null, pkgJsonPath: null, root: null }
): Promise<ProjectInfo> {
  if (result.pkgJsonPath === null) {
    const pkgJsonPath = path.join(dir, 'package.json');
    if (await fileExists(pkgJsonPath)) {
      result.projectDir = dir;
      result.pkgJsonPath = pkgJsonPath;
    }
  } else {
    const pkgJsonPath = path.join(dir, 'package.json');
    if (await fileExists(pkgJsonPath)) {
      const json = await readJson<PkgJson>(pkgJsonPath);
      if (Array.isArray(json.workspaces)) result.root = dir;
      else if (await fileExists(path.join(dir, 'pnpm-workspace.yaml'))) result.root = dir;
    }
  }

  const lockfiles: [string, PkgManagerName][] = [
    ['package-lock.json', 'npm'],
    ['bun.lockb', 'bun'],
    ['bun.lock', 'bun'],
    ['yarn.lock', 'yarn'],
    ['pnpm-lock.yaml', 'pnpm']
  ];
  for (const [file, name] of lockfiles) {
    if (await fileExists(path.join(dir, file))) {
      logDebug(`Detected ${name} from lockfile ${file}`);
      result.pkgManagerName = name;
      return result;
    }
  }

  const prev = dir;
  dir = path.dirname(dir);
  if (dir === prev) return result;
  return findProjectDir(cwd, dir, result);
}

const PERIODS = {
  year: 365 * 24 * 60 * 60 * 1000,
  month: 30 * 24 * 60 * 60 * 1000,
  week: 7 * 24 * 60 * 60 * 1000,
  day: 24 * 60 * 60 * 1000,
  hour: 60 * 60 * 1000,
  minute: 60 * 1000,
  second: 1000
};

export function prettyTime(diff: number): string {
  if (diff > PERIODS.day) return `${Math.floor(diff / PERIODS.day)}d`;
  if (diff > PERIODS.hour) return `${Math.floor(diff / PERIODS.hour)}h`;
  if (diff > PERIODS.minute) return `${Math.floor(diff / PERIODS.minute)}m`;
  if (diff > PERIODS.second) return `${Math.floor(diff / PERIODS.second)}s`;
  return `${diff}ms`;
}

export function timeAgo(diff: number): string {
  for (const [unit, ms] of Object.entries(PERIODS)) {
    if (diff > ms) {
      const v = Math.floor(diff / ms);
      return `${v} ${unit}${v > 1 ? 's' : ''} ago`;
    }
  }
  return 'just now';
}

export function getNewLineChars(source: string): string {
  const i = source.indexOf('\n');
  return source[i - 1] === '\r' ? '\r\n' : '\n';
}

export class ExecError extends Error {
  code: number;
  constructor(code: number) {
    super(`Child process exited with: ${code}`);
    this.code = code;
  }
}

export interface ExecOutput {
  combined: string;
  stdout: string;
  stderr: string;
}

export async function exec(
  cmd: string,
  args: string[],
  cwd: string,
  env?: Record<string, string | undefined>,
  captureOutput?: boolean
): Promise<ExecOutput> {
  const cp = spawn(
    cmd,
    args.map(arg => (process.platform === 'win32' ? `"${arg}"` : `'${arg}'`)),
    { stdio: captureOutput ? 'pipe' : 'inherit', cwd, shell: true, env }
  );

  let combined = '';
  let stdout = '';
  let stderr = '';
  if (captureOutput) {
    cp.stdout?.on('data', d => {
      combined += d;
      stdout += d;
    });
    cp.stderr?.on('data', d => {
      combined += d;
      stderr += d;
    });
  }

  return new Promise<ExecOutput>((resolve, reject) => {
    cp.on('exit', code => {
      if (code === 0) resolve({ combined, stdout, stderr });
      else reject(new ExecError(code ?? 1));
    });
  });
}

type StyleColor = 'red' | 'green' | 'cyan' | 'magenta' | 'dim' | 'yellow';
export const styleText: (style: StyleColor, text: string) => string =
  typeof (util as { styleText?: unknown }).styleText === 'function'
    ? (util as unknown as { styleText: (s: string, t: string) => string }).styleText
    : (_style, text) => text;
