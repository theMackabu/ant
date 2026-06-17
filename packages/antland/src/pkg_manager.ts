import semiver from 'semiver';
import { exec, findProjectDir, logDebug, styleText } from './utils';

export type InstallMode = 'dev' | 'prod' | 'optional';

async function execWithLog(cmd: string, args: string[], cwd: string) {
  console.log(styleText('dim', `$ ${cmd} ${args.join(' ')}`));
  return exec(cmd, args, cwd);
}

function modeToFlag(mode: InstallMode): string[] {
  return mode === 'dev' ? ['--save-dev'] : mode === 'optional' ? ['--save-optional'] : [];
}
function modeToFlagYarn(mode: InstallMode): string[] {
  return mode === 'dev' ? ['--dev'] : mode === 'optional' ? ['--optional'] : [];
}

export interface PackageManager {
  cwd: string;
  install(specs: string[], mode: InstallMode): Promise<void>;
  remove(ids: string[]): Promise<void>;
  runScript(script: string): Promise<void>;
  publish(args: string[]): Promise<void>;
  setConfigValue?(key: string, value: string): Promise<void>;
}

class Npm implements PackageManager {
  constructor(public cwd: string) {}
  async install(specs: string[], mode: InstallMode) {
    await execWithLog('npm', ['install', ...modeToFlag(mode), ...specs], this.cwd);
  }
  async remove(ids: string[]) {
    await execWithLog('npm', ['remove', ...ids], this.cwd);
  }
  async runScript(script: string) {
    await execWithLog('npm', ['run', script], this.cwd);
  }
  async publish(args: string[]) {
    await execWithLog('npm', ['publish', ...args], this.cwd);
  }
}

class Yarn implements PackageManager {
  constructor(public cwd: string) {}
  async install(specs: string[], mode: InstallMode) {
    await execWithLog('yarn', ['add', ...modeToFlagYarn(mode), ...specs], this.cwd);
  }
  async remove(ids: string[]) {
    await execWithLog('yarn', ['remove', ...ids], this.cwd);
  }
  async runScript(script: string) {
    await execWithLog('yarn', [script], this.cwd);
  }
  async publish(args: string[]) {
    await execWithLog('yarn', ['npm', 'publish', ...args], this.cwd);
  }
}

export class YarnBerry extends Yarn {
  async setConfigValue(key: string, value: string) {
    await execWithLog('yarn', ['config', 'set', key, value], this.cwd);
  }
}

class Pnpm implements PackageManager {
  constructor(public cwd: string) {}
  async install(specs: string[], mode: InstallMode) {
    await execWithLog('pnpm', ['add', ...modeToFlag(mode), ...specs], this.cwd);
  }
  async remove(ids: string[]) {
    await execWithLog('pnpm', ['remove', ...ids], this.cwd);
  }
  async runScript(script: string) {
    await execWithLog('pnpm', [script], this.cwd);
  }
  async publish(args: string[]) {
    await execWithLog('pnpm', ['publish', ...args], this.cwd);
  }
}

export class Bun implements PackageManager {
  constructor(public cwd: string) {}
  async install(specs: string[], mode: InstallMode) {
    await execWithLog('bun', ['add', ...modeToFlagYarn(mode), ...specs], this.cwd);
  }
  async remove(ids: string[]) {
    await execWithLog('bun', ['remove', ...ids], this.cwd);
  }
  async runScript(script: string) {
    await execWithLog('bun', ['run', script], this.cwd);
  }
  async publish(args: string[]) {
    await execWithLog('npm', ['publish', ...args], this.cwd); // bun has no publish to a custom registry
  }
  async isNpmrcSupported() {
    const { stdout } = await exec('bun', ['--version'], this.cwd, undefined, true);
    return stdout != null && semiver(stdout.trim(), '1.1.18') >= 0; // npmrc support since 1.1.18
  }
}

export type PkgManagerName = 'npm' | 'yarn' | 'pnpm' | 'bun';

function fromUserAgent(value: string): PkgManagerName | null {
  if (value.startsWith('pnpm/')) return 'pnpm';
  if (value.startsWith('yarn/')) return 'yarn';
  if (value.startsWith('npm/')) return 'npm';
  if (value.startsWith('bun/')) return 'bun';
  return null;
}

async function isYarnBerry(cwd: string): Promise<boolean> {
  const { stdout } = await exec('yarn', ['--version'], cwd, undefined, true);
  if (!stdout) return false;
  return !stdout.trim().startsWith('1.');
}

export async function getPkgManager(cwd: string, pkgManagerName: PkgManagerName | null): Promise<{ root: string; pkgManager: PackageManager }> {
  const ua = process.env.npm_config_user_agent;
  const fromEnv = ua ? fromUserAgent(ua) : null;
  const { projectDir, pkgManagerName: fromLockfile, root } = await findProjectDir(cwd);
  const rootPath = root || projectDir;
  const chosen = pkgManagerName || fromLockfile || fromEnv || 'npm';
  logDebug(`Using package manager: ${chosen}`);

  let pkgManager: PackageManager;
  if (chosen === 'yarn') pkgManager = (await isYarnBerry(projectDir)) ? new YarnBerry(projectDir) : new Yarn(projectDir);
  else if (chosen === 'pnpm') pkgManager = new Pnpm(projectDir);
  else if (chosen === 'bun') pkgManager = new Bun(projectDir);
  else pkgManager = new Npm(projectDir);

  return { root: rootPath, pkgManager };
}
