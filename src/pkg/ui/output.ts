import { fail, help, note, paint, usage, write } from 'upm/src/cli.ts';
import type { Output, Style } from 'upm/src/cli.ts';

export { fail, help, note, usage, write };

export const out = (text: string) => write('stdout', text);
export const color = (style: Style, text: string, to: Output = 'stdout') => paint(style, text, to);

export function plural(n: number, one: string, many = `${one}s`): string {
  return `${n} ${n === 1 ? one : many}`;
}

export function duration(ms: number): string {
  return ms < 1000 ? `${Math.round(ms)}ms` : `${(ms / 1000).toFixed(2)}s`;
}

export function size(bytes: number): string {
  const kb = bytes / 1000;
  return kb < 1000 ? `${kb.toFixed(1)} kB` : `${(kb / 1000).toFixed(1)} MB`;
}

export function summary(verb: string, parts: string[]): void {
  out(`${color('green', `✓ ${verb}`)} ${color('gray', `· ${parts.join(' · ')}`)}\n`);
}

export function list(rows: [string, string][], indent = ''): string {
  return rows.map(([name, detail]) => `${indent}${color('cyan', name)}\n${indent}  ${color('gray', detail)}\n`).join('');
}
