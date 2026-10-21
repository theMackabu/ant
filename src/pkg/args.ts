export const VALUED = new Set([
  '-w', '--workspace', '--registry', '--dir', '-C', '--prefix', '--store', '-p', '--package', '-c', '--call',
  '--before', '--min-release-age', '--min-release-age-exclude', '--omit', '--include', '--loglevel',
]);

export const isHelp = (args: string[]) => args.includes('-h') || args.includes('--help');
export const isGlobal = (args: string[]) => args.includes('-g') || args.includes('--global');
export const withoutGlobal = (args: string[]) => args.filter(arg => arg !== '-g' && arg !== '--global');
export const flagsOf = (args: string[]) => args.filter(arg => arg.startsWith('-'));

export function positionals(argv: string[], valued = VALUED): string[] {
  const out: string[] = [];
  
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i]!;
    
    if (arg === '--') {
      out.push(...argv.slice(i + 1));
      break;
    }
    
    if (arg.startsWith('-')) {
      if (valued.has(arg)) i++;
      continue;
    }
    
    out.push(arg);
  }
  
  return out;
}

export function nameOfSpec(spec: string): string {
  const at = spec.indexOf('@', spec.startsWith('@') ? 1 : 0);
  return at < 0 ? spec : spec.slice(0, at);
}

export function splitKey(key: string): { name: string; version: string } {
  const at = key.indexOf('@', key.startsWith('@') ? 1 : 0);
  return { name: key.slice(0, at), version: key.slice(at + 1) };
}
