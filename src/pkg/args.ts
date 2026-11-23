import type { Source } from './settings.ts';

const VALUED = new Set([
  '-w', '--workspace', '--registry', '--dir', '-C', '--prefix', '--store', '-p', '--package', '-c', '--call',
  '--before', '--min-release-age', '--min-release-age-exclude', '--omit', '--include', '--loglevel',
]);

const DIR_FLAGS = new Set(['--dir', '-C', '--prefix']);
const GLOBAL_FLAGS = new Set(['-g', '--global']);
const ALL_FLAGS = new Set(['-a', '--all']);

const SOURCE_PREFIXES: [string, Source][] = [
  ['ants.land:', 'land'],
  ['land:', 'land'],
  ['npm:', 'npm'],
];

export class UsageError extends Error {}

export interface Spec {
  spec: string;
  source?: Source;
}

export interface Args {
  specs: Spec[];
  flags: string[];
  dir?: string;
  global: boolean;
  all: boolean;
}

export const isHelp = (args: string[]) => args.includes('-h') || args.includes('--help');

export function parseSource(token: string): Spec {
  const prefix = SOURCE_PREFIXES.find(([name]) => token.startsWith(name));
  if (!prefix) return { spec: token };

  const [name, source] = prefix;
  const spec = token.slice(name.length);
  if (!spec) throw new UsageError(`invalid package source in "${token}"`);
  return { spec, source };
}

export function parseArgs(args: string[]): Args {
  const parsed: Args = { specs: [], flags: [], global: false, all: false };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i]!;
    if (arg === '--') {
      parsed.specs.push(...args.slice(i + 1).map(spec => ({ spec })));
      break;
    }

    if (!arg.startsWith('-')) parsed.specs.push(parseSource(arg));
    else if (GLOBAL_FLAGS.has(arg)) parsed.global = true;
    else if (ALL_FLAGS.has(arg)) parsed.all = true;
    else {
      const eq = arg.indexOf('=');
      const flag = eq < 0 ? arg : arg.slice(0, eq);
      let value = eq < 0 ? undefined : arg.slice(eq + 1);
      parsed.flags.push(arg);
      if (eq < 0 && VALUED.has(flag) && ++i < args.length) parsed.flags.push((value = args[i]!));
      if (DIR_FLAGS.has(flag) && value !== undefined) parsed.dir = value;
    }
  }

  return parsed;
}

export function splitKey(key: string): { name: string; version: string } {
  const at = key.indexOf('@', 1);
  return at < 0 ? { name: key, version: '' } : { name: key.slice(0, at), version: key.slice(at + 1) };
}

export const nameOfSpec = (spec: string) => splitKey(spec).name;
