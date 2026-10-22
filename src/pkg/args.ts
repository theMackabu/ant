import type { Source } from './settings.ts';

const VALUED = new Set([
  '-w', '--workspace', '--registry', '--dir', '-C', '--prefix', '--store', '-p', '--package', '-c', '--call',
  '--before', '--min-release-age', '--min-release-age-exclude', '--omit', '--include', '--loglevel',
]);

const DIR_FLAGS = new Set(['--dir', '-C', '--prefix']);
const GLOBAL_FLAGS = new Set(['-g', '--global']);
const ALL_FLAGS = new Set(['-a', '--all']);

export interface Args {
  specs: string[];
  flags: string[];
  source?: Source;
  dir?: string;
  global: boolean;
  all: boolean;
}

export const isHelp = (args: string[]) => args.includes('-h') || args.includes('--help');

export function parseSource(token: string): { spec: string; source?: Source } {
  const source = token.startsWith('land:') ? 'land' : token.startsWith('npm:') ? 'npm' : undefined;
  if (!source) return { spec: token };
  const spec = token.slice(source.length + 1);
  if (!spec) throw new Error(`invalid package source in "${token}"`);
  return { spec, source };
}

export function parseArgs(args: string[]): Args {
  const parsed: Args = { specs: [], flags: [], global: false, all: false };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i]!;
    if (arg === '--') {
      parsed.specs.push(...args.slice(i + 1));
      break;
    }

    if (!arg.startsWith('-')) {
      const { spec, source } = parseSource(arg);
      if (source && parsed.source && source !== parsed.source) {
        throw new Error('land: and npm: packages cannot be mixed in one command');
      }
      parsed.source ??= source;
      parsed.specs.push(spec);
      continue;
    }

    if (GLOBAL_FLAGS.has(arg)) parsed.global = true;
    else if (ALL_FLAGS.has(arg)) parsed.all = true;
    else {
      const eq = arg.indexOf('=');
      const flag = eq < 0 ? arg : arg.slice(0, eq);
      const value = eq < 0 && VALUED.has(flag) ? args[++i] : eq < 0 ? undefined : arg.slice(eq + 1);
      if (DIR_FLAGS.has(flag) && value !== undefined) parsed.dir = value;
      parsed.flags.push(arg);
      if (eq < 0 && VALUED.has(flag) && value !== undefined) parsed.flags.push(value);
    }
  }

  return parsed;
}

export function nameOfSpec(spec: string): string {
  const at = spec.indexOf('@', spec.startsWith('@') ? 1 : 0);
  return at < 0 ? spec : spec.slice(0, at);
}

export function splitKey(key: string): { name: string; version: string } {
  const at = key.indexOf('@', key.startsWith('@') ? 1 : 0);
  return { name: key.slice(0, at), version: key.slice(at + 1) };
}
