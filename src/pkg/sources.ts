import { VALUED } from './args.ts';
import type { Source } from './settings.ts';

export interface Prefixed {
  args: string[];
  specs: string[];
  source?: Source;
}

export function stripSourcePrefixes(args: string[], valued = VALUED): Prefixed {
  const out: string[] = [];
  const specs: string[] = [];
  let source: Source | undefined;
  let rest = false;

  for (let i = 0; i < args.length; i++) {
    const arg = args[i]!;
    if (!rest && arg === '--') {
      rest = true;
      out.push(arg);
      continue;
    }
    if (!rest && arg.startsWith('-')) {
      out.push(arg);
      if (valued.has(arg) && i + 1 < args.length) out.push(args[++i]!);
      continue;
    }

    const prefix = arg.startsWith('land:') ? 'land' : arg.startsWith('npm:') ? 'npm' : undefined;
    const spec = prefix ? arg.slice(prefix.length + 1) : arg;
    if (prefix && !spec) throw new Error(`invalid package source in "${arg}"`);
    if (prefix && source && prefix !== source) throw new Error('land: and npm: packages cannot be mixed in one command');
    if (prefix) source = prefix;
    out.push(spec);
    specs.push(spec);
  }

  return { args: out, specs, source };
}
