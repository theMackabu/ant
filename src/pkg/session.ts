import { main as upmMain } from 'upm/src/cli.ts';

import { findProjectDir } from './project.ts';
import { useDefaultRegistry, useProjectStore } from './registry.ts';
import { loadConfig } from './settings.ts';
import type { PkgConfig } from './settings.ts';

export interface Session {
  settings: PkgConfig;
  dir?: string;
  verbose: boolean;
  projectStore: boolean;
}

export async function openSession(dirOption: string | undefined, verbose: boolean): Promise<Session> {
  const settings = loadConfig();
  const dir = await findProjectDir(dirOption);
  useDefaultRegistry(settings);
  const projectStore = useProjectStore(settings, dir);
  return { settings, dir, verbose, projectStore };
}

export const projectRoot = (session: Session) => session.dir ?? process.cwd();

export function upm(argv: string[], session: Session): Promise<number> {
  return upmMain(session.verbose ? ['--verbose', ...argv] : argv);
}
