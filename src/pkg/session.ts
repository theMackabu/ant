import { main as upmMain } from 'upm/src/cli.ts';

import { afterInstall } from './lifecycle/report.ts';
import { migrateLockb } from './lockb/migrate.ts';
import { findProjectDir } from './project.ts';
import { applyRegistry, applyStore } from './registry.ts';
import { loadConfig } from './settings.ts';
import type { PkgConfig } from './settings.ts';
import { note } from './ui/output.ts';

export interface Session {
  settings: PkgConfig;
  dir?: string;
  verbose: boolean;
}

export function openSession(args: string[]): Session {
  const settings = loadConfig();
  const dir = findProjectDir(args);
  applyRegistry(settings);
  applyStore(settings, dir);
  return { settings, dir, verbose: process.env.ANT_PKG_VERBOSE === '1' };
}

export const projectRoot = (session: Session) => session.dir ?? process.cwd();

export function upm(argv: string[], session: Session): Promise<number> {
  return upmMain(session.verbose ? ['--verbose', ...argv] : argv);
}

export async function migrate(session: Session): Promise<void> {
  if (session.dir) await migrateLockb(session.dir, message => note(message));
}

export async function finish(code: number, session: Session): Promise<number> {
  if (code !== 0 || !session.dir) return code;
  return await afterInstall(session.dir, session.verbose);
}
